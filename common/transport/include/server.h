/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include <deque>
#include <string_view>
#include <memory>
#include <span>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <nghttp3/nghttp3.h>

#include <ev.h>

#include "server_base.h"
#include "tls_server_context_boringssl.h"
#include "network.h"
#include "shared.h"
#include "util.h"
#include "shared_chunk.h"

using namespace ngtcp2;

struct HTTPHeader {
  HTTPHeader(const std::string_view &name, const std::string_view &value)
    : name(name), value(value) {}

  std::string_view name;
  std::string_view value;
};

class Handler;
struct FileEntry;

class StreamIdentifier;

struct Stream {
    Stream(int64_t stream_id, Handler *handler);

    int start_response(nghttp3_conn *conn);
    std::pair<FileEntry, int> open_file(const std::string &path);
    void map_file(const FileEntry &fe);
    int send_status_response(nghttp3_conn *conn, unsigned int status_code,
                            const std::vector<HTTPHeader> &extra_headers = {});
    int send_redirect_response(nghttp3_conn *conn, unsigned int status_code,
                                const std::string_view &path);
    int64_t find_dyn_length(const Request &path);
    void append_data(std::span<const uint8_t> data);
    void http_acked_stream_data(uint64_t datalen);
    Request &get_request() { 
        assert(req.method == method);
        return req;}

    int64_t stream_id;
    Handler *handler;
    // uri stores the request URI/path, which is the target resource of the HTTP request.
    std::string uri; // Example: https://www.example.com:443/path/to/resource?query=example#fragment
    // authority stores the authority component of the request URI, typically the host and port.
    std::string authority; // Example: www.example.com:443
    // method is the HTTP method (e.g., GET, POST) of the request.
    std::string method; // Example: GET
    Request req;
    // status_resp_body stores the body of the HTTP response for status responses.
    // It is used to send predefined status messages (e.g., 404 Not Found).
    std::string status_resp_body; // Example: "404 Not Found"
    // data is a pointer to the memory which maps file denoted by fd.
    uint8_t *data;
    // datalen is the length of mapped file by data.
    uint64_t datalen;
    // dynresp is true if dynamic data response is enabled.
    bool dynresp;
    // dyndataleft is the number of dynamic data left to send.
    uint64_t dyndataleft;
    // dynbuflen is the number of bytes in-flight.
    uint64_t dynbuflen;
    // live_stream is true if the stream length is undefined, and the stream is expected to continue indefinitely.
    bool live_stream;
    // Span for caching partial chunks
    shared_span<> partial_chunk;
    // Has sent trailers?
    bool trailers_sent;
};

class Server;

// Endpoint is a local endpoint.
struct Endpoint {
  Address addr;
  ev_io rev;
  Server *server;
  int fd;
};

class Handler : public HandlerBase {
public:
  Handler(struct ev_loop *loop, Server *server);
  ~Handler();

  int init(const Endpoint &ep, const Address &local_addr, const sockaddr *sa,
           socklen_t salen, const ngtcp2_cid *dcid, const ngtcp2_cid *scid,
           const ngtcp2_cid *ocid, std::span<const uint8_t> token,
           ngtcp2_token_type token_type, uint32_t version,
           TLSServerContext &tls_ctx);

  int on_read(const Endpoint &ep, const Address &local_addr, const sockaddr *sa,
              socklen_t salen, const ngtcp2_pkt_info *pi,
              std::span<const uint8_t> data);
  int on_write();
  int write_streams();
  int feed_data(const Endpoint &ep, const Address &local_addr,
                const sockaddr *sa, socklen_t salen, const ngtcp2_pkt_info *pi,
                std::span<const uint8_t> data);
  void update_timer();
  int handle_expiry();
  void signal_write();
  int handshake_completed();

  Server *server();
  int recv_stream_data(uint32_t flags, int64_t stream_id,
                       std::span<const uint8_t> data);
  int acked_stream_data_offset(int64_t stream_id, uint64_t datalen);
  uint32_t version() const;
  void on_stream_open(int64_t stream_id);
  int on_stream_close(int64_t stream_id, uint64_t app_error_code);
  void start_draining_period();
  int start_closing_period();
  int handle_error();
  int send_conn_close();

  int update_key(uint8_t *rx_secret, uint8_t *tx_secret,
                 ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
                 ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
                 const uint8_t *current_rx_secret,
                 const uint8_t *current_tx_secret, size_t secretlen);

  int setup_httpconn();
  void http_consume(int64_t stream_id, size_t nconsumed);
  void extend_max_remote_streams_bidi(uint64_t max_streams);
  Stream *find_stream(int64_t stream_id);
  void http_begin_request_headers(int64_t stream_id);
  void http_recv_request_header(Stream *stream, int32_t token,
                                nghttp3_rcbuf *name, nghttp3_rcbuf *value);
  int http_end_request_headers(Stream *stream);
  int http_end_stream(Stream *stream);
  int start_response(Stream *stream);
  int on_stream_reset(int64_t stream_id);
  int on_stream_stop_sending(int64_t stream_id);
  int extend_max_stream_data(int64_t stream_id, uint64_t max_data);
  void shutdown_read(int64_t stream_id, int app_error_code);
  void http_acked_stream_data(Stream *stream, uint64_t datalen);
  void http_stream_close(int64_t stream_id, uint64_t app_error_code);
  int http_stop_sending(int64_t stream_id, uint64_t app_error_code);
  int http_reset_stream(int64_t stream_id, uint64_t app_error_code);

