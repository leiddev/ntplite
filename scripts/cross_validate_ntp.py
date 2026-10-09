#!/usr/bin/env python3
"""Cross-validate ntplite against an independent implementation of NTP.

The C++ test suite checks the library against a mock server that is built on the
library's own packet codec, so a misunderstanding of RFC 5905 would be shared by
both sides and go unnoticed.  This script closes that hole: it speaks the
protocol with `struct` and arithmetic of its own, and it drives the compiled
`ntplite` command line tool against it, so the wire format and the reported
numbers are checked from the outside.

What it covers:

* the request is a 48 byte RFC 5905 client packet with exactly one live
  timestamp in it;
* a well formed answer is accepted, and the offset, root delay, root dispersion
  and reported time all agree with what the server sent;
* a negative offset comes back with the right sign;
* a server that overstates its processing time produces a negative round trip
  delay, and the tool says the delay is not plausible;
* a Kiss-o'-Death is reported, with its code, and is not retried;
* an answer that is ours but unusable ends the exchange;
* an answer whose origin field does not echo the request is ignored and the
  request is sent again;
* an answer from a different port is ignored even when it is well formed.

Examples
--------
    python scripts/cross_validate_ntp.py
    python scripts/cross_validate_ntp.py --tool build/vs2022/tools/Debug/ntplite.exe

The only dependency is the standard library, and the tool must already be built
(`cmake --build <build dir>`).  Exit status is 0 when every check passed.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, List, Optional, Sequence, Tuple

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

# NTP counts seconds from 1900, Unix from 1970.
NTP_EPOCH_OFFSET = 2208988800
PACKET_SIZE = 48
FRACTION_UNIT = 1 << 32

LEAP_NO_WARNING = 0
LEAP_UNSYNCHRONIZED = 3
VERSION_4 = 4
MODE_CLIENT = 3
MODE_SERVER = 4

# Mirrors ntplite.h.  Repeated rather than imported, because this script is
# deliberately not allowed to share anything with the library under test.
STATUS_OK = 0
STATUS_TIMEOUT = 4
STATUS_PROTOCOL = 5
STATUS_KOD = 6

SUCCESS_REFERENCE_ID = b"\xc0\x00\x02\x01"  # 192.0.2.1, as a stratum 2 server


class CheckFailed(Exception):
    """One expectation did not hold; the message says which."""


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailed(message)


# ---------------------------------------------------------------------------
# The wire format, written out longhand
# ---------------------------------------------------------------------------


def encode_timestamp(unix_seconds: float) -> bytes:
    """Pack seconds since the Unix epoch as a 64 bit 32.32 NTP timestamp."""
    shifted = unix_seconds + NTP_EPOCH_OFFSET
    seconds = int(math.floor(shifted))
    fraction = shifted - seconds  # already in [0, 1), thanks to the floor
    scaled = int(round(fraction * FRACTION_UNIT))
    if scaled >= FRACTION_UNIT:  # rounding carried into the seconds
        seconds += 1
        scaled = 0
    return struct.pack("!II", seconds & 0xFFFFFFFF, scaled & 0xFFFFFFFF)


def decode_timestamp(raw: bytes) -> float:
    """Read a 64 bit 32.32 NTP timestamp as seconds since the Unix epoch."""
    seconds, fraction = struct.unpack("!II", raw)
    return seconds + fraction / float(FRACTION_UNIT) - NTP_EPOCH_OFFSET


def encode_short(seconds: float) -> int:
    """Pack a signed 16.16 short, as NTP uses for delays and dispersions."""
    scaled = int(round(seconds * 65536.0))
    return max(0, min(0xFFFFFFFF, scaled))


def decode_short(raw: bytes) -> float:
    return struct.unpack("!I", raw)[0] / 65536.0


def decode_request(data: bytes) -> dict:
    """Split a datagram into the fields of RFC 5905 figure 8, longhand."""
    expect(len(data) == PACKET_SIZE, f"the request is {len(data)} bytes, not {PACKET_SIZE}")
    first = data[0]
    return {
        "leap": first >> 6,
        "version": (first >> 3) & 0x07,
        "mode": first & 0x07,
        "stratum": data[1],
        "poll": struct.unpack("!b", data[2:3])[0],
        "precision": struct.unpack("!b", data[3:4])[0],
        "root_delay": decode_short(data[4:8]),
        "root_dispersion": decode_short(data[8:12]),
        "reference_id": data[12:16],
        "reference": data[16:24],
        "origin": data[24:32],
        "receive": data[32:40],
        "transmit": data[40:48],
    }


def build_reply(
    request: dict,
    *,
    offset: float = 0.0,
    stratum: int = 2,
    reference_id: bytes = SUCCESS_REFERENCE_ID,
    poll: int = 3,
    precision: int = -20,
    root_delay: float = 0.001,
    root_dispersion: float = 0.002,
    processing: float = 0.0,
    origin: Optional[bytes] = None,
    leap: int = LEAP_NO_WARNING,
    version: int = VERSION_4,
    mode: int = MODE_SERVER,
) -> bytes:
    """Build the answer to `request`.

    `offset` shifts the server's clock away from this machine's, `processing` is
    the time the server claims passed between reading the request and writing the
    answer, and `origin` overrides the echo of the request's transmit timestamp.
    """
    answered_at = time.time() + offset
    receive = encode_timestamp(answered_at)
    transmit = encode_timestamp(answered_at + processing)

    packet = struct.pack(
        "!BBbb", (leap << 6) | (version << 3) | mode, stratum, poll, precision
    )
    packet += struct.pack("!II", encode_short(root_delay), encode_short(root_dispersion))
    packet += reference_id
    packet += encode_timestamp(answered_at - 60.0)  # when we last set our clock
    packet += request["transmit"] if origin is None else origin
    packet += receive
    packet += transmit
    return packet


# ---------------------------------------------------------------------------
# A server, and a caller
# ---------------------------------------------------------------------------


class NtpServer:
    """A one thread NTP server on loopback that remembers what it was asked.

    `responder` is handed the decoded request and its one based index, and
    returns the bytes to answer with, or None to stay silent.
    """

    def __init__(self, responder: Callable[[dict, int], Optional[bytes]]) -> None:
        self._responder = responder
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind(("127.0.0.1", 0))
        self._socket.settimeout(0.05)
        self.port = self._socket.getsockname()[1]
        self.requests: List[Tuple[bytes, tuple]] = []
        self.failures: List[str] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self) -> "NtpServer":
        self._thread.start()
        return self

    def __exit__(self, *unused: object) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._socket.close()

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                data, peer = self._socket.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            self.requests.append((data, peer))
            try:
                reply = self._responder(decode_request(data), len(self.requests))
            except Exception as error:  # noqa: BLE001 - reported, not swallowed
                self.failures.append(f"could not answer request {len(self.requests)}: {error!r}")
                return
            if reply is not None:
                self._socket.sendto(reply, peer)

    def wait_for_requests(self, count: int, timeout: float = 5.0) -> int:
        deadline = time.time() + timeout
        while len(self.requests) < count and time.time() < deadline:
            time.sleep(0.005)
        return len(self.requests)


def run_tool(tool: Path, port: int, extra: Sequence[str]) -> Tuple[int, dict, str]:
    """Run the command line tool against 127.0.0.1:port and parse its JSON."""
    command = [str(tool), "-4", "--json", "127.0.0.1", "-p", str(port)]
    command += [str(item) for item in extra]
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=60, check=False
    )
    text = (completed.stdout or "").strip()
    expect(text != "", f"the tool printed nothing (exit {completed.returncode})")
    try:
        payload = json.loads(text.splitlines()[-1])
    except json.JSONDecodeError as error:
        raise CheckFailed(f"the tool did not print JSON: {text!r} ({error})") from None
    return completed.returncode, payload, completed.stderr or ""


ISO_PATTERN = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?Z$"
)


def parse_iso_utc(text: str) -> float:
    """Read the tool's ISO 8601 UTC text as seconds since the Unix epoch."""
    match = ISO_PATTERN.match(text)
    expect(match is not None, f"{text!r} is not an ISO 8601 UTC timestamp")
    assert match is not None  # for the type checker; expect() already proved it
    year, month, day, hour, minute, second = (int(group) for group in match.groups()[:6])
    digits = match.group(7) or ""
    nanoseconds = int((digits + "000000000")[:9]) if digits else 0
    moment = datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)
    return moment.timestamp() + nanoseconds / 1e9


