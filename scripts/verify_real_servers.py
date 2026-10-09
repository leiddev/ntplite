#!/usr/bin/env python3
"""Verify ntplite against real NTP servers on the public internet.

Everything else in this repository is deterministic and offline.  The mock server
and the cross-validation script between them cover the protocol, but both are
answering over a loopback interface with a clock that is a few microseconds
away, and neither of them is a real server: no stratum 1 or 2 machine, no
dispersion that grew over days, no network in between, and no clock on the other
end that is kept right by somebody else.

This script closes that last gap.  It runs the compiled tool against real servers
and checks the numbers it reports against an NTP client written here, longhand,
with no code shared with the library:

* the tool reaches the server, and the server really is a server - a stratum of
  at least 1, an answer that echoes the request, a leap indicator that is not the
  unsynchronised alarm;
* the offset the tool reports agrees with the offset this script measures against
  the same machine, within the uncertainty both round trips allow;
* the time the tool reports agrees with this host's own clock, which is a second
  opinion the tool has never met: it was synchronised by whatever the machine
  runs, not by ntplite;
* the local clock is untouched, checked by watching the wall clock and the
  monotonic clock move together across the run - a clock that had been stepped
  would part them immediately.

What it does not do is compare against a second NTP implementation such as
`ntpq`, because that would make the check depend on what happens to be installed.

Examples
--------
    python scripts/verify_real_servers.py
    python scripts/verify_real_servers.py --servers pool.ntp.org,ntp.aliyun.com
    python scripts/verify_real_servers.py --repeat 3 --host-tolerance-ms 20000

It needs a network that lets UDP port 123 out, and a built tool
(`cmake --build <build dir>`); CTest registers it as `ntplite.real_servers` when
the project is configured with `-DNTP_LITE_ONLINE_TESTS=ON`.  Exit status is 0
when every round passed.
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
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Sequence, Tuple

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

# NTP counts seconds from 1900, Unix from 1970.
NTP_EPOCH_OFFSET = 2208988800
PACKET_SIZE = 48
FRACTION_UNIT = 1 << 32

LEAP_ALARM = 3  # the server says its own clock is not right
MODE_CLIENT = 3
MODE_SERVER = 4
VERSION_4 = 4

# Every server on the pool answers, everywhere, and the tool defaults to it too.
DEFAULT_SERVERS = ("pool.ntp.org",)

ISO_PATTERN = re.compile(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?Z$")


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


def decode_short(raw: bytes) -> float:
    """Read one of NTP's 16.16 fixed point fields, as the server sent it."""
    return struct.unpack("!I", raw)[0] / 65536.0


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


def split_endpoint(text: str) -> Tuple[str, int]:
    """Split "192.0.2.1:123" or "[2001:db8::1]:123" into its two halves."""
    expect(text != "", "the tool did not report which server answered")
    host, separator, port = text.rpartition(":")
    expect(separator != "" and port.isdigit(), f"{text!r} is not an address and a port")
    if host.startswith("[") and host.endswith("]"):
        host = host[1:-1]
    return host, int(port)


# ---------------------------------------------------------------------------
# This script's own NTP client
# ---------------------------------------------------------------------------


class Measurement:
    """One exchange with one server, measured by this script alone."""

    def __init__(
        self,
        address: str,
        port: int,
        sent_at: float,
        received_at: float,
        reply: bytes,
    ) -> None:
        self.address = address
        self.port = port
        self.sent_at = sent_at  # T1
        self.received_at = received_at  # T4
        self.receive = decode_timestamp(reply[32:40])  # T2, the server's clock
        self.transmit = decode_timestamp(reply[40:48])  # T3, as the server sent it
        self.root_delay = decode_short(reply[4:8])
        self.root_dispersion = decode_short(reply[8:12])
        self.stratum = reply[1]
        self.version = (reply[0] >> 3) & 0x07
        self.reference_id = reply[12:16]

    @property
    def offset(self) -> float:
        """theta = ((T2 - T1) + (T3 - T4)) / 2, straight out of RFC 5905."""
        return ((self.receive - self.sent_at) + (self.transmit - self.received_at)) / 2.0

    @property
    def delay(self) -> float:
        """delta = (T4 - T1) - (T3 - T2)."""
        return (self.received_at - self.sent_at) - (self.transmit - self.receive)


