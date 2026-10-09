// ============================================================================
// ntplite - detail/socket.hpp
// ----------------------------------------------------------------------------
// The UDP transport: platform sockets, name resolution and timeouts.
//
// This is the only header in ntplite that talks to the operating system, and
// it is deliberately the only one that is platform conditional.  Everything
// above it (the client) is pure logic over bytes, which is what makes the
// client testable without a network and what keeps the packet layer free of
// platform types.
//
// Design notes
//   * Only UDP datagrams are supported; NTP never uses anything else.  A
//     *connected* UDP socket gives us the kernel's own error reporting for the
//     ICMP "port unreachable" replies a dead server produces, which is worth
//     more than the convenience of sendto().
//   * Timeouts use select(2) rather than poll(2) because Winsock has no poll,
//     and we never wait on more than one descriptor at a time.
//   * Deadlines are measured against std::chrono::steady_clock.  A wall-clock
//     jump - which is exactly what happens on a host that some *other*
//     program is busy synchronising - must never turn a 2 s timeout into a
//     hang or an instant failure.
//
// Windows integration caveat
//   <winsock2.h> must be seen before <windows.h>, and <windows.h> drags in the
//   Winsock 1 headers unless WIN32_LEAN_AND_MEAN is defined.  Both switches are
//   set below, before the first Windows header is read, so the normal case is
//   simply "include ntplite, then include whatever you like".  If a translation
//   unit already pulled in <windows.h> without WIN32_LEAN_AND_MEAN the two
//   Winsock generations cannot coexist; rather than emit a screenful of
//   redefinition errors we stop with an explanation.
// ============================================================================

#ifndef NTP_LITE_DETAIL_SOCKET_HPP
#define NTP_LITE_DETAIL_SOCKET_HPP

#include <ntplite/detail/config.hpp>

// ---------------------------------------------------------------------------
// Native socket headers
// ---------------------------------------------------------------------------
#if NTP_LITE_PLATFORM_WINDOWS
#if defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_)
#error \
    "ntplite: <winsock2.h> has to be included before <windows.h>.  Either define WIN32_LEAN_AND_MEAN before including <windows.h>, or include <ntplite/ntplite.hpp> first."
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ntplite/detail/error.hpp>
#include <string>
#include <vector>