# ---------------------------------------------------------------------------
# The checks
# ---------------------------------------------------------------------------


def check_answers_a_query(tool: Path) -> None:
    """A correct answer is accepted, and every field in it survives the trip."""
    offset = 1234.25  # a quarter second, so the fraction part is exercised
    server = NtpServer(lambda request, index: build_reply(request, offset=offset))

    with server:
        exit_code, result, _ = run_tool(tool, server.port, ["-t", "2000", "--retry-interval", "200"])

    expect(not server.failures, "; ".join(server.failures))
    expect(exit_code == 0, f"the tool exited {exit_code}, expected 0")
    expect(result["ok"] is True, f"ok is {result['ok']}, expected true")
    expect(result["status"] == STATUS_OK, f"status is {result['status']}, expected {STATUS_OK}")

    # The request, as the server saw it.
    expect(
        len(server.requests) == 1,
        f"the tool sent {len(server.requests)} requests, expected exactly 1",
    )
    request = decode_request(server.requests[0][0])
    expect(request["mode"] == MODE_CLIENT, f"mode is {request['mode']}, expected {MODE_CLIENT}")
    expect(request["version"] == VERSION_4, f"version is {request['version']}, expected 4")
    expect(request["leap"] == LEAP_NO_WARNING, f"leap is {request['leap']}, expected 0")
    expect(request["stratum"] == 0, f"stratum is {request['stratum']}, expected 0")
    expect(request["poll"] == 3, f"poll is {request['poll']}, expected 3")
    expect(request["precision"] == -20, f"precision is {request['precision']}, expected -20")
    for field in ("reference", "origin", "receive"):
        expect(
            request[field] == b"\0" * 8,
            f"a request must leave {field} zero, but it holds {request[field]!r}",
        )
    expect(
        request["transmit"] != b"\0" * 8,
        "the request carries no transmit timestamp for the server to echo",
    )
    asked_at = decode_timestamp(request["transmit"])
    expect(
        abs(asked_at - time.time()) < 5.0,
        f"the request is dated {asked_at}, which is not about now ({time.time()})",
    )

    # The answer, as the tool reported it.
    expect(result["valid"] is True, "the result is not marked valid")
    expect(result["kiss_of_death"] is False, "a normal answer was flagged as a refusal")
    expect(result["stratum"] == 2, f"stratum is {result['stratum']}, expected 2")
    expect(result["version"] == VERSION_4, f"version is {result['version']}, expected 4")
    expect(result["mode"] == MODE_SERVER, f"mode is {result['mode']}, expected {MODE_SERVER}")
    expect(result["reference"] == "192.0.2.1", f"reference is {result['reference']!r}")
    expect(
        abs(result["offset_seconds"] - offset) < 0.05,
        f"offset is {result['offset_seconds']}, expected about {offset}",
    )
    expect(
        abs(result["root_delay_seconds"] - 0.001) < 1e-4,
        f"root delay is {result['root_delay_seconds']}, expected about 0.001",
    )
    expect(
        abs(result["root_dispersion_seconds"] - 0.002) < 1e-4,
        f"root dispersion is {result['root_dispersion_seconds']}, expected about 0.002",
    )
    expect(
        abs(result["server_processing_seconds"]) < 1e-6,
        f"server processing is {result['server_processing_seconds']}, expected 0",
    )
    expect(
        result["delay_is_plausible"] is True,
        "a loopback exchange was judged to have an implausible delay",
    )
    expect(result["attempts"] == 1, f"attempts is {result['attempts']}, expected 1")

    reported = parse_iso_utc(result["time"])
    expect(
        abs(reported - (time.time() + offset)) < 1.0,
        f"the tool reports {result['time']}, which is not the server's clock",
    )