  void write_qlog(const void *data, size_t datalen);

  void on_send_blocked(Endpoint &ep, const ngtcp2_addr &local_addr,
                       const ngtcp2_addr &remote_addr, unsigned int ecn,
                       std::span<const uint8_t> data, size_t gso_size);
  void start_wev_endpoint(const Endpoint &ep);
  int send_blocked_packet();
  ngtcp2_cid const &get_scid() const { return scid_; }

  bool lock_outgoing_chunks(const vector<StreamIdentifier>& sids, nghttp3_vec *vec, size_t veccnt);
  pair<size_t, vector<StreamIdentifier>> get_pending_chunks_size(int64_t stream_id, size_t veccnt);
  void unlock_chunks(nghttp3_vec *vec, size_t veccnt);
  void shared_span_incr_rc(uint8_t *locked_ptr, shared_span<> &&to_lock);
  void shared_span_decr_rc(uint8_t *locked_ptr);

  void push_incoming_chunk(const Request& req, shared_span<> &&chunk);

  void writecb_start();

private:
  struct ev_loop *loop_;
  Server *server_;
  ev_io wev_;
  ev_timer timer_;
  FILE *qlog_;
  ngtcp2_cid scid_;
  nghttp3_conn *httpconn_;
  std::unordered_map<int64_t, std::unique_ptr<Stream>> streams_;
  // conn_closebuf_ contains a packet which contains CONNECTION_CLOSE.
  // This packet is repeatedly sent as a response to the incoming
  // packet in draining period.
  std::unique_ptr<Buffer> conn_closebuf_;
  // nkey_update_ is the number of key update occurred.
  size_t nkey_update_;
  bool no_gso_;
  map<uint8_t *, shared_span<>> locked_chunks_;

  struct {
    bool send_blocked;
    size_t num_blocked;
    size_t num_blocked_sent;
    // blocked field is effective only when send_blocked is true.
    struct {
      Endpoint *endpoint;
      Address local_addr;
      Address remote_addr;
      unsigned int ecn;
      std::span<const uint8_t> data;
      size_t gso_size;
    } blocked[2];
    std::unique_ptr<uint8_t[]> data;
  } tx_;
};

class QuicListener;

class Server {
public:
  Server(struct ev_loop *loop, TLSServerContext &tls_ctx, QuicListener &listener);
  ~Server();

  int init(const char *addr, const char *port);
  void disconnect();
  void close();

  int on_read(Endpoint &ep);
  void read_pkt(Endpoint &ep, const Address &local_addr, const sockaddr *sa,
                socklen_t salen, const ngtcp2_pkt_info *pi,
                std::span<const uint8_t> data);
  int send_version_negotiation(uint32_t version, std::span<const uint8_t> dcid,
                               std::span<const uint8_t> scid, Endpoint &ep,
                               const Address &local_addr, const sockaddr *sa,
                               socklen_t salen);
  int send_retry(const ngtcp2_pkt_hd *chd, Endpoint &ep,
                 const Address &local_addr, const sockaddr *sa, socklen_t salen,
                 size_t max_pktlen);
  int send_stateless_connection_close(const ngtcp2_pkt_hd *chd, Endpoint &ep,
                                      const Address &local_addr,
                                      const sockaddr *sa, socklen_t salen);
  int send_stateless_reset(size_t pktlen, std::span<const uint8_t> dcid,
                           Endpoint &ep, const Address &local_addr,
                           const sockaddr *sa, socklen_t salen);
  int verify_retry_token(ngtcp2_cid *ocid, const ngtcp2_pkt_hd *hd,
                         const sockaddr *sa, socklen_t salen);
  int verify_token(const ngtcp2_pkt_hd *hd, const sockaddr *sa,
                   socklen_t salen);
  int send_packet(Endpoint &ep, const ngtcp2_addr &local_addr,
                  const ngtcp2_addr &remote_addr, unsigned int ecn,
                  std::span<const uint8_t> data);
  std::pair<std::span<const uint8_t>, int>
  send_packet(Endpoint &ep, bool &no_gso, const ngtcp2_addr &local_addr,
              const ngtcp2_addr &remote_addr, unsigned int ecn,
              std::span<const uint8_t> data, size_t gso_size);
  void remove(const Handler *h);

  void associate_cid(const ngtcp2_cid *cid, Handler *h);
  void dissociate_cid(const ngtcp2_cid *cid);

  void on_stateless_reset_regen();

  QuicListener &listener() { return listener_; }

  void writecb_start(StreamIdentifier sid);

private:
  std::unordered_map<ngtcp2_cid, Handler *> handlers_;
  struct ev_loop *loop_;
  std::vector<Endpoint> endpoints_;
  TLSServerContext &tls_ctx_;
  ev_signal sigintev_;
  ev_timer stateless_reset_regen_timer_;
  size_t stateless_reset_bucket_;
  QuicListener &listener_;
};