namespace ntplite {
namespace detail {

// ===========================================================================
// Native handles
// ===========================================================================

#if NTP_LITE_PLATFORM_WINDOWS
/// Winsock handles are opaque kernel object handles, not small integers, so
/// they need their own type and must never be squeezed into an int or passed
/// to a function expecting a descriptor index.
typedef SOCKET native_socket;
#else
typedef int native_socket;
#endif

/// The value of a handle that refers to no socket.
inline native_socket invalid_native_socket() NTP_LITE_NOEXCEPT {
#if NTP_LITE_PLATFORM_WINDOWS
  return INVALID_SOCKET;
#else
  return -1;
#endif
}

/// True when `handle` refers to an open socket.
inline bool socket_is_valid(native_socket handle) NTP_LITE_NOEXCEPT {
#if NTP_LITE_PLATFORM_WINDOWS
  return handle != INVALID_SOCKET;
#else
  return handle >= 0;
#endif
}

// ===========================================================================
// Error translation
// ===========================================================================

/// The error code of the most recent failed socket call on this thread.
///
/// Winsock reports through WSAGetLastError() rather than errno, which is the
/// whole reason this indirection exists.
inline int last_socket_error() NTP_LITE_NOEXCEPT {
#if NTP_LITE_PLATFORM_WINDOWS
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

/// True when `code` means "nothing was available", i.e. a non-blocking receive
/// found an empty queue.  Not a failure.
inline bool is_would_block_error(int code) NTP_LITE_NOEXCEPT {
#if NTP_LITE_PLATFORM_WINDOWS
  return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
  return code == EAGAIN
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
         || code == EWOULDBLOCK
#endif
      ;
#endif
}

/// True when `code` means the call was cut short by a signal; try again.
inline bool is_interrupted_error(int code) NTP_LITE_NOEXCEPT {
#if NTP_LITE_PLATFORM_WINDOWS
  return code == WSAEINTR;
#else
  return code == EINTR;
#endif
}

/// The platform's "the operation timed out" code.
inline int socket_timeout_error() NTP_LITE_NOEXCEPT {
#if NTP_LITE_PLATFORM_WINDOWS
  return WSAETIMEDOUT;
#else
  return ETIMEDOUT;
#endif
}

/// The platform's "bad argument to a socket call" code.
inline int socket_invalid_error() NTP_LITE_NOEXCEPT {
#if NTP_LITE_PLATFORM_WINDOWS
  return WSAEINVAL;
#else
  return EINVAL;
#endif
}

/// Translates an OS socket error into ntplite's own taxonomy.
///
/// Three sources of "nothing happened yet" all collapse into
/// error_code::timeout: a non-blocking call that would block, a call
/// interrupted by a signal, and a real timeout.  They mean the same thing to a
/// client waiting for a reply - come back later, until the deadline expires -
/// and none of them deserves a status of its own in the frozen C enumeration.
/// Callers that must tell them apart can use is_would_block_error() and
/// is_interrupted_error() on the raw code.
inline error_code classify_socket_error(int code) NTP_LITE_NOEXCEPT {
  if (code == 0) {
    return error_code::ok;
  }
  if (is_would_block_error(code) || is_interrupted_error(code)) {
    return error_code::timeout;
  }
  if (code == socket_timeout_error()) {
    return error_code::timeout;
  }
  if (code == socket_invalid_error()) {
    return error_code::invalid_argument;
  }
  return error_code::network_error;
}

// ===========================================================================
// Socket subsystem lifetime
// ===========================================================================

/// Owns the process-wide socket subsystem initialisation.
///
/// On POSIX this is a no-op; on Windows it wraps WSAStartup/WSACleanup.  It is
/// created lazily by socket_runtime_instance() and, because that accessor is
/// inline, every translation unit in the program shares one instance - which
/// matters, because calling WSAStartup() twice is legal but calling
/// WSACleanup() twice is not.
class socket_runtime {
 public:
  socket_runtime() NTP_LITE_NOEXCEPT : error_(0), ready_(true) {
#if NTP_LITE_PLATFORM_WINDOWS
    // Winsock 2.2, packed the way WSAStartup wants it: major version in the
    // low byte, minor in the high byte.  Spelled out instead of using
    // MAKEWORD() so that no Windows macro is expanded at this call site.
    WSADATA data;
    std::memset(&data, 0, sizeof(data));
    const int rc = ::WSAStartup(static_cast<WORD>(0x0202), &data);
    if (rc != 0) {
      ready_ = false;
      error_ = rc;
    }
#endif
  }

  ~socket_runtime() {
#if NTP_LITE_PLATFORM_WINDOWS
    if (ready_) {
      ::WSACleanup();
    }
#endif
  }

  socket_runtime(const socket_runtime&) = delete;
  socket_runtime& operator=(const socket_runtime&) = delete;

  /// False when the platform's socket subsystem could not be started, in which
  /// case every socket operation will fail.
  bool ready() const NTP_LITE_NOEXCEPT { return ready_; }

  /// The WSAStartup failure code, or 0.
  int error() const NTP_LITE_NOEXCEPT { return error_; }

 private:
  int error_;
  bool ready_;
};

/// The one and only socket_runtime of this program.
inline const socket_runtime& socket_runtime_instance() {
  static const socket_runtime instance;
  return instance;
}

// ===========================================================================
// Address family
// ===========================================================================

/// Which address family a socket or a resolution may use.
enum class address_family {
  any = 0,   ///< resolver's choice; sockets fall back to IPv4
  ipv4 = 4,  ///< AF_INET only
  ipv6 = 6   ///< AF_INET6 only
};

/// True when the requested family is usable in this build.
inline bool address_family_supported(address_family family) NTP_LITE_NOEXCEPT {
  if (family == address_family::ipv6) {
#if NTP_LITE_ENABLE_IPV6
    return true;
#else
    return false;
#endif
  }
  return family == address_family::any || family == address_family::ipv4;
}

// ===========================================================================
// endpoint - a resolved socket address
// ===========================================================================

/// A resolved IPv4 or IPv6 address plus port.
///
/// Holds a sockaddr_storage internally, so it is a fixed-size, copyable value
/// with no allocation, and it can be handed to bind()/sendto()/connect()
/// without further translation.  It is what the socket layer speaks; the
/// textual host name a user typed never gets this far.
class endpoint {
 public:
  /// An endpoint that refers to nothing.
  endpoint() NTP_LITE_NOEXCEPT : length_(0) { std::memset(&storage_, 0, sizeof(storage_)); }

  /// Adopts a raw OS socket address.  Returns an invalid endpoint when the
  /// address is null, empty, or longer than a sockaddr_storage - the length
  /// check matters, because the length comes from a kernel call.
  static endpoint from_native(const void* address, int length) NTP_LITE_NOEXCEPT {
    endpoint result;
    if (address == nullptr || length <= 0 ||
        static_cast<std::size_t>(length) > sizeof(sockaddr_storage)) {
      return result;
    }
    std::memcpy(&result.storage_, address, static_cast<std::size_t>(length));
    result.length_ = length;
    return result;
  }

  /// 127.0.0.1 or ::1.  Used by the tests and by the in-process mock server.
  static endpoint loopback(address_family family, std::uint16_t port) NTP_LITE_NOEXCEPT {
    return loopback_or_any(family, port, true);
  }

  /// 0.0.0.0 or ::, for bind()-ing a listening socket.
  static endpoint any(address_family family, std::uint16_t port) NTP_LITE_NOEXCEPT {
    return loopback_or_any(family, port, false);
  }

  /// True when this endpoint actually denotes an address.
  bool valid() const NTP_LITE_NOEXCEPT { return length_ != 0; }

  /// AF_INET, AF_INET6, or 0 when invalid.
  int family() const NTP_LITE_NOEXCEPT {
    return valid() ? static_cast<int>(storage_.ss_family) : 0;
  }

  bool is_ipv6() const NTP_LITE_NOEXCEPT { return valid() && storage_.ss_family == AF_INET6; }

  /// Port in host byte order, or 0 when invalid.
  std::uint16_t port() const NTP_LITE_NOEXCEPT {
    if (!valid()) {
      return 0;
    }
    if (storage_.ss_family == AF_INET) {
      return ntohs(reinterpret_cast<const sockaddr_in*>(&storage_)->sin_port);
    }
    if (storage_.ss_family == AF_INET6) {
      return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_port);
    }
    return 0;
  }

  /// Replaces the port, leaving the address alone.
  void set_port(std::uint16_t port) NTP_LITE_NOEXCEPT {
    if (storage_.ss_family == AF_INET) {
      sockaddr_in* address = reinterpret_cast<sockaddr_in*>(&storage_);
      address->sin_port = static_cast<decltype(address->sin_port)>(htons(port));
    } else if (storage_.ss_family == AF_INET6) {
      sockaddr_in6* address = reinterpret_cast<sockaddr_in6*>(&storage_);
      address->sin6_port = static_cast<decltype(address->sin6_port)>(htons(port));
    }
  }

  /// The numeric address as text, without a port.  Empty when invalid.
  ///
  /// A host name is never returned here: by the time an endpoint exists, the
  /// name has already been resolved.
  std::string address() const {
    if (!valid()) {
      return std::string();
    }
    const void* source = nullptr;
    if (storage_.ss_family == AF_INET) {
      source = &reinterpret_cast<const sockaddr_in*>(&storage_)->sin_addr;
    } else if (storage_.ss_family == AF_INET6) {
      source = &reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_addr;
    } else {
      return std::string();
    }
    char text[INET6_ADDRSTRLEN];
    std::memset(text, 0, sizeof(text));
    if (::inet_ntop(family(), source, text, sizeof(text)) == nullptr) {
      return std::string();
    }
    return std::string(text);
  }

  /// "192.0.2.1:123" or "[2001:db8::1]:123".  Empty when invalid.
  std::string to_string() const {
    const std::string host = address();
    if (host.empty()) {
      return std::string();
    }
    std::string result;
    if (is_ipv6()) {
      result.push_back('[');
      result += host;
      result.push_back(']');
    } else {
      result = host;
    }
    result.push_back(':');
    result += std::to_string(static_cast<unsigned int>(port()));
    return result;
  }

  /// Two endpoints are equal when they name the same address, port and scope.
  friend bool operator==(const endpoint& lhs, const endpoint& rhs) NTP_LITE_NOEXCEPT {
    if (lhs.family() != rhs.family() || lhs.port() != rhs.port()) {
      return false;
    }
    if (lhs.family() == AF_INET) {
      const sockaddr_in* left = reinterpret_cast<const sockaddr_in*>(&lhs.storage_);
      const sockaddr_in* right = reinterpret_cast<const sockaddr_in*>(&rhs.storage_);
      return std::memcmp(&left->sin_addr, &right->sin_addr, sizeof(in_addr)) == 0;
    }
    if (lhs.family() == AF_INET6) {
      const sockaddr_in6* left = reinterpret_cast<const sockaddr_in6*>(&lhs.storage_);
      const sockaddr_in6* right = reinterpret_cast<const sockaddr_in6*>(&rhs.storage_);
      return std::memcmp(&left->sin6_addr, &right->sin6_addr, sizeof(in6_addr)) == 0 &&
             left->sin6_scope_id == right->sin6_scope_id;
    }
    return !lhs.valid() && !rhs.valid();
  }

  friend bool operator!=(const endpoint& lhs, const endpoint& rhs) NTP_LITE_NOEXCEPT {
    return !(lhs == rhs);
  }

  /// Raw accessors for the socket calls.
  sockaddr* native_address() NTP_LITE_NOEXCEPT { return reinterpret_cast<sockaddr*>(&storage_); }
  const sockaddr* native_address() const NTP_LITE_NOEXCEPT {
    return reinterpret_cast<const sockaddr*>(&storage_);
  }
  int native_length() const NTP_LITE_NOEXCEPT { return length_; }

 private:
  static endpoint loopback_or_any(address_family family, std::uint16_t port,
                                  bool loopback) NTP_LITE_NOEXCEPT {
    endpoint result;
    if (family == address_family::ipv6) {
      sockaddr_in6 address;
      std::memset(&address, 0, sizeof(address));
      address.sin6_family = static_cast<decltype(address.sin6_family)>(AF_INET6);
      address.sin6_addr = loopback ? in6addr_loopback : in6addr_any;
      address.sin6_port = static_cast<decltype(address.sin6_port)>(htons(port));
      return from_native(&address, static_cast<int>(sizeof(address)));
    }
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = static_cast<decltype(address.sin_family)>(AF_INET);
    address.sin_addr.s_addr = htonl(loopback ? static_cast<std::uint32_t>(INADDR_LOOPBACK)
                                             : static_cast<std::uint32_t>(INADDR_ANY));
    address.sin_port = static_cast<decltype(address.sin_port)>(htons(port));
    return from_native(&address, static_cast<int>(sizeof(address)));
  }

  sockaddr_storage storage_;
  int length_;  ///< 0 means "invalid"; otherwise the used prefix length
};

// ===========================================================================
// Name resolution
// ===========================================================================

/// Resolves `host` into the list of UDP endpoints getaddrinfo() offers.
///
/// `host` may be a name, an IPv4 literal or an IPv6 literal.  On success `out`
/// is non-empty and holds every candidate in the resolver's preferred order
/// (IPv6 first on a dual-stack host).  A client is expected to try them in
/// turn, because "the first address of a round-robin DNS name is down" is the
/// single most common way an NTP query fails.
///
/// Fails with error_code::invalid_argument for a null or empty host, with
/// error_code::unsupported when the family is compiled out, and with
/// error_code::resolve_error when the name does not resolve.
inline bool resolve(const char* host, std::uint16_t port, address_family family,
                    std::vector<endpoint>& out, error_code& ec) {
  out.clear();

  if (host == nullptr || host[0] == '\0') {
    ec = error_code::invalid_argument;
    return false;
  }
  if (!address_family_supported(family)) {
    ec = error_code::unsupported;
    return false;
  }
  if (!socket_runtime_instance().ready()) {
    ec = error_code::network_error;
    return false;
  }

  int requested_family = AF_UNSPEC;
  if (family == address_family::ipv4) {
    requested_family = AF_INET;
  } else if (family == address_family::ipv6) {
    requested_family = AF_INET6;
  }
#if !NTP_LITE_ENABLE_IPV6
  else {
    // IPv6 is compiled out, so "any" has to mean IPv4.
    requested_family = AF_INET;
  }
#endif

  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = requested_family;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  // The port is applied afterwards, so that a scope identifier filled in by
  // the resolver for a link-local IPv6 address survives untouched.
  addrinfo* results = nullptr;
  const int rc = ::getaddrinfo(host, nullptr, &hints, &results);
  if (rc != 0 || results == nullptr) {
    if (results != nullptr) {
      ::freeaddrinfo(results);
    }
    ec = error_code::resolve_error;
    return false;
  }

  for (addrinfo* node = results; node != nullptr; node = node->ai_next) {
    if (node->ai_addr == nullptr || node->ai_addrlen == 0) {
      continue;
    }
    endpoint candidate = endpoint::from_native(node->ai_addr, static_cast<int>(node->ai_addrlen));
    if (!candidate.valid()) {
      continue;
    }
    candidate.set_port(port);
    out.push_back(candidate);
  }
  ::freeaddrinfo(results);

  if (out.empty()) {
    ec = error_code::resolve_error;
    return false;
  }
  ec = error_code::ok;
  return true;
}

/// Overload for callers that already hold the host as a std::string.
inline bool resolve(const std::string& host, std::uint16_t port, address_family family,
                    std::vector<endpoint>& out, error_code& ec) {
  return resolve(host.c_str(), port, family, out, ec);
}

// ===========================================================================
// Deadlines
// ===========================================================================

/// The largest timeout select(2) can express, in milliseconds.
inline int max_wait_milliseconds() NTP_LITE_NOEXCEPT {
  return 2147483;
}  // INT_MAX / 1000

/// A monotonic point in time, for I/O timeouts and retry budgets.
///
/// An *invalid* deadline (see never()) means "no limit"; every other deadline
/// is a fixed instant on steady_clock, so a wall-clock jump cannot corrupt it.
class deadline {
 public:
  /// A deadline that never expires; its wait timeout is "block indefinitely".
  static deadline never() NTP_LITE_NOEXCEPT { return deadline(); }