def check_negative_offset(tool: Path) -> None:
    """A server behind us produces a negative offset, not a mangled one."""
    offset = -2000.5
    server = NtpServer(lambda request, index: build_reply(request, offset=offset))

    with server:
        exit_code, result, _ = run_tool(tool, server.port, ["-t", "2000"])

    expect(not server.failures, "; ".join(server.failures))
    expect(exit_code == 0, f"the tool exited {exit_code}, expected 0")
    expect(
        abs(result["offset_seconds"] - offset) < 0.05,
        f"offset is {result['offset_seconds']}, expected about {offset}",
    )
    expect(
        parse_iso_utc(result["time"]) < time.time(),
        "the reported time is ahead of ours even though the server is behind",
    )


def check_overstated_processing(tool: Path) -> None:
    """A server that claims to have held the packet too long is called out."""
    processing = 0.2
    server = NtpServer(lambda request, index: build_reply(request, processing=processing))

    with server:
        exit_code, result, _ = run_tool(tool, server.port, ["-t", "2000"])

    expect(not server.failures, "; ".join(server.failures))
    expect(exit_code == 0, f"the tool exited {exit_code}, expected 0")
    expect(
        abs(result["server_processing_seconds"] - processing) < 1e-6,
        f"server processing is {result['server_processing_seconds']}, expected {processing}",
    )
    # (T4 - T1) - (T3 - T2) with T3 - T2 invented: on loopback this goes negative,
    # which is exactly the signal a caller needs before trusting the offset.
    expect(
        result["round_trip_delay_seconds"] < 0,
        f"the round trip delay is {result['round_trip_delay_seconds']}, expected negative",
    )
    expect(
        result["delay_is_plausible"] is False,
        "an impossible delay was reported as plausible",
    )
    expect(
        abs(result["offset_seconds"] - processing / 2.0) < 0.01,
        f"offset is {result['offset_seconds']}, expected about {processing / 2.0}",
    )