def ask_a_server(address: str, port: int, timeout: float) -> Measurement:
    """Ask one server directly, with a client built from `socket` and `struct`."""
    family = socket.AF_INET6 if ":" in address else socket.AF_INET
    connection = socket.socket(family, socket.SOCK_DGRAM)
    connection.settimeout(timeout)

    request = bytearray(PACKET_SIZE)
    request[0] = (VERSION_4 << 3) | MODE_CLIENT
    request[2] = 3  # poll: ask again in 8 seconds if we ever did
    request[3] = 0xEC  # precision: -20, the smallest a sane server reports
    sent_at = time.time()
    request[40:48] = encode_timestamp(sent_at)

    try:
        connection.sendto(bytes(request), (address, port))
        reply, _ = connection.recvfrom(2048)
        received_at = time.time()
    except socket.timeout:
        raise CheckFailed(
            f"{address}:{port} did not answer within {timeout:.0f} s on UDP port {port}"
        ) from None
    finally:
        connection.close()

    expect(
        len(reply) == PACKET_SIZE,
        f"{address} answered with {len(reply)} bytes, not {PACKET_SIZE}",
    )

    leap = reply[0] >> 6
    mode = reply[0] & 0x07
    expect(mode == MODE_SERVER, f"{address} answered in mode {mode}, not {MODE_SERVER}")
    expect(
        reply[24:32] == request[40:48],
        f"{address} answered with an origin timestamp that is not the one we sent",
    )
    if reply[1] == 0:
        raise CheckFailed(
            f"{address} answered with stratum 0 and reference id "
            f"{reply[12:16].decode('ascii', 'replace')!r}: that is a refusal, not an answer"
        )

    measurement = Measurement(address, port, sent_at, received_at, reply)
    expect(
        measurement.delay >= 0.0,
        f"{address} reports a receive time before our send time: the clocks disagree "
        f"by more than the round trip",
    )
    expect(leap != LEAP_ALARM, f"{address} answers while saying its own clock is not right")
    return measurement


# ---------------------------------------------------------------------------
# The tool under test
# ---------------------------------------------------------------------------


def run_tool(tool: Path, server: str, timeout_ms: int) -> dict:
    """Query one name with the compiled tool and return its JSON report."""
    # Four times one server's budget: a silent server has to be asked more than
    # once before the tool is entitled to give up.
    total_ms = 4 * timeout_ms
    command = [
        str(tool),
        "-4",
        "--json",
        server,
        "-t",
        str(timeout_ms),
        "-T",
        str(total_ms),
    ]
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=total_ms / 1000.0 + 30.0, check=False
    )
    text = (completed.stdout or "").strip()
    expect(
        text != "",
        f"the tool printed nothing for {server} (exit {completed.returncode}): "
        f"{(completed.stderr or '').strip()}",
    )
    try:
        report = json.loads(text.splitlines()[-1])
    except json.JSONDecodeError as error:
        raise CheckFailed(f"the tool did not print JSON: {text!r} ({error})") from None
    expect(
        report["ok"] is True,
        f"the tool could not use {server}: {report['status_text']}"
        + (f" (kiss-o'-death {report['kiss_code']})" if report["kiss_of_death"] else ""),
    )
    return report


def monotonic_seconds() -> float:
    """A clock that cannot be stepped: CLOCK_MONOTONIC where there is one."""
    if hasattr(time, "clock_gettime"):  # POSIX; Windows has no CLOCK_MONOTONIC
        return time.clock_gettime(time.CLOCK_MONOTONIC)
    return time.perf_counter()