  /// A deadline `milliseconds` from now.  Zero or negative produces an
  /// already expired deadline, which is how "do not wait at all" is spelled.
  static deadline after(std::int64_t milliseconds) NTP_LITE_NOEXCEPT {
    deadline result;
    result.valid_ = true;
    result.instant_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    return result;
  }

  /// False only for deadline::never().
  bool valid() const NTP_LITE_NOEXCEPT { return valid_; }

  /// True once the deadline has passed; never true for deadline::never().
  bool expired() const NTP_LITE_NOEXCEPT {
    if (!valid_) {
      return false;
    }
    return std::chrono::steady_clock::now() >= instant_;
  }

  /// Milliseconds left, or -1 when unbounded.  Never negative otherwise: an
  /// expired deadline reports 0.
  std::int64_t remaining_milliseconds() const NTP_LITE_NOEXCEPT {
    if (!valid_) {
      return -1;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now >= instant_) {
      return 0;
    }
    const std::int64_t left =
        std::chrono::duration_cast<std::chrono::milliseconds>(instant_ - now).count();
    // Round up rather than truncate: a deadline half a millisecond away is not
    // "expired", and turning it into a 0 ms wait would spin the caller.
    return left <= 0 ? 1 : left;
  }

  /// The timeout to hand to a wait call: -1 to block until ready, otherwise a
  /// non-negative millisecond budget clamped to what select(2) can express.
  int wait_timeout_ms() const NTP_LITE_NOEXCEPT {
    const std::int64_t remaining = remaining_milliseconds();
    if (remaining < 0) {
      return -1;
    }
    if (remaining > max_wait_milliseconds()) {
      return max_wait_milliseconds();
    }
    return static_cast<int>(remaining);
  }

