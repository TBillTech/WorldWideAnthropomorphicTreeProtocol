/*
 * Implementation of the Windows recvmsg/sendmsg/errno shims declared in
 * win_socket_compat.h. See that header for the rationale.
 */
#ifdef _WIN32

#include "win_socket_compat.h"

#include <cerrno>
#include <vector>

namespace ngtcp2 {

void wwatp_ensure_winsock_initialized() {
  static bool initialized = [] {
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    return true;
  }();
  (void)initialized;
}

void wwatp_set_errno_from_wsa_last_error() {
  switch (WSAGetLastError()) {
  case WSAEWOULDBLOCK:
    errno = EWOULDBLOCK;
    break;
  case WSAEINTR:
    errno = EINTR;
    break;
  case WSAECONNRESET:
    errno = ECONNRESET;
    break;
  case WSAEMSGSIZE:
    errno = EMSGSIZE;
    break;
  case WSAENOTSOCK:
    errno = ENOTSOCK;
    break;
  case WSAEINVAL:
    errno = EINVAL;
    break;
  case WSAENETUNREACH:
    errno = ENETUNREACH;
    break;
  case WSAEHOSTUNREACH:
    errno = EHOSTUNREACH;
    break;
  default:
    errno = EIO;
    break;
  }
}

namespace {

// WSARecvMsg is an extension function; it must be looked up per-socket via
// WSAIoctl rather than linked directly (unlike WSASendMsg, which is exported).
LPFN_WSARECVMSG get_wsa_recvmsg_fn(SOCKET fd) {
  static LPFN_WSARECVMSG fn = nullptr;
  if (fn) {
    return fn;
  }
  GUID guid = WSAID_WSARECVMSG;
  DWORD bytes = 0;
  if (WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
               &fn, sizeof(fn), &bytes, nullptr, nullptr) != 0) {
    fn = nullptr;
  }
  return fn;
}

std::vector<WSABUF> to_wsabufs(iovec *iov, size_t iovlen) {
  std::vector<WSABUF> bufs(iovlen);
  for (size_t i = 0; i < iovlen; ++i) {
    bufs[i].buf = static_cast<CHAR *>(iov[i].iov_base);
    bufs[i].len = static_cast<ULONG>(iov[i].iov_len);
  }
  return bufs;
}

} // namespace

ssize_t recvmsg(int fd, msghdr *msg, int /* flags */) {
  wwatp_ensure_winsock_initialized();

  auto sock = static_cast<SOCKET>(fd);
  auto recvmsg_fn = get_wsa_recvmsg_fn(sock);
  if (!recvmsg_fn) {
    errno = ENOSYS;
    return -1;
  }

  auto bufs = to_wsabufs(msg->msg_iov, msg->msg_iovlen);

  WSAMSG wsamsg{};
  wsamsg.name = static_cast<LPSOCKADDR>(msg->msg_name);
  wsamsg.namelen = msg->msg_namelen;
  wsamsg.lpBuffers = bufs.data();
  wsamsg.dwBufferCount = static_cast<DWORD>(bufs.size());
  wsamsg.Control.buf = static_cast<CHAR *>(msg->msg_control);
  wsamsg.Control.len = static_cast<ULONG>(msg->msg_controllen);

  DWORD nread = 0;
  if (recvmsg_fn(sock, &wsamsg, &nread, nullptr, nullptr) != 0) {
    wwatp_set_errno_from_wsa_last_error();
    return -1;
  }

  msg->msg_namelen = wsamsg.namelen;
  msg->msg_controllen = wsamsg.Control.len;
  msg->msg_flags = static_cast<int>(wsamsg.dwFlags);

  return static_cast<ssize_t>(nread);
}

ssize_t sendmsg(int fd, const msghdr *msg, int /* flags */) {
  wwatp_ensure_winsock_initialized();

  auto bufs = to_wsabufs(msg->msg_iov, msg->msg_iovlen);

  WSAMSG wsamsg{};
  wsamsg.name = static_cast<LPSOCKADDR>(msg->msg_name);
  wsamsg.namelen = msg->msg_namelen;
  wsamsg.lpBuffers = bufs.data();
  wsamsg.dwBufferCount = static_cast<DWORD>(bufs.size());
  wsamsg.Control.buf = static_cast<CHAR *>(msg->msg_control);
  wsamsg.Control.len = static_cast<ULONG>(msg->msg_controllen);

  DWORD nsent = 0;
  if (WSASendMsg(static_cast<SOCKET>(fd), &wsamsg, 0, &nsent, nullptr,
                 nullptr) != 0) {
    wwatp_set_errno_from_wsa_last_error();
    return -1;
  }

  return static_cast<ssize_t>(nsent);
}

} // namespace ngtcp2

#endif // defined(_WIN32)
