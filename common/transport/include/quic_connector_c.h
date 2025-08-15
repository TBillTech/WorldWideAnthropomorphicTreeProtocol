// Minimal C-compatible facade for QuicConnector to enable Node bindings (N-API/FFI)
// This is a header-only declaration. Implementation will live in a corresponding .cc/.c file.
// The API is intentionally narrow: create a session, open a bidi stream, write/read, and close.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// Opaque handles (do not rely on their internals from C consumers)
typedef struct wwatp_quic_session_t wwatp_quic_session_t;
typedef struct wwatp_quic_stream_t wwatp_quic_stream_t;

// Error codes (0 success; negative values are errors)
enum {
  WWATP_QUIC_OK = 0,
  WWATP_QUIC_ERR_GENERIC = -1,
  WWATP_QUIC_ERR_CONNECT = -2,
  WWATP_QUIC_ERR_IO = -3,
  WWATP_QUIC_ERR_TIMEOUT = -4,
  WWATP_QUIC_ERR_PARAM = -5
};

// Session options (all string pointers may be NULL when not used)
typedef struct {
  const char *url;        // e.g., "https://127.0.0.1:12345"
  const char *authority;  // optional SNI/authority override
  const char *cert_file;  // client certificate (PEM), optional
  const char *key_file;   // client private key (PEM), optional
  const char *ca_file;    // CA bundle (PEM), optional
  int insecure_skip_verify; // 1 to skip verification (dev only)
  uint32_t timeout_ms;      // idle/connect timeout, 0 means default
} wwatp_quic_session_opts_t;

// Create and connect a QUIC (HTTP/3) client session.
// Returns NULL on failure. On success, returns a session handle.
wwatp_quic_session_t *wwatp_quic_create_session(const wwatp_quic_session_opts_t *opts);

// Close and free the session. Safe to pass NULL (no-op).
void wwatp_quic_close_session(wwatp_quic_session_t *session);

// Open a new bidirectional stream associated with an HTTP/3 request path.
// path must be a UTF-8 string like "/init/wwatp/". Returns NULL on failure.
wwatp_quic_stream_t *wwatp_quic_open_bidi_stream(wwatp_quic_session_t *session, const char *path);

// Set the WWATP request signal (e.g., 0x17 for GET_FULL_TREE_REQUEST) for the first
// request emission on this stream. If not set, a default may be used.
void wwatp_quic_stream_set_wwatp_signal(wwatp_quic_stream_t *stream, uint8_t signal);

// Write request bytes on the stream. Returns number of bytes written (>=0) or negative error.
// If end_stream is non-zero, half-closes the send side after write.
int64_t wwatp_quic_stream_write(wwatp_quic_stream_t *stream, const uint8_t *data, size_t len, int end_stream);

// Read response bytes from the stream into buf up to buf_len.
// Returns number of bytes read (>0), 0 on clean end-of-stream, or negative error.
int64_t wwatp_quic_stream_read(wwatp_quic_stream_t *stream, uint8_t *buf, size_t buf_len, uint32_t timeout_ms);

// Gracefully shutdown and free the stream. Safe to pass NULL (no-op).
void wwatp_quic_stream_close(wwatp_quic_stream_t *stream);

// Optional helper to get a simple last error string for diagnostics. Returns a static string.
const char *wwatp_quic_last_error(void);

#ifdef __cplusplus
} // extern "C"
#endif