 private:
  deadline() NTP_LITE_NOEXCEPT : instant_(), valid_(false) {}

  std::chrono::steady_clock::time_point instant_;
  bool valid_;
};

/// Suspends the calling thread for at least `milliseconds`.
///
/// Used for the small pause between retries.  Winsock's select() makes no
/// promise about being called with no descriptor sets at all, so Windows uses
/// Sleep() and POSIX uses select() with three null sets, which POSIX does
/// specify as a plain sleep.
inline void sleep_ms(int milliseconds) {
  if (milliseconds <= 0) {
    return;
  }
#if NTP_LITE_PLATFORM_WINDOWS
  ::Sleep(static_cast<DWORD>(milliseconds));
#else
  timeval timeout;
  std::memset(&timeout, 0, sizeof(timeout));
  timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(milliseconds / 1000);
  timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((milliseconds % 1000) * 1000);
  (void)::select(0, nullptr, nullptr, nullptr, &timeout);
#endif
}

// ===========================================================================
// Waiting for readiness
// ===========================================================================

/// The outcome of waiting for a socket.
enum class wait_result {
  ready = 0,      ///< the requested condition holds
  timed_out = 1,  ///< the timeout elapsed first
  failed = 2      ///< the wait itself failed; `ec` explains
};

/// Waits until `handle` is readable, writable, or `timeout_ms` elapses.
///
/// `timeout_ms` is negative to block until the condition holds.  Interrupted
/// waits are retried against the remaining budget, so a stray signal cannot
/// make a caller give up early.
inline wait_result wait_socket(native_socket handle, bool for_write, int timeout_ms,
                               error_code& ec) NTP_LITE_NOEXCEPT {
  ec = error_code::ok;
  if (!socket_is_valid(handle)) {
    ec = error_code::invalid_argument;
    return wait_result::failed;
  }

  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  for (;;) {
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);

    fd_set* read_ptr = nullptr;
    fd_set* write_ptr = nullptr;
    if (for_write) {
      FD_SET(handle, &write_set);
      write_ptr = &write_set;
    } else {
      FD_SET(handle, &read_set);
      read_ptr = &read_set;
    }

    timeval timeout;
    std::memset(&timeout, 0, sizeof(timeout));
    timeval* timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
      timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
      timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
      timeout_ptr = &timeout;
    }

