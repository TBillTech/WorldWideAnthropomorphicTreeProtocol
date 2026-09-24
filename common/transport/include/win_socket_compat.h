/*
 * Windows/Winsock compatibility shim for the POSIX msghdr/cmsghdr/recvmsg/sendmsg
 * API used throughout common/transport (quic_connector.cpp, quic_listener.cpp,
 * shared.cc). This lets those files keep their existing POSIX-shaped call sites
 * (recvmsg/sendmsg/CMSG_*) largely unchanged on Windows, translating underneath
 * to WSARecvMsg/WSASendMsg.
 *
 * Only ever included when _WIN32 is defined (see network.h / util.h / quic_*.cpp
 * #ifdef blocks); the POSIX headers/declarations remain untouched on other platforms.
 *
 * Known simplification: this codebase stores socket handles in plain `int fd`
 * fields (Endpoint::fd etc.), matching Linux fds. Windows' SOCKET is a 64-bit
 * unsigned handle; storing it in `int` truncates it. In practice this works
 * (INVALID_SOCKET's low 32 bits still compare equal to -1, and real Winsock
 * handles observed in this codebase's usage fit in 32 bits), but a real handle
 * collision after truncation is a theoretical risk this port accepts rather
 * than widening `fd` to a platform-dependent type throughout the codebase.
 */
#pragma once

#ifdef _WIN32

//#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#  define NOMINMAX
#endif // !defined(NOMINMAX)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
// if_indextoname (used by quic_listener.cpp's multi-homing logging). Deliberately
// <netioapi.h> rather than the full <iphlpapi.h>: the latter drags in COM/RPC headers
// (rpcndr.h) whose `byte` typedef collides with `std::byte` wherever `using namespace
// std` is in scope (still requires linking -liphlpapi; the function lives in that DLL).
//#include <netioapi.h>

// wincrypt.h (pulled in above regardless of NOCRYPT/WIN32_LEAN_AND_MEAN, since some MinGW
// headers include it directly rather than gating it through windows.h's own guard) defines
// legacy CryptoAPI macros that collide with BoringSSL's identically-named OpenSSL-compatible
// types/functions. Undefine them here, before any BoringSSL header can be reached.
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_SIGNER_INFO
#undef PKCS7_ISSUER_AND_SERIAL

#include <cstddef>
#include <cstdint>

namespace ngtcp2 {

// Windows doesn't set errno for socket API failures (use WSAGetLastError() instead);
// this translates the common codes into their MinGW <errno.h> equivalents so existing
// call sites written against errno (EAGAIN/EWOULDBLOCK/EINTR/...) keep working unchanged.
void wwatp_set_errno_from_wsa_last_error();

// One-time WSAStartup(); safe to call repeatedly (subsequent calls are no-ops).
void wwatp_ensure_winsock_initialized();

// Socket-handle close. Kept as a distinctly named helper (rather than a blanket
// `#define close closesocket`) because this translation unit may also close plain
// CRT file descriptors, which must keep using ::close().
inline int wwatp_close_socket(int fd) {
  return ::closesocket(static_cast<SOCKET>(fd));
}

} // namespace ngtcp2

// Global-scope, exact-`int`-fd overloads of the BSD socket API used throughout
// common/transport. These exist for two reasons, not just SOCKET/const-char*
// signature mismatches:
//  - Winsock's setsockopt/getsockopt take `const char*`/`char*` optval, not
//    `void*`, so the existing `&val` (int*) call sites don't match it directly.
//  - Without an exact `int`-typed `bind`/`connect` overload here, unqualified
//    `bind(fd, addr, addrlen)` calls resolve to std::bind (visible via `using
//    namespace std`) instead of the socket call: std::bind's template is a
//    perfect-match (no conversions) for any arguments, while Winsock's
//    `bind(SOCKET, ...)` needs an int->SOCKET conversion, so std::bind wins
//    overload resolution. On POSIX this ambiguity doesn't occur because the fd
//    argument types already match exactly. Providing an exact `int` overload
//    here restores that same "no conversions needed" tie, so plain
//    (non-template) overload resolution correctly prefers it, matching Linux.
inline int setsockopt(int fd, int level, int optname, const void *optval,
                      int optlen) {
  return ::setsockopt(static_cast<SOCKET>(fd), level, optname,
                       static_cast<const char *>(optval), optlen);
}

inline int getsockopt(int fd, int level, int optname, void *optval,
                      int *optlen) {
  return ::getsockopt(static_cast<SOCKET>(fd), level, optname,
                       static_cast<char *>(optval), optlen);
}

inline int bind(int fd, const sockaddr *addr, int addrlen) {
  return ::bind(static_cast<SOCKET>(fd), addr, addrlen);
}

// Windows' own `addrinfo::ai_addrlen` is `size_t` (unlike POSIX's socklen_t),
// so an additional exact-match overload is needed for call sites passing that
// field directly, for the same std::bind-shadowing reason described above.
inline int bind(int fd, const sockaddr *addr, size_t addrlen) {
  return ::bind(static_cast<SOCKET>(fd), addr, static_cast<int>(addrlen));
}