def sample_clocks() -> Tuple[float, float]:
    """The monotonic and the wall clock, read as close together as possible."""
    monotonic = monotonic_seconds()
    wall = time.time()
    return monotonic, wall


def clock_step_seconds(before: Tuple[float, float], after: Tuple[float, float]) -> float:
    """How far the wall clock was stepped while the two samples were taken.

    Zero when the wall clock advanced exactly as much as the monotonic clock did,
    which is what "the program did not touch the system time" means in practice.
    """
    return (after[1] - before[1]) - (after[0] - before[0])


# ---------------------------------------------------------------------------
# The check
# ---------------------------------------------------------------------------


class Round:
    """What one round of the check learned, for the report."""

    def __init__(self) -> None:
        self.tool_offset = 0.0
        self.tool_delay = 0.0
        self.tool_stratum = 0
        self.tool_version = 0
        self.tool_attempts = 0
        self.tool_reference = ""
        self.server = ""
        self.own_offset = 0.0
        self.own_delay = 0.0
        self.own_stratum = 0
        self.tolerance = 0.0
        self.host_difference = 0.0
        self.step = 0.0
        self.notes: List[str] = []


def check_one_round(
    tool: Path,
    name: str,
    timeout_ms: int,
    tolerance_floor: float,
    host_tolerance: float,
    wall_clock_tolerance: float,
) -> Round:
    """One query of `name` by the tool, checked against this script's own."""
    round_result = Round()

    before = sample_clocks()
    report = run_tool(tool, name, timeout_ms)
    after = sample_clocks()

    expect(report["valid"] is True, f"{name}: the answer was not marked valid")
    expect(
        report["version"] in (3, VERSION_4),
        f"{name}: the answer says it speaks NTPv{report['version']}",
    )
    expect(report["stratum"] >= 1, f"{name}: stratum is {report['stratum']}")
    expect(
        report["leap"] != LEAP_ALARM,
        f"{name}: the server answers while saying its own clock is not right",
    )
    expect(
        report["delay_is_plausible"] is True,
        f"{name}: the round trip delay was judged implausible "
        f"(delay {report['round_trip_delay_seconds']} s, processing "
        f"{report['server_processing_seconds']} s)",
    )
    expect(
        report["round_trip_delay_seconds"] >= 0.0,
        f"{name}: a negative round trip delay ({report['round_trip_delay_seconds']} s)",
    )
    expect(
        report["round_trip_time_seconds"] + 1e-9 >= report["round_trip_delay_seconds"],
        f"{name}: the round trip time ({report['round_trip_time_seconds']} s) is shorter "
        f"than the delay inside it ({report['round_trip_delay_seconds']} s)",
    )
    if report["stratum"] >= 2:
        expect(
            report["reference"] not in ("", "0.0.0.0", "::"),
            f"{name}: a stratum {report['stratum']} server reported reference id "
            f"{report['reference']!r}; a synchronised server names the clock it follows",
        )

    # The local clock, watched across the query that was supposed to leave it be.
    round_result.step = clock_step_seconds(before, after)
    expect(
        abs(round_result.step) <= wall_clock_tolerance,
        f"{name}: the wall clock moved {round_result.step * 1e3:.1f} ms relative to the "
        f"monotonic clock while the tool ran; the local clock was touched",
    )

    # The reported instant, against the clock this host keeps for itself.
    reported = parse_iso_utc(report["time"])
    round_result.host_difference = reported - after[1]
    expect(
        abs(round_result.host_difference) <= host_tolerance,
        f"{name}: the server is {round_result.host_difference:+.3f} s from this host's own "
        f"clock, which is more than the {host_tolerance:.3f} s allowed; either the server or "
        f"this host is wrong (--host-tolerance-ms changes the bound)",
    )

    # The same machine, asked by this script's own client.
    address, port = split_endpoint(report["server"])
    own = ask_a_server(address, port, max(timeout_ms / 1000.0, 1.0))

    round_result.server = report["server"]
    round_result.tool_offset = report["offset_seconds"]
    round_result.tool_delay = report["round_trip_delay_seconds"]
    round_result.tool_stratum = report["stratum"]
    round_result.tool_version = report["version"]
    round_result.tool_attempts = report["attempts"]
    round_result.tool_reference = report["reference"]
    round_result.own_offset = own.offset
    round_result.own_delay = own.delay
    round_result.own_stratum = own.stratum

    # Each exchange pins the true offset to within half its own round trip, under
    # the usual assumption that the path out took about as long as the path back.
    # The two exchanges happened a second apart over the same route, so their
    # errors largely share a sign and the difference should sit far inside that.
    # Allowing the two half delays to add up is the worst case, which is loose
    # enough for an asymmetric route and still orders of magnitude tighter than
    # the smallest mistake worth catching.
    round_result.tolerance = max(
        tolerance_floor, own.delay + report["round_trip_delay_seconds"]
    )
    difference = report["offset_seconds"] - own.offset
    expect(
        abs(difference) <= round_result.tolerance,
        f"{name}: the tool reports an offset of {report['offset_seconds']:+.6f} s and this "
        f"script's own client reports {own.offset:+.6f} s against the same machine; they "
        f"differ by {difference * 1e3:+.1f} ms, more than the "
        f"{round_result.tolerance * 1e3:.1f} ms the two round trips allow",
    )

    # The server's own numbers, read out of its answer by two sets of eyes: a
    # root delay or dispersion taken from the wrong bytes, or with the wrong byte
    # order, would be orders of magnitude out rather than milliseconds.
    for field, ours in (
        ("root_delay_seconds", own.root_delay),
        ("root_dispersion_seconds", own.root_dispersion),
    ):
        theirs = report[field]
        expect(
            abs(theirs - ours) <= 2.0,
            f"{name}: the tool reads {field} as {theirs} s where this script reads {ours} s "
            f"out of the same server's answer",
        )

    if own.stratum != report["stratum"]:
        round_result.notes.append(
            f"stratum {report['stratum']} to the tool, {own.stratum} to this script "
            f"(the address answers on more than one machine)"
        )
    elif own.stratum >= 2:
        # A stratum 2 server names the clock it follows, and both readers should
        # find the same name.  A difference is reported rather than judged: an
        # anycast address can answer from two machines that keep two upstreams.
        ours = socket.inet_ntoa(own.reference_id)
        if ours != report["reference"]:
            round_result.notes.append(
                f"reference id {report['reference']} to the tool, {ours} to this script"
            )
    if report["attempts"] > 1:
        round_result.notes.append(f"{report['attempts']} requests were needed")
    return round_result