    // Winsock ignores nfds - a SOCKET is a handle, not a table index - and
    // computing handle + 1 from it would be meaningless.
    int descriptor_count = 0;
#if !NTP_LITE_PLATFORM_WINDOWS
    descriptor_count = static_cast<int>(handle) + 1;
#endif

    const int rc = ::select(descriptor_count, read_ptr, write_ptr, nullptr, timeout_ptr);

    if (rc > 0) {
      return wait_result::ready;
    }
    if (rc == 0) {
      ec = error_code::timeout;
      return wait_result::timed_out;
    }

    const int code = last_socket_error();
    if (!is_interrupted_error(code)) {
      ec = classify_socket_error(code);
      return wait_result::failed;
    }
    if (timeout_ms < 0) {
      continue;  // unbounded: a signal is not a reason to give up
    }
    const std::int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
    const std::int64_t remaining = static_cast<std::int64_t>(timeout_ms) - elapsed;
    if (remaining <= 0) {
      ec = error_code::timeout;
      return wait_result::timed_out;
    }
    timeout_ms = static_cast<int>(remaining);
  }
}

/// Waits until `handle` has data or an error to report.
inline wait_result wait_readable(native_socket handle, int timeout_ms,
                                 error_code& ec) NTP_LITE_NOEXCEPT {
  return wait_socket(handle, false, timeout_ms, ec);
}

