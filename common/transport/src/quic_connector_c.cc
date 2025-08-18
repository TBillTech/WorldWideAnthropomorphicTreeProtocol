// Real implementation for the C facade declared in quic_connector_c.h
// This bridges the C API to the C++ QuicConnector using its request/response queues.

#include "quic_connector_c.h"

#include "quic_connector.h"
#include "request.h"
#include "shared_chunk.h"

#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <boost/asio.hpp>
#include <yaml-cpp/yaml.h>

using namespace std;

namespace {
struct ParsedUrl {
  string scheme;
  string host;
  int port{0};
  string path;
};

static inline bool parse_url(const string &url, ParsedUrl &out) {
  // Very small URL parser: scheme://host[:port][/path]
  auto pos_scheme = url.find("://");
  if (pos_scheme == string::npos) return false;
  out.scheme = url.substr(0, pos_scheme);
  auto rest = url.substr(pos_scheme + 3);
  auto pos_path = rest.find('/');
  string hostport = (pos_path == string::npos) ? rest : rest.substr(0, pos_path);
  out.path = (pos_path == string::npos) ? string("/") : rest.substr(pos_path);
  auto pos_colon = hostport.rfind(':');
  if (pos_colon != string::npos && pos_colon != hostport.size() - 1) {
    out.host = hostport.substr(0, pos_colon);
    try {
      out.port = stoi(hostport.substr(pos_colon + 1));
    } catch (...) { return false; }
  } else {
    out.host = hostport;
    out.port = (out.scheme == "https") ? 443 : 80;
  }
  if (out.host.empty()) return false;
  return true;
}

static const char* g_last_error = "";
} // namespace

// Define the opaque structs in global namespace to match header typedefs
struct wwatp_quic_session_t {
  boost::asio::io_context io;
  unique_ptr<QuicConnector> qc;
  string scheme;
  string authority; // host:port
  string host;
  int port{0};
  atomic<uint16_t> next_request_id{2}; // client-chosen WWATP request ids (even preferred)
  thread io_thread;
  atomic<int> open_streams{0};
};

struct wwatp_quic_stream_t {
  wwatp_quic_session_t* session{nullptr};
  StreamIdentifier sid{ngtcp2_cid{}, static_cast<uint16_t>(0)};
  string path;
  uint16_t request_id{0};
  bool closed{false};
  // WWATP request signal to use for payload chunks on this stream.
  // If 0, a sensible default will be used (GET_FULL_TREE_REQUEST).
  uint8_t wwatp_signal{0};

  // Buffers/state bridged by the worker callback
  mutex m;
  vector<uint8_t> outgoing;
  bool half_close{false}; // set when end_stream requested and outgoing drained
  vector<uint8_t> incoming;
  bool remote_eos{false};
  bool sent_any{false}; // whether we've emitted at least one payload chunk
};