def check_kiss_of_death(tool: Path) -> None:
    """A refusal is reported with its code, and asking again would be rude."""
    server = NtpServer(
        lambda request, index: build_reply(
            request, stratum=0, reference_id=b"RATE", leap=LEAP_UNSYNCHRONIZED
        )
    )

    with server:
        exit_code, result, _ = run_tool(tool, server.port, ["-t", "2000", "--retry-interval", "200"])

    expect(not server.failures, "; ".join(server.failures))
    expect(exit_code == 1, f"the tool exited {exit_code}, expected 1")
    expect(result["ok"] is False, "a refusal was reported as success")
    expect(
        result["status"] == STATUS_KOD,
        f"status is {result['status']}, expected {STATUS_KOD}",
    )
    expect(result["valid"] is False, "a refusal was marked valid")
    expect(result["kiss_of_death"] is True, "the refusal was not reported as such")
    expect(result["kiss_code"] == "RATE", f"kiss code is {result['kiss_code']!r}, expected 'RATE'")
    expect(result["reference"] == "RATE", f"reference is {result['reference']!r}, expected 'RATE'")
    expect(
        len(server.requests) == 1,
        f"the tool asked {len(server.requests)} times after being refused",
    )


def check_unusable_answer(tool: Path) -> None:
    """An answer that is ours but the wrong shape is reported, not waited on."""
    server = NtpServer(lambda request, index: build_reply(request, mode=MODE_CLIENT))

    with server:
        exit_code, result, _ = run_tool(tool, server.port, ["-t", "2000", "--retry-interval", "200"])

    expect(not server.failures, "; ".join(server.failures))
    expect(exit_code == 1, f"the tool exited {exit_code}, expected 1")
    expect(
        result["status"] == STATUS_PROTOCOL,
        f"status is {result['status']}, expected {STATUS_PROTOCOL}",
    )
    expect(result["valid"] is False, "a malformed answer was marked valid")
    expect(
        len(server.requests) == 1,
        f"the tool asked {len(server.requests)} times after an unusable answer",
    )


def check_wrong_origin(tool: Path) -> None:
    """A datagram that does not echo our transmit timestamp is not an answer."""
    server = NtpServer(
        lambda request, index: build_reply(request, origin=b"\x01\x02\x03\x04\x05\x06\x07\x08")
    )

    with server:
        exit_code, result, _ = run_tool(tool, server.port, ["-t", "400", "--retry-interval", "150"])

    expect(not server.failures, "; ".join(server.failures))
    expect(exit_code == 1, f"the tool exited {exit_code}, expected 1")
    expect(
        result["status"] == STATUS_TIMEOUT,
        f"status is {result['status']}, expected {STATUS_TIMEOUT} (the stray reply was used)",
    )
    expect(result["valid"] is False, "a stray reply was marked valid")
    expect(
        len(server.requests) >= 2,
        f"the tool asked {len(server.requests)} times, so it neither retried nor waited",
    )