/// Waits until `handle` can accept a datagram.
inline wait_result wait_writable(native_socket handle, int timeout_ms,
                                 error_code& ec) NTP_LITE_NOEXCEPT {
  return wait_socket(handle, true, timeout_ms, ec);
}

// ===========================================================================
// udp_socket
// ===========================================================================

/// A UDP socket that owns its handle.
///
/// Move-only: two objects must never own the same handle, because then neither
/// could be closed safely.  Every operation reports failure through an
/// error_code out-parameter instead of throwing, which keeps the whole library
/// usable from exception-free builds.
class udp_socket {
 public:
  udp_socket() NTP_LITE_NOEXCEPT : handle_(invalid_native_socket()) {}

  ~udp_socket() { close(); }

  udp_socket(udp_socket&& other) NTP_LITE_NOEXCEPT : handle_(other.handle_) {
    other.handle_ = invalid_native_socket();
  }

  udp_socket& operator=(udp_socket&& other) NTP_LITE_NOEXCEPT {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = invalid_native_socket();
    }
    return *this;
  }

  udp_socket(const udp_socket&) = delete;
  udp_socket& operator=(const udp_socket&) = delete;

  /// Opens a socket for `family`.  address_family::any falls back to IPv4,
  /// which is what a client that never sees an IP literal wants, and what
  /// every NTP server in the world answers.
  bool open(address_family family, error_code& ec) NTP_LITE_NOEXCEPT {
    close();
    if (!address_family_supported(family)) {
      ec = error_code::unsupported;
      return false;
    }
    if (!socket_runtime_instance().ready()) {
      ec = error_code::network_error;
      return false;
    }
    const int native_family = family == address_family::ipv6 ? AF_INET6 : AF_INET;
    const native_socket handle = ::socket(native_family, SOCK_DGRAM, IPPROTO_UDP);
    if (!socket_is_valid(handle)) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    handle_ = handle;
    ec = error_code::ok;
    return true;
  }

  /// Convenience overload: an IPv4 socket.
  bool open(error_code& ec) NTP_LITE_NOEXCEPT { return open(address_family::ipv4, ec); }

  /// Binds to a local address.  Pass port 0 to let the kernel choose one, then
  /// ask local_endpoint() which one it picked.
  bool bind(const endpoint& local, error_code& ec) NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_) || !local.valid()) {
      ec = error_code::invalid_argument;
      return false;
    }
    if (::bind(handle_, local.native_address(), local.native_length()) != 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    ec = error_code::ok;
    return true;
  }

  /// Associates the socket with a peer, so that send()/receive() need no
  /// address and the kernel reports ICMP errors on the next call.
  bool connect(const endpoint& remote, error_code& ec) NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_) || !remote.valid()) {
      ec = error_code::invalid_argument;
      return false;
    }
    if (::connect(handle_, remote.native_address(), remote.native_length()) != 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    ec = error_code::ok;
    return true;
  }

  /// Allows the address to be reused immediately after close(), so a server
  /// can restart without waiting out the kernel's lingering-address delay.
  bool set_reuse_address(bool enabled, error_code& ec) NTP_LITE_NOEXCEPT {
    const int value = enabled ? 1 : 0;
    if (!set_option(SOL_SOCKET, SO_REUSEADDR, value, ec)) {
      return false;
    }
    ec = error_code::ok;
    return true;
  }

  /// Allows sending to a broadcast address.  Off by default, and only needed
  /// for the (largely obsolete) NTP broadcast mode.
  bool set_broadcast(bool enabled, error_code& ec) NTP_LITE_NOEXCEPT {
    const int value = enabled ? 1 : 0;
    if (!set_option(SOL_SOCKET, SO_BROADCAST, value, ec)) {
      return false;
    }
    ec = error_code::ok;
    return true;
  }

  /// Switches between blocking and non-blocking mode.
  bool set_nonblocking(bool enabled, error_code& ec) NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_)) {
      ec = error_code::invalid_argument;
      return false;
    }