def print_round(name: str, round_result: Round) -> None:
    print(f"  {name}")
    print(
        f"    the tool        {round_result.server:<24} offset {round_result.tool_offset:+.6f} s"
        f"  delay {round_result.tool_delay * 1e3:8.3f} ms"
        f"  NTPv{round_result.tool_version}"
        f"  {round_result.tool_attempts} request"
        f"{'' if round_result.tool_attempts == 1 else 's'}"
    )
    print(
        f"    our own client  {round_result.server:<24} offset {round_result.own_offset:+.6f} s"
        f"  delay {round_result.own_delay * 1e3:8.3f} ms"
    )
    print(
        f"    the server      stratum {round_result.tool_stratum}, "
        f"reference {round_result.tool_reference}"
    )
    print(
        f"    the two agree   they differ by "
        f"{abs(round_result.tool_offset - round_result.own_offset) * 1e3:.3f} ms, "
        f"within the {round_result.tolerance * 1e3:.1f} ms the round trips allow"
    )
    print(
        f"    this host       the server reads {round_result.host_difference * 1e3:+8.3f} ms "
        f"from the clock this machine keeps for itself"
    )
    print(
        f"    local clock     untouched: the wall clock moved "
        f"{round_result.step * 1e6:+.1f} us relative to the monotonic clock"
    )
    for note in round_result.notes:
        print(f"    note            {note}")