inline int connect(int fd, const sockaddr *addr, int addrlen) {
  return ::connect(static_cast<SOCKET>(fd), addr, addrlen);
}

inline int connect(int fd, const sockaddr *addr, size_t addrlen) {
  return ::connect(static_cast<SOCKET>(fd), addr, static_cast<int>(addrlen));
}

inline int getsockname(int fd, sockaddr *addr, int *addrlen) {
  return ::getsockname(static_cast<SOCKET>(fd), addr, addrlen);
}

// Note: deliberately no wrapper/macro for socket() itself: it collides
// textually with boost::asio's own socket() if macro'd, and the existing
// `auto fd = socket(...); if (fd == -1)` call sites already work correctly
// as-is (SOCKET(-1) == INVALID_SOCKET, and the SOCKET->int narrowing at the
// point it's stored into an `int fd` is an accepted, documented simplification
// of this port -- see the top of win_socket_compat.h).

// UNTESTED (server-side multi-homing, see quic_listener.cpp comments): unlike
// Linux, Windows has no separate "enable receiving packet info" option --
// IPV6_PKTINFO both requests ancillary destination-address info and is the
// cmsg_type used to read it back.
#ifndef IPV6_RECVPKTINFO
#  define IPV6_RECVPKTINFO IPV6_PKTINFO
#endif

// UNTESTED (server-side multi-homing): Windows' <iphlpapi.h>/<netioapi.h>
// define IF_NAMESIZE too, but pull it in defensively in case it isn't
// transitively included here.
#ifndef IF_NAMESIZE
#  define IF_NAMESIZE 128
#endif

// ---- POSIX-shaped iovec/msghdr/cmsghdr, mirroring glibc's layout/macros ----
// (WSAMSG/WSABUF use different field names, so we define our own rather than
// trying to alias Windows' native ws2def.h WSAMSG type.)

struct iovec {
  void *iov_base;
  size_t iov_len;
};

// mswsock.h already defines a `cmsghdr` (aliased from its native WSACMSGHDR,
// with the same cmsg_len/cmsg_level/cmsg_type layout), so reuse it here rather
// than redeclaring it. Its CMSG_* macros operate on WSAMSG (Control.buf/len),
// though, so those are redefined below to work with our POSIX-shaped msghdr.

struct msghdr {
  void *msg_name;
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  size_t msg_iovlen;
  void *msg_control;
  size_t msg_controllen;
  int msg_flags;
};

#undef CMSG_FIRSTHDR
#undef CMSG_NXTHDR
#undef CMSG_SPACE
#undef CMSG_LEN
#undef CMSG_DATA

#define WWATP_CMSG_ALIGN(len) \
  (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) \
  (WWATP_CMSG_ALIGN(sizeof(cmsghdr)) + WWATP_CMSG_ALIGN(len))
#define CMSG_LEN(len) (WWATP_CMSG_ALIGN(sizeof(cmsghdr)) + (len))
#define CMSG_FIRSTHDR(mhdr) \
  ((mhdr)->msg_controllen >= sizeof(cmsghdr) \
       ? reinterpret_cast<cmsghdr *>((mhdr)->msg_control) \
       : nullptr)
#define CMSG_DATA(cmsg) \
  (reinterpret_cast<unsigned char *>(cmsg) + WWATP_CMSG_ALIGN(sizeof(cmsghdr)))
#define CMSG_NXTHDR(mhdr, cmsg) ngtcp2::wwatp_cmsg_nxthdr(mhdr, cmsg)

namespace ngtcp2 {

inline cmsghdr *wwatp_cmsg_nxthdr(msghdr *mhdr, cmsghdr *cmsg) {
  auto *ctrl_end =
      reinterpret_cast<unsigned char *>(mhdr->msg_control) + mhdr->msg_controllen;
  auto *next = reinterpret_cast<cmsghdr *>(
      reinterpret_cast<unsigned char *>(cmsg) + WWATP_CMSG_ALIGN(cmsg->cmsg_len));
  if (reinterpret_cast<unsigned char *>(next) + sizeof(cmsghdr) >
      ctrl_end) {
    return nullptr;
  }
  return next;
}

// Windows' IN_PKTINFO/IN6_PKTINFO use the same field names (ipi_addr/ipi_ifindex,
// ipi6_addr/ipi6_ifindex) as Linux's in_pktinfo/in6_pktinfo, so callers can stay
// textually unchanged behind these aliases.
using in_pktinfo = IN_PKTINFO;
using in6_pktinfo = IN6_PKTINFO;

// recvmsg/sendmsg implemented in win_socket_compat.cc via WSARecvMsg/WSASendMsg.
ssize_t recvmsg(int fd, msghdr *msg, int flags);
ssize_t sendmsg(int fd, const msghdr *msg, int flags);

} // namespace ngtcp2

using ngtcp2::recvmsg;
using ngtcp2::sendmsg;

#endif // defined(_WIN32)
