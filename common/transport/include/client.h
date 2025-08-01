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
#include <deque>
#include <map>
#include <string_view>
#include <memory>
#include <span>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <nghttp3/nghttp3.h>

#include <ev.h>

#include "client_base.h"
#include "tls_client_context_boringssl.h"
#include "tls_client_session_boringssl.h"
#include "network.h"
#include "shared.h"
#include "template.h"
#include "shared_chunk.h"

using namespace ngtcp2;

class Client;
class StreamIdentifier;

struct ClientStream {
    ClientStream(const Request &req, int64_t stream_id, Client *handler);
    ~ClientStream();

    void append_data(std::span<const uint8_t> data);

    bool lock_outgoing_chunks(vector<StreamIdentifier> const &sids, nghttp3_vec *vec, size_t veccnt);
    pair<size_t, vector<StreamIdentifier>> get_pending_chunks_size(int64_t stream_id, size_t veccnt);   

    void sendCloseSignal();

    Request req;
    int64_t stream_id;
    int fd;
    Client *handler;
    shared_span<> partial_chunk;
    chunks locked_chunks;
    bool trailers_sent;
};

struct Endpoint {
    Address addr;
    ev_io rev;
    Client *client;
    int fd;
};

class QuicConnector;

class Client : public ClientBase {
public:
  Client(struct ev_loop *loop, uint32_t client_chosen_version,
         uint32_t original_version, QuicConnector &quic_connector);
  ~Client();

  int init(int fd, const Address &local_addr, const Address &remote_addr,
           const char *addr, const char *port, TLSClientContext &tls_ctx);
  void disconnect();

  int on_read(const Endpoint &ep);
  int on_write();
  void start_check_stream_timer();
  void stop_check_stream_timer();
  void check_and_create_streams();
  int write_streams();
  int feed_data(const Endpoint &ep, const sockaddr *sa, socklen_t salen,
                const ngtcp2_pkt_info *pi, std::span<const uint8_t> data);
  int handle_expiry();
  void update_timer();
  int handshake_completed();
  int handshake_confirmed();
  void recv_version_negotiation(const uint32_t *sv, size_t nsv);

  int send_packet(const Endpoint &ep, const ngtcp2_addr &remote_addr,
                  unsigned int ecn, std::span<const uint8_t> data);
  std::pair<std::span<const uint8_t>, int>
  send_packet(const Endpoint &ep, const ngtcp2_addr &remote_addr,
              unsigned int ecn, std::span<const uint8_t> data, size_t gso_size);
  int on_stream_close(int64_t stream_id, uint64_t app_error_code);
  int on_extend_max_streams();
  int handle_error();
  int make_stream_early();
  int change_local_addr();
  void start_change_local_addr_timer();
  int update_key(uint8_t *rx_secret, uint8_t *tx_secret,
                 ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
                 ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
                 const uint8_t *current_rx_secret,
                 const uint8_t *current_tx_secret, size_t secretlen);
  int initiate_key_update();
  void start_key_update_timer();
  void start_delay_stream_timer();

  int select_preferred_address(Address &selected_addr,
                               const ngtcp2_preferred_addr *paddr);

  std::optional<Endpoint *> endpoint_for(const Address &remote_addr);

  void set_remote_addr(const ngtcp2_addr &remote_addr);

  int setup_httpconn();
  int submit_http_request(ClientStream *stream, bool live_stream);
  int recv_stream_data(uint32_t flags, int64_t stream_id,
                       std::span<const uint8_t> data);
  int acked_stream_data_offset(int64_t stream_id, uint64_t datalen);
  void http_consume(int64_t stream_id, size_t nconsumed);
  void http_write_data(int64_t stream_id, std::span<const uint8_t> data);
  int on_stream_reset(int64_t stream_id);
  int on_stream_stop_sending(int64_t stream_id);
  int extend_max_stream_data(int64_t stream_id, uint64_t max_data);
  int stop_sending(int64_t stream_id, uint64_t app_error_code);
  int reset_stream(int64_t stream_id, uint64_t app_error_code);
  int http_stream_close(int64_t stream_id, uint64_t app_error_code);

  void on_send_blocked(const Endpoint &ep, const ngtcp2_addr &remote_addr,
                       unsigned int ecn, std::span<const uint8_t> data,
                       size_t gso_size);
  void start_wev_endpoint(const Endpoint &ep);
  int send_blocked_packet();

  const std::vector<uint32_t> &get_offered_versions() const;

  bool get_early_data() const;
  void early_data_rejected();

  bool lock_outgoing_chunks(chunks &locked_chunks, vector<StreamIdentifier> const &sids, nghttp3_vec *vec, size_t veccnt);
  pair<size_t, vector<StreamIdentifier>> get_pending_chunks_size(int64_t stream_id, size_t veccnt);
  void unlock_chunks(nghttp3_vec *vec, size_t veccnt);
  void shared_span_incr_rc(uint8_t *locked_ptr, shared_span<> &&to_lock);
  void shared_span_decr_rc(uint8_t *locked_ptr);

  void push_incoming_chunk(shared_span<> &&chunk, Request const &req);

  void writecb_start();

  bool active_request(Request const& req) const {
    return std::any_of(streams_.begin(), streams_.end(),
                       [&req](const auto &pair) {
                         return pair.second->req == req;
                       });
  }

  ngtcp2_cid const &get_dcid() const;
  
  QuicConnector& getQuicConnector() { return quic_connector_; }

private:
  std::vector<Endpoint> endpoints_;
  Address remote_addr_;
  ev_io wev_;
  ev_timer timer_;
  ev_timer change_local_addr_timer_;
  ev_timer key_update_timer_;
  ev_timer delay_stream_timer_;
  ev_timer check_stream_timer_;
  ev_signal sigintev_;
  struct ev_loop *loop_;
  std::map<int64_t, std::unique_ptr<ClientStream>> streams_;
  std::vector<uint32_t> offered_versions_;
  nghttp3_conn *httpconn_;
  // addr_ is the server host address.
  const char *addr_;
  // port_ is the server port.
  const char *port_;
  // nstreams_done_ is the number of streams opened.
  size_t nstreams_done_;
  // nstreams_closed_ is the number of streams get closed.
  size_t nstreams_closed_;
  // nkey_update_ is the number of key update occurred.
  size_t nkey_update_;
  uint32_t client_chosen_version_;
  uint32_t original_version_;
  // early_data_ is true if client attempts to do 0RTT data transfer.
  bool early_data_;
  // handshake_confirmed_ gets true after handshake has been
  // confirmed.
  bool handshake_confirmed_;
  bool no_gso_;
  QuicConnector &quic_connector_;
  map<uint8_t *, shared_span<>> locked_chunks_;

  struct {
    bool send_blocked;
    size_t num_blocked;
    size_t num_blocked_sent;
    // blocked field is effective only when send_blocked is true.
    struct {
      const Endpoint *endpoint;
      Address remote_addr;
      unsigned int ecn;
      std::span<const uint8_t> data;
      size_t gso_size;
    } blocked[2];
    std::array<uint8_t, 64_k> data;
  } tx_;
};