def find_tool() -> Path:
    """Find a built ntplite tool, so the script works without arguments."""
    candidates = [
        candidate
        for pattern in ("build/**/ntplite.exe", "build/**/ntplite")
        for candidate in REPOSITORY_ROOT.glob(pattern)
        if candidate.is_file()
    ]
    if not candidates:
        raise CheckFailed(
            "no built ntplite tool found under build/; build it first, or pass --tool PATH"
        )
    # The newest, so a Debug and a Release build side by side are not a lottery.
    return max(candidates, key=lambda candidate: candidate.stat().st_mtime)


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Verify the ntplite tool against real NTP servers.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--tool",
        type=Path,
        help="the ntplite command line tool to drive (default: the newest one under build/)",
    )
    parser.add_argument(
        "--servers",
        default=",".join(DEFAULT_SERVERS),
        help=f"comma separated names to ask (default: {','.join(DEFAULT_SERVERS)})",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=2,
        help="rounds per name, to show the result is repeatable (default: 2)",
    )
    parser.add_argument(
        "--timeout-ms",
        type=int,
        default=3000,
        help="budget for one server inside the tool (default: 3000)",
    )
    parser.add_argument(
        "--tolerance-floor-ms",
        type=float,
        default=50.0,
        help="smallest difference allowed between the two offsets (default: 50)",
    )
    parser.add_argument(
        "--host-tolerance-ms",
        type=float,
        default=5000.0,
        help="how far the server may be from this host's own clock (default: 5000; "
        "raise it if this host is not synchronised)",
    )
    parser.add_argument(
        "--wall-clock-tolerance-ms",
        type=float,
        default=50.0,
        help="how far the wall clock may drift from the monotonic clock across a "
        "query before it counts as having been touched (default: 50)",
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

    names = [name.strip() for name in arguments.servers.split(",") if name.strip()]
    if not names:
        print("error: --servers is empty", file=sys.stderr)
        return 2
    if arguments.repeat < 1:
        print("error: --repeat must be at least 1", file=sys.stderr)
        return 2

    tolerance_floor = arguments.tolerance_floor_ms / 1000.0
    host_tolerance = arguments.host_tolerance_ms / 1000.0
    wall_clock_tolerance = arguments.wall_clock_tolerance_ms / 1000.0

    if not arguments.quiet:
        print(f"verifying {tool}")
        print(
            f"{len(names)} server{'s' if len(names) != 1 else ''}, "
            f"{arguments.repeat} round{'s' if arguments.repeat != 1 else ''} each, "
            f"against a client written from RFC 5905\n"
        )

    failed = 0
    rounds = 0
    for name in names:
        for index in range(arguments.repeat):
            rounds += 1
            try:
                result = check_one_round(
                    tool, name, arguments.timeout_ms, tolerance_floor, host_tolerance,
                    wall_clock_tolerance,
                )
            except CheckFailed as error:
                failed += 1
                print(f"FAIL  {name} (round {index + 1} of {arguments.repeat})\n        {error}")
            except socket.gaierror as error:
                failed += 1
                print(f"FAIL  {name} (round {index + 1} of {arguments.repeat})\n        "
                      f"the name does not resolve: {error}")
            except Exception as error:  # noqa: BLE001 - a crash is a failure too
                failed += 1
                print(f"ERROR {name} (round {index + 1} of {arguments.repeat})\n        {error!r}")
            else:
                if not arguments.quiet:
                    print_round(name, result)

    if failed:
        print(f"\n{failed} of {rounds} rounds failed")
        return 1
    if not arguments.quiet:
        print(f"\nall {rounds} rounds passed: the tool agrees with a real server")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
