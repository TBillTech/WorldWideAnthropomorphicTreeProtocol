// Stub implementation for the C facade declared in quic_connector_c.h
// This version only provides no-op behavior sufficient to link and load a shared library.
// Replace with real QuicConnector-backed logic.

#include "quic_connector_c.h"

#include <atomic>
#include <string>
#include <vector>

struct wwatp_quic_session_t {
  std::string url;
  std::atomic<int> open_streams{0};
};

struct wwatp_quic_stream_t {
  wwatp_quic_session_t* session;
  std::vector<uint8_t> buffer; // echo buffer for stub
  bool closed{false};
};

static const char* g_last_error = "";

extern "C" {

wwatp_quic_session_t* wwatp_quic_create_session(const wwatp_quic_session_opts_t* opts) {
  if (!opts || !opts->url) { g_last_error = "invalid opts/url"; return nullptr; }
  auto* s = new wwatp_quic_session_t();
  s->url = opts->url;
  return s;
}

void wwatp_quic_close_session(wwatp_quic_session_t* session) {
  if (!session) return;
  delete session;
}

wwatp_quic_stream_t* wwatp_quic_open_bidi_stream(wwatp_quic_session_t* session, const char* path) {
  if (!session || !path) { g_last_error = "invalid session/path"; return nullptr; }
  auto* st = new wwatp_quic_stream_t();
  st->session = session;
  session->open_streams.fetch_add(1);
  (void)path; // unused in stub
  return st;
}

int64_t wwatp_quic_stream_write(wwatp_quic_stream_t* stream, const uint8_t* data, size_t len, int end_stream) {
  if (!stream || !data) { g_last_error = "invalid stream/data"; return WWATP_QUIC_ERR_PARAM; }
  if (stream->closed) { g_last_error = "stream closed"; return WWATP_QUIC_ERR_IO; }
  // Stub: append to buffer and optionally mark closed for writing
  stream->buffer.insert(stream->buffer.end(), data, data + len);
  (void)end_stream;
  return static_cast<int64_t>(len);
}

int64_t wwatp_quic_stream_read(wwatp_quic_stream_t* stream, uint8_t* buf, size_t buf_len, uint32_t /*timeout_ms*/) {
  if (!stream || !buf) { g_last_error = "invalid stream/buf"; return WWATP_QUIC_ERR_PARAM; }
  if (stream->buffer.empty()) return 0; // EOS for stub once consumed
  size_t n = stream->buffer.size();
  if (n > buf_len) n = buf_len;
  std::copy(stream->buffer.begin(), stream->buffer.begin() + n, buf);
  stream->buffer.erase(stream->buffer.begin(), stream->buffer.begin() + n);
  return static_cast<int64_t>(n);
}

void wwatp_quic_stream_close(wwatp_quic_stream_t* stream) {
  if (!stream) return;
  stream->closed = true;
  if (stream->session) stream->session->open_streams.fetch_sub(1);
  delete stream;
}

const char* wwatp_quic_last_error(void) {
  return g_last_error;
}

} // extern "C"