def check_wrong_source_port(tool: Path) -> None:
    """A well formed answer from another port is ignored: it is not the server.

    The socket is connected, so the kernel already discards datagrams from other
    sources on most systems; the check also covers the library's own comparison,
    which is what protects a platform that does deliver them.
    """
    impostor = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    impostor.bind(("127.0.0.1", 0))

    def stay_silent(request: dict, index: int) -> Optional[bytes]:
        return None

    server = NtpServer(stay_silent)

    def answer_from_the_wrong_port() -> None:
        answered = 0
        deadline = time.time() + 10.0
        while answered < 3 and time.time() < deadline:
            while answered < len(server.requests):
                data, peer = server.requests[answered]
                answered += 1
                impostor.sendto(build_reply(decode_request(data)), peer)
            time.sleep(0.005)

    with server:
        thread = threading.Thread(target=answer_from_the_wrong_port, daemon=True)
        thread.start()
        try:
            exit_code, result, _ = run_tool(
                tool, server.port, ["-t", "400", "--retry-interval", "150"]
            )
        finally:
            thread.join(timeout=2.0)
            impostor.close()

    expect(not server.failures, "; ".join(server.failures))
    expect(
        len(server.requests) >= 2,
        f"the tool asked {len(server.requests)} times, so it never retried",
    )
    expect(exit_code == 1, f"the tool exited {exit_code}, expected 1")
    expect(
        result["status"] == STATUS_TIMEOUT,
        f"status is {result['status']}, expected {STATUS_TIMEOUT}: an answer from another "
        f"port was accepted",
    )
    expect(result["valid"] is False, "an answer from another port was marked valid")


CHECKS: Sequence[Tuple[str, Callable[[Path], None]]] = (
    ("a well formed answer is accepted, and the request is RFC 5905", check_answers_a_query),
    ("a negative offset keeps its sign", check_negative_offset),
    ("an overstated processing time makes the delay implausible", check_overstated_processing),
    ("a Kiss-o'-Death is reported and not retried", check_kiss_of_death),
    ("an unusable answer ends the exchange", check_unusable_answer),
    ("a reply that does not echo the request is ignored", check_wrong_origin),
    ("a reply from the wrong port is ignored", check_wrong_source_port),
)


def find_tool() -> Path:
    """Find a built ntplite tool, so the script works without arguments."""
    for pattern in ("build/**/ntplite.exe", "build/**/ntplite"):
        for candidate in sorted(REPOSITORY_ROOT.glob(pattern)):
            if candidate.is_file():
                return candidate
    raise CheckFailed(
        "no built ntplite tool found under build/; build it first, or pass --tool PATH"
    )


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Cross-validate the ntplite tool against a second implementation of NTP.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--tool",
        type=Path,
        help="the ntplite command line tool to drive (default: the newest one under build/)",
    )
    parser.add_argument("-q", "--quiet", action="store_true", help="only report failures")
    arguments = parser.parse_args(argv)

    try:
        tool = arguments.tool or find_tool()
    except CheckFailed as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if not tool.is_file():
        print(f"error: {tool} does not exist", file=sys.stderr)
        return 1

    if not arguments.quiet:
        print(f"cross-validating {tool}")
        print(f"{len(CHECKS)} checks, against a server written from RFC 5905\n")

    failed = 0
    for name, check in CHECKS:
        try:
            check(tool)
        except CheckFailed as error:
            failed += 1
            print(f"FAIL  {name}\n        {error}")
        except Exception as error:  # noqa: BLE001 - a crash is a failure too
            failed += 1
            print(f"ERROR {name}\n        {error!r}")
        else:
            if not arguments.quiet:
                print(f"ok    {name}")

    if failed:
        print(f"\n{failed} of {len(CHECKS)} checks failed")
        return 1

    if not arguments.quiet:
        print(f"\nall {len(CHECKS)} checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