#if NTP_LITE_PLATFORM_WINDOWS
    u_long mode = enabled ? 1UL : 0UL;
    if (::ioctlsocket(handle_, FIONBIO, &mode) != 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
#else
    const int current = ::fcntl(handle_, F_GETFL, 0);
    if (current < 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    const int updated = enabled ? (current | O_NONBLOCK) : (current & ~O_NONBLOCK);
    if (::fcntl(handle_, F_SETFL, updated) != 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
#endif
    ec = error_code::ok;
    return true;
  }

  /// Closes the socket.  Safe on an already closed or never opened object, and
  /// called by the destructor.
  void close() NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_)) {
      return;
    }
#if NTP_LITE_PLATFORM_WINDOWS
    ::closesocket(handle_);
#else
    ::close(handle_);
#endif
    handle_ = invalid_native_socket();
  }

  bool is_open() const NTP_LITE_NOEXCEPT { return socket_is_valid(handle_); }

  native_socket handle() const NTP_LITE_NOEXCEPT { return handle_; }

  /// The address this socket is bound to, as the kernel sees it.
  bool local_endpoint(endpoint& out, error_code& ec) const NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_)) {
      ec = error_code::invalid_argument;
      return false;
    }
    sockaddr_storage storage;
    std::memset(&storage, 0, sizeof(storage));
    socklen_t length = static_cast<socklen_t>(sizeof(storage));
    if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    out = endpoint::from_native(&storage, static_cast<int>(length));
    if (!out.valid()) {
      ec = error_code::internal_error;
      return false;
    }
    ec = error_code::ok;
    return true;
  }

  /// Sends one datagram to the connected peer.
  ///
  /// Reports error_code::internal_error if the kernel accepts only part of the
  /// datagram, which UDP does not allow; that would mean an assumption above
  /// is wrong, and silently reporting success would hide it.
  bool send(const void* data, std::size_t size, error_code& ec) NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_) || data == nullptr) {
      ec = error_code::invalid_argument;
      return false;
    }
    const int sent = ::send(handle_, static_cast<const char*>(data), static_cast<int>(size), 0);
    if (sent < 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    if (static_cast<std::size_t>(sent) != size) {
      ec = error_code::internal_error;
      return false;
    }
    ec = error_code::ok;
    return true;
  }

  /// Sends one datagram to `to`, whether or not the socket is connected.
  bool send_to(const endpoint& to, const void* data, std::size_t size,
               error_code& ec) NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_) || !to.valid() || data == nullptr) {
      ec = error_code::invalid_argument;
      return false;
    }
    const int sent = ::sendto(handle_, static_cast<const char*>(data), static_cast<int>(size), 0,
                              to.native_address(), to.native_length());
    if (sent < 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    if (static_cast<std::size_t>(sent) != size) {
      ec = error_code::internal_error;
      return false;
    }
    ec = error_code::ok;
    return true;
  }

  /// Receives one datagram from the connected peer.
  bool receive(void* data, std::size_t capacity, std::size_t& received,
               error_code& ec) NTP_LITE_NOEXCEPT {
    received = 0;
    if (!socket_is_valid(handle_) || data == nullptr || capacity == 0) {
      ec = error_code::invalid_argument;
      return false;
    }
    const int rc = ::recv(handle_, static_cast<char*>(data), static_cast<int>(capacity), 0);
    if (rc < 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    received = static_cast<std::size_t>(rc);
    ec = error_code::ok;
    return true;
  }

  /// Receives one datagram and reports who sent it.
  ///
  /// A datagram longer than `capacity` is truncated on POSIX and reported as
  /// WSAEMSGSIZE on Windows; either way `received` never exceeds `capacity`,
  /// so callers that care must compare the result with the expected size.
  bool receive_from(void* data, std::size_t capacity, std::size_t& received, endpoint& from,
                    error_code& ec) NTP_LITE_NOEXCEPT {
    received = 0;
    from = endpoint();
    if (!socket_is_valid(handle_) || data == nullptr || capacity == 0) {
      ec = error_code::invalid_argument;
      return false;
    }
    sockaddr_storage storage;
    std::memset(&storage, 0, sizeof(storage));
    socklen_t length = static_cast<socklen_t>(sizeof(storage));
    const int rc = ::recvfrom(handle_, static_cast<char*>(data), static_cast<int>(capacity), 0,
                              reinterpret_cast<sockaddr*>(&storage), &length);
    if (rc < 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    received = static_cast<std::size_t>(rc);
    from = endpoint::from_native(&storage, static_cast<int>(length));
    ec = error_code::ok;
    return true;
  }

 private:
  bool set_option(int level, int name, int value, error_code& ec) NTP_LITE_NOEXCEPT {
    if (!socket_is_valid(handle_)) {
      ec = error_code::invalid_argument;
      return false;
    }
    if (::setsockopt(handle_, level, name, reinterpret_cast<const char*>(&value),
                     static_cast<socklen_t>(sizeof(value))) != 0) {
      ec = classify_socket_error(last_socket_error());
      return false;
    }
    return true;
  }

  native_socket handle_;
};

}  // namespace detail
}  // namespace ntplite

#endif  // NTP_LITE_DETAIL_SOCKET_HPP