namespace {
// Helper: build Request
static inline Request make_request(const wwatp_quic_session_t* sess, const string& path) {
  Request req;
  req.scheme = sess->scheme;
  req.authority = sess->authority;
  req.path = path;
  req.method = "POST";
  req.pri.urgency = 0;
  req.pri.inc = 0;
  return req;
}

// Create a lambda that moves incoming chunks into stream->incoming and returns
// any pending outgoing chunks prepared as WWATP payload frames
static inline stream_callback_fn make_stream_cb(wwatp_quic_stream_t* st) {
  return [st](const StreamIdentifier& /*sid*/, chunks& to_process) -> chunks {
    // 1) Consume incoming into st->incoming
    if (!to_process.empty()) {
      lock_guard<mutex> lk(st->m);
      for (auto &ch : to_process) {
        if (ch.get_signal_type() == payload_chunk_header::GLOBAL_SIGNAL_TYPE) {
          // Append payload bytes
          for (auto b : ch.range<uint8_t>()) {
            st->incoming.push_back(b);
          }
          // If server signals FINAL, note EOS
          if (ch.get_signal_signal() == payload_chunk_header::SIGNAL_WWATP_RESPONSE_FINAL) {
            st->remote_eos = true;
          }
        } else if (ch.get_signal_type() == signal_chunk_header::GLOBAL_SIGNAL_TYPE) {
          auto sig = ch.get_signal<signal_chunk_header>().signal;
          if (sig == signal_chunk_header::SIGNAL_CLOSE_STREAM) {
            st->remote_eos = true;
          }
        }
      }
    }

    // 2) Produce outgoing if available
    chunks produced;
    vector<uint8_t> send_now;
    bool mark_final = false;
    {
      lock_guard<mutex> lk(st->m);
      if (!st->outgoing.empty()) {
        send_now.swap(st->outgoing);
        mark_final = st->half_close; // if half_close set and we're draining all
        // We will keep half_close true so no more writes after drain
      } else if (st->half_close && !st->sent_any) {
        // Allow zero-length final requests (e.g., GET_FULL_TREE has no body).
        mark_final = true;
      }
    }
    // Choose the WWATP request signal to use for outgoing payload chunks
    uint8_t req_signal = st->wwatp_signal ? st->wwatp_signal
                                          : payload_chunk_header::SIGNAL_WWATP_GET_FULL_TREE_REQUEST;
    if (!send_now.empty()) {
      payload_chunk_header hdr(st->request_id, req_signal, 0 /* set by flatten */);
      shared_span<> composed(hdr, std::span<const uint8_t>(send_now.data(), send_now.size()));
      auto vec = composed.flatten_with_signal(hdr);
      for (auto &sp : vec) produced.emplace_back(sp);
      st->sent_any = true;
    } else if (mark_final) {
      // Send a zero-length payload chunk carrying the request signal
      payload_chunk_header hdr(st->request_id, req_signal, 0);
      shared_span<> composed(hdr, std::span<const uint8_t>());
      auto vec = composed.flatten_with_signal(hdr);
      for (auto &sp : vec) produced.emplace_back(sp);
      st->sent_any = true;
    }
    return produced;
  };
}
} // namespace

extern "C" {

wwatp_quic_session_t* wwatp_quic_create_session(const wwatp_quic_session_opts_t* opts) {
  if (!opts || !opts->url) { g_last_error = "invalid opts/url"; return nullptr; }
  if (!opts->cert_file || !opts->key_file) { g_last_error = "missing cert_file/key_file"; return nullptr; }

  ParsedUrl u{};
  if (!parse_url(opts->url, u)) { g_last_error = "invalid url"; return nullptr; }

  auto* s = new (nothrow) wwatp_quic_session_t();
  if (!s) { g_last_error = "oom"; return nullptr; }
  s->scheme = u.scheme;
  s->host = u.host;
  s->port = u.port;
  s->authority = u.host + string(":") + to_string(u.port);

  try {
    YAML::Node cfg;
    cfg["private_key_file"] = string(opts->key_file);
    cfg["cert_file"] = string(opts->cert_file);
  // Provide required fields for QuicConnector initialization
  // Use a temp directory for logs by default; can be overridden in future opts
  cfg["log_path"] = string("/tmp/wwatp_quic");
    // Use reasonable defaults; QuicConnector will validate required fields
    s->qc = make_unique<QuicConnector>(s->io, cfg);
  } catch (const std::exception& e) {
    g_last_error = "QuicConnector init failed";
    delete s;
    return nullptr;
  }

  // Kick an IO thread just in case timers/sockets are used
  try {
    s->io_thread = std::thread([&io = s->io]() { io.run(); });
  } catch (...) {
    g_last_error = "io thread start failed";
    delete s;
    return nullptr;
  }

  // Start QUIC connection in its own thread (managed by QuicConnector)
  try {
    s->qc->connect(s->host, s->host, s->port);
  } catch (...) {
    g_last_error = "connect failed";
    if (s->io_thread.joinable()) s->io.stop(), s->io_thread.join();
    delete s;
    return nullptr;
  }

  // Give the connection thread a brief moment to initialize the Client
  // object before first stream operations to avoid races.
  // TODO: Expose a proper readiness signal from QuicConnector.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  return s;
}

void wwatp_quic_close_session(wwatp_quic_session_t* session) {
  if (!session) return;
  // Best-effort close
  try { if (session->qc) session->qc->close(); } catch (...) {}
  try { session->io.stop(); } catch (...) {}
  if (session->io_thread.joinable()) {
    try { session->io_thread.join(); } catch (...) {}
  }
  delete session;
}

wwatp_quic_stream_t* wwatp_quic_open_bidi_stream(wwatp_quic_session_t* session, const char* path) {
  if (!session || !path) { g_last_error = "invalid session/path"; return nullptr; }
  auto* st = new (nothrow) wwatp_quic_stream_t();
  if (!st) { g_last_error = "oom"; return nullptr; }
  st->session = session;
  st->path = path;

  // Register response handler so processRequestStream can pump
  try {
    Request req = make_request(session, st->path);
  st->sid = session->qc->getNewRequestStreamIdentifier(req);
  // Server expects request_id to equal stream logical_id for WWATP chunk streams
  st->request_id = st->sid.logical_id;
    session->qc->registerResponseHandler(st->sid, make_stream_cb(st));
  } catch (...) {
    delete st;
    g_last_error = "open stream failed";
    return nullptr;
  }
  session->open_streams.fetch_add(1);
  return st;
}

int64_t wwatp_quic_stream_write(wwatp_quic_stream_t* stream, const uint8_t* data, size_t len, int end_stream) {
  if (!stream || (!data && len)) { g_last_error = "invalid stream/data"; return WWATP_QUIC_ERR_PARAM; }
  if (stream->closed) { g_last_error = "stream closed"; return WWATP_QUIC_ERR_IO; }

  {
    lock_guard<mutex> lk(stream->m);
    if (len > 0 && data) {
      stream->outgoing.insert(stream->outgoing.end(), data, data + len);
    }
    if (end_stream) stream->half_close = true;
  }

  // Pump once to hand data to IO thread via callback
  try {
    (void)stream->session->qc->processRequestStream();
  } catch (...) {
    g_last_error = "write pump failed";
    return WWATP_QUIC_ERR_IO;
  }

  return static_cast<int64_t>(len);
}

int64_t wwatp_quic_stream_read(wwatp_quic_stream_t* stream, uint8_t* buf, size_t buf_len, uint32_t timeout_ms) {
  if (!stream || !buf) { g_last_error = "invalid stream/buf"; return WWATP_QUIC_ERR_PARAM; }
  using namespace std::chrono;
  auto start = steady_clock::now();

  for (;;) {
    // First, see if we already have bytes
    {
      lock_guard<mutex> lk(stream->m);
      if (!stream->incoming.empty()) {
        size_t n = min(buf_len, stream->incoming.size());
        memcpy(buf, stream->incoming.data(), n);
        stream->incoming.erase(stream->incoming.begin(), stream->incoming.begin() + n);
        return static_cast<int64_t>(n);
      }
      if (stream->remote_eos) return 0; // clean EOS
    }

    // Pump once; if nothing arrives, optionally wait
    try {
      bool did = stream->session->qc->processRequestStream();
      (void)did;
    } catch (...) {
      g_last_error = "read pump failed";
      return WWATP_QUIC_ERR_IO;
    }

    {
      lock_guard<mutex> lk(stream->m);
      if (!stream->incoming.empty()) continue; // loop to deliver
      if (stream->remote_eos) return 0;
    }

    if (timeout_ms == 0) {
      // Non-blocking: nothing available now
      return 0;
    }
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    if (elapsed >= timeout_ms) {
      return WWATP_QUIC_ERR_TIMEOUT;
    }
    this_thread::sleep_for(chrono::milliseconds(5));
  }
}

void wwatp_quic_stream_close(wwatp_quic_stream_t* stream) {
  if (!stream) return;
  // If we've already closed, do nothing. Calling close twice should be a no-op.
  // Note: The caller MUST NOT call this function more than once for the same pointer;
  // this guard prevents an explicit double-delete path but cannot make use-after-free safe.
  if (stream->closed) { return; }
  stream->closed = true;
  // Best-effort deregistration; ignore errors
  try { stream->session->qc->deregisterResponseHandler(stream->sid); } catch (...) {}
  stream->session->open_streams.fetch_sub(1);
  delete stream;
}

const char* wwatp_quic_last_error(void) {
  return g_last_error;
}

void wwatp_quic_stream_set_wwatp_signal(wwatp_quic_stream_t *stream, uint8_t signal) {
  if (!stream) { g_last_error = "invalid stream"; return; }
  stream->wwatp_signal = signal;
}

} // extern "C"
