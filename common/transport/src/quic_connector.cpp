#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>
#include <iomanip>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/mman.h>
#include <libgen.h>
#include <netinet/udp.h>

#include <urlparse.h>

#include "client.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "shared.h"
#include "quic_connector.h"
#include "config_yaml.h"

using namespace ngtcp2;
using namespace std::literals;

namespace {
auto randgen = util::make_mt19937();
} // namespace

namespace {
constexpr size_t max_preferred_versionslen = 4;
} // namespace


ClientStream::ClientStream(const Request &req, int64_t stream_id, Client *client)
    : req(req),
        stream_id(stream_id),
        fd(-1),
        handler(client),
        partial_chunk(global_no_chunk_header, false),
        trailers_sent(false) {}

ClientStream::~ClientStream() {
  if (fd != -1) {
    close(fd);
  }
}


void ClientStream::append_data(std::span<const uint8_t> data) {
    if (!req.isWWATP()) {
        shared_span<> other_chunk(global_no_chunk_header, data);
        handler->push_incoming_chunk(move(other_chunk), req);
        return;
    }
    if (data.empty()) {
        return;
    }
    std::span<const uint8_t> remaining_span = data;
    // Note: if empty partial_chunk, stored_size() and get_wire_size() are both 0, 
    //       while get_signal_size() and get_wire_size() are also both 0, so it will test as not in progress.
    // Check if a prior chunk is already in progress, by examining the in memory size so far with the size read from the wire
    // The partial header condition is detected by checking if the wire size is less than the signal size, which is by definition in progress.
    // (But the stored_size and wire_size will be the same for incomplete headers)
    // For complete headers, wire size >= signal size, and stored_size == wire_size.
    if (partial_chunk.stored_size() != partial_chunk.get_wire_size() || partial_chunk.get_wire_size() < partial_chunk.get_signal_size())
    {
        // If so, append the as much data as necessary to the partial chunk
        size_t copy_bytes = min(partial_chunk.get_wire_size() - partial_chunk.stored_size(), data.size());
        if (partial_chunk.get_wire_size() < partial_chunk.get_signal_size()) {
            copy_bytes = partial_chunk.get_signal_size() - partial_chunk.stored_size();
        }
        auto next_start = partial_chunk.expand_use(copy_bytes);
        auto data_span = data.subspan(0, copy_bytes);
        partial_chunk.copy_span(data_span, {true, next_start});
        remaining_span = data.subspan(copy_bytes);
    } else {
        // Otherwise, create a new chunk and copy the data into it
        uint8_t signal_type = data[0];
        if (signal_type == signal_chunk_header::GLOBAL_SIGNAL_TYPE)
        {
            auto signal_span = data.subspan(0, min(sizeof(signal_chunk_header), data.size())); 
            partial_chunk = shared_span<>(signal_span);
            remaining_span = data.subspan(signal_span.size());
        } else if (signal_type == payload_chunk_header::GLOBAL_SIGNAL_TYPE)
        {
            if (data.size() < sizeof(payload_chunk_header)) {
                partial_chunk = shared_span<>(data);
                remaining_span = data.subspan(data.size());
            }
            else {
                auto header = reinterpret_cast<payload_chunk_header const*>(data.subspan(0, sizeof(payload_chunk_header)).data());
                auto data_span = data.subspan(0, min(header->get_wire_size(), data.size()));
                partial_chunk = shared_span<>(data_span);
                remaining_span = data.subspan(data_span.size());
            }
        } else {
            throw std::invalid_argument("Unhandled signal type " + std::to_string(signal_type) + " in Stream::append_data");
        }
    }
    // Check if the chunk is complete
    if (partial_chunk.stored_size() == partial_chunk.get_wire_size() && partial_chunk.get_wire_size() >= partial_chunk.get_signal_size()) {
        // If so, push it onto the incoming queue
        handler->push_incoming_chunk(move(partial_chunk), req);
        partial_chunk = shared_span<>(global_no_chunk_header, false);
    }
    append_data(remaining_span);
}

bool ClientStream::lock_outgoing_chunks(vector<StreamIdentifier> const &sids, nghttp3_vec *vec, size_t veccnt)
{
    // Assume that by the time lock_outgoing_chunks is called on this stream again, that it is safe to free
    // any previously locked chunks.
    //locked_chunks.clear();
    return handler->lock_outgoing_chunks(locked_chunks, sids, vec, veccnt);
}

pair<size_t, vector<StreamIdentifier>> ClientStream::get_pending_chunks_size(int64_t stream_id, size_t veccnt)
{
    return handler->get_pending_chunks_size(stream_id, veccnt);
}

void ClientStream::sendCloseSignal() {
    auto signal = signal_chunk_header(0, signal_chunk_header::SIGNAL_CLOSE_STREAM);
    shared_span<> chunk(signal, true);
    handler->push_incoming_chunk(move(chunk), req);
}


namespace {
void writecb(struct ev_loop * /* loop */, ev_io *w, int /* revents */) {
  auto c = static_cast<Client *>(w->data);

  c->on_write();
}
} // namespace

namespace {
void readcb(struct ev_loop * /* loop */, ev_io *w, int /* revents*/) {
  auto ep = static_cast<Endpoint *>(w->data);
  auto c = ep->client;

  if (c->on_read(*ep) != 0) {
    return;
  }

  c->on_write();
}
} // namespace

namespace {
void timeoutcb(struct ev_loop * /* loop */, ev_timer *w, int /* revents */) {
  int rv;
  auto c = static_cast<Client *>(w->data);

  rv = c->handle_expiry();
  if (rv != 0) {
    return;
  }

  c->on_write();
}
} // namespace

namespace {
void change_local_addrcb(struct ev_loop * /* loop */, ev_timer *w, int /*revents */) {
  auto c = static_cast<Client *>(w->data);

  c->change_local_addr();
}
} // namespace

namespace {
void key_updatecb(struct ev_loop * /* loop */, ev_timer *w, int /*revents */) {
  auto c = static_cast<Client *>(w->data);

  if (c->initiate_key_update() != 0) {
    c->disconnect();
  }
}
} // namespace

namespace {
void delay_streamcb(struct ev_loop * loop, ev_timer *w, int /*revents */) {
  auto c = static_cast<Client *>(w->data);

  ev_timer_stop(loop, w);
  c->on_extend_max_streams();
  c->on_write();
}
} // namespace

namespace {
void check_streamcb(struct ev_loop * /* loop */, ev_timer *w, int /*revents */) {
    auto c = static_cast<Client *>(w->data);

    c->check_and_create_streams();
}
} // namespace  

namespace {
void siginthandler(struct ev_loop *loop, ev_signal */* w */, int /* revents */) {
  ev_break(loop, EVBREAK_ALL);
}
} // namespace

Client::Client(struct ev_loop *loop, uint32_t client_chosen_version,
               uint32_t original_version, QuicConnector &quic_connector)
  : remote_addr_{},
    loop_(loop),
    httpconn_(nullptr),
    addr_(nullptr),
    port_(nullptr),
    nstreams_done_(0),
    nstreams_closed_(0),
    nkey_update_(0),
    client_chosen_version_(client_chosen_version),
    original_version_(original_version),
    early_data_(false),
    handshake_confirmed_(false),
    no_gso_{
#ifdef UDP_SEGMENT
      false
#else  // !defined(UDP_SEGMENT)
      true
#endif // !defined(UDP_SEGMENT)
    },
    quic_connector_(quic_connector),
    tx_{} {
  ev_io_init(&wev_, writecb, 0, EV_WRITE);
  wev_.data = this;
  ev_timer_init(&timer_, timeoutcb, 0., 0.);
  timer_.data = this;
  ev_timer_init(&change_local_addr_timer_, change_local_addrcb,
                static_cast<double>(quic_connector_.getConfig().change_local_addr) / NGTCP2_SECONDS,
                0.);
  change_local_addr_timer_.data = this;
  ev_timer_init(&key_update_timer_, key_updatecb,
                static_cast<double>(quic_connector_.getConfig().key_update) / NGTCP2_SECONDS, 0.);
  key_update_timer_.data = this;
  ev_timer_init(&delay_stream_timer_, delay_streamcb,
                static_cast<double>(quic_connector_.getConfig().delay_stream) / NGTCP2_SECONDS, 0.);
  delay_stream_timer_.data = this;
  ev_timer_init(&check_stream_timer_, check_streamcb, 0.1, 0.1); // Initialize the timer
  check_stream_timer_.data = this;
  ev_signal_init(&sigintev_, siginthandler, SIGINT);
}

Client::~Client() {
  disconnect();

  if (httpconn_) {
    nghttp3_conn_del(httpconn_);
    httpconn_ = nullptr;
  }
}

void Client::disconnect() {
  tx_.send_blocked = false;

  handle_error();

  // quic_connector_.getConfig().tx_loss_prob = 0; // Cannot modify const config

  ev_timer_stop(loop_, &delay_stream_timer_);
  ev_timer_stop(loop_, &key_update_timer_);
  ev_timer_stop(loop_, &change_local_addr_timer_);
  ev_timer_stop(loop_, &timer_);
  ev_timer_stop(loop_, &check_stream_timer_);

  ev_io_stop(loop_, &wev_);

  for (auto &ep : endpoints_) {
    ev_io_stop(loop_, &ep.rev);
    close(ep.fd);
  }

  endpoints_.clear();

  ev_signal_stop(loop_, &sigintev_);
}

namespace {
int recv_crypto_data(ngtcp2_conn *conn,
                     ngtcp2_encryption_level encryption_level, uint64_t offset,
                     const uint8_t *data, size_t datalen, void *user_data) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet && c->getQuicConnector().getConfig().quic_dump) {
        debug::print_crypto_data(encryption_level, {data, datalen});
    }

    return ngtcp2_crypto_recv_crypto_data_cb(conn, encryption_level, offset, data,
                                            datalen, user_data);
}
} // namespace

namespace {
int recv_stream_data(ngtcp2_conn *, uint32_t flags, int64_t stream_id,
                     uint64_t /* offset */, const uint8_t *data, size_t datalen,
                     void *user_data, void */*stream_user_data*/) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet && c->getQuicConnector().getConfig().quic_dump) {
        debug::print_stream_data(stream_id, {data, datalen});
    }

    if (c->recv_stream_data(flags, stream_id, {data, datalen}) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

namespace {
int acked_stream_data_offset(ngtcp2_conn *, int64_t stream_id,
                             uint64_t /* offset */, uint64_t datalen, void *user_data,
                             void * /*stream_user_data */) {
    auto c = static_cast<Client *>(user_data);
    if (c->acked_stream_data_offset(stream_id, datalen) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
} // namespace

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
    auto c = static_cast<Client *>(user_data);

    if (!c->getQuicConnector().getConfig().quiet) {
        debug::handshake_completed(conn, user_data);
    }

    if (c->handshake_completed() != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

int Client::handshake_completed() {
  if (early_data_ && !tls_session_.get_early_data_accepted()) {
    if (!quic_connector_.getConfig().quiet) {
      std::cerr << "Client Early data was rejected by server" << std::endl;
    }

    // Some TLS backends only report early data rejection after
    // handshake completion (e.g., OpenSSL).  For TLS backends which
    // report it early (e.g., BoringSSL and PicoTLS), the following
    // functions are noop.
    if (auto rv = ngtcp2_conn_tls_early_data_rejected(conn_); rv != 0) {
      std::cerr << "Client ngtcp2_conn_tls_early_data_rejected: "
                << ngtcp2_strerror(rv) << std::endl;
      return -1;
    }

    if (setup_httpconn() != 0) {
      return -1;
    }
  }

  if (!quic_connector_.getConfig().quiet) {
    std::cerr << "Client Negotiated cipher suite is " << tls_session_.get_cipher_name()
              << std::endl;
    if (auto group = tls_session_.get_negotiated_group(); !group.empty()) {
      std::cerr << "Client Negotiated group is " << group << std::endl;
    }
    std::cerr << "Client Negotiated ALPN is " << tls_session_.get_selected_alpn()
              << std::endl;
  }

  if (quic_connector_.getConfig().tp_file.c_str()) {
    std::array<uint8_t, 256> data;
    auto datalen =
      ngtcp2_conn_encode_0rtt_transport_params(conn_, data.data(), data.size());
    if (datalen < 0) {
      std::cerr << "Client Could not encode 0-RTT transport parameters: "
                << ngtcp2_strerror(datalen) << std::endl;
    } else if (util::write_transport_params(
                 quic_connector_.getConfig().tp_file.c_str(), {data.data(), static_cast<size_t>(datalen)}) !=
               0) {
      std::cerr << "Client Could not write transport parameters in " << quic_connector_.getConfig().tp_file
                << std::endl;
    }
  }

  return 0;
}

namespace {
int handshake_confirmed(ngtcp2_conn *conn, void *user_data) {
    auto c = static_cast<Client *>(user_data);

    if (!c->getQuicConnector().getConfig().quiet) {
        debug::handshake_confirmed(conn, user_data);
    }

    if (c->handshake_confirmed() != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

int Client::handshake_confirmed() {
  handshake_confirmed_ = true;

  if (quic_connector_.getConfig().change_local_addr) {
    start_change_local_addr_timer();
  }
  if (quic_connector_.getConfig().key_update) {
    start_key_update_timer();
  }
  if (quic_connector_.getConfig().delay_stream) {
    start_delay_stream_timer();
  }
  start_check_stream_timer();

  return 0;
}

namespace {
int recv_version_negotiation(ngtcp2_conn *, const ngtcp2_pkt_hd * /* hd */,
                             const uint32_t *sv, size_t nsv, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  c->recv_version_negotiation(sv, nsv);

  return 0;
}
} // namespace

void Client::recv_version_negotiation(const uint32_t *sv, size_t nsv) {
  offered_versions_.resize(nsv);
  std::copy_n(sv, nsv, std::begin(offered_versions_));
}

namespace {
int stream_close(ngtcp2_conn *, uint32_t flags, int64_t stream_id,
                 uint64_t app_error_code, void *user_data,
                 void * /* stream_user_data */) {
  auto c = static_cast<Client *>(user_data);

  if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) {
    app_error_code = NGHTTP3_H3_NO_ERROR;
  }

  if (c->on_stream_close(stream_id, app_error_code) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int stream_reset(ngtcp2_conn *, int64_t stream_id, uint64_t /* final_size */,
                 uint64_t /* app_error_code */, void *user_data,
                 void */* stream_user_data */) {
  auto c = static_cast<Client *>(user_data);

  if (c->on_stream_reset(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int stream_stop_sending(ngtcp2_conn *, int64_t stream_id,
                        uint64_t /* app_error_code */, void *user_data,
                        void */*stream_user_data*/) {
  auto c = static_cast<Client *>(user_data);

  if (c->on_stream_stop_sending(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int extend_max_local_streams_bidi(ngtcp2_conn *, uint64_t /* max_streams */,
                                  void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->on_extend_max_streams() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
void rand(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx */* rand_ctx */) {
  auto dis = std::uniform_int_distribution<uint8_t>();
  std::generate(dest, dest + destlen, [&dis]() { return dis(randgen); });
}
} // namespace

namespace {
int get_new_connection_id(ngtcp2_conn *, ngtcp2_cid *cid, uint8_t *token,
                          size_t cidlen, void *user_data) {
    if (util::generate_secure_random({cid->data, cidlen}) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    cid->datalen = cidlen;
    auto c = static_cast<Client *>(user_data);
    if (ngtcp2_crypto_generate_stateless_reset_token(
            token, c->getQuicConnector().getConfig().static_secret.data(), c->getQuicConnector().getConfig().static_secret.size(), cid) !=
        0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

namespace {
int do_hp_mask(uint8_t *dest, const ngtcp2_crypto_cipher *hp,
               const ngtcp2_crypto_cipher_ctx *hp_ctx, const uint8_t *sample) {
    if (ngtcp2_crypto_hp_mask(dest, hp, hp_ctx, sample) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    // debug::print_hp_mask({dest, NGTCP2_HP_MASKLEN},
    //                      {sample, NGTCP2_HP_SAMPLELEN});

    return 0;
}
} // namespace

namespace {
int update_key(ngtcp2_conn *, uint8_t *rx_secret, uint8_t *tx_secret,
               ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
               ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
               const uint8_t *current_rx_secret,
               const uint8_t *current_tx_secret, size_t secretlen,
               void *user_data) {
    auto c = static_cast<Client *>(user_data);

    if (c->update_key(rx_secret, tx_secret, rx_aead_ctx, rx_iv, tx_aead_ctx,
                        tx_iv, current_rx_secret, current_tx_secret,
                        secretlen) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}
} // namespace

namespace {
int path_validation(ngtcp2_conn *, uint32_t flags, const ngtcp2_path *path,
                    const ngtcp2_path */*old_path*/,
                    ngtcp2_path_validation_result res, void *user_data) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::path_validation(path, res);
    }

    if (flags & NGTCP2_PATH_VALIDATION_FLAG_PREFERRED_ADDR) {

        c->set_remote_addr(path->remote);
    }

    return 0;
}
} // namespace

void Client::set_remote_addr(const ngtcp2_addr &remote_addr) {
  memcpy(&remote_addr_.su, remote_addr.addr, remote_addr.addrlen);
  remote_addr_.len = remote_addr.addrlen;
}

namespace {
int select_preferred_address(ngtcp2_conn *, ngtcp2_path *dest,
                             const ngtcp2_preferred_addr *paddr,
                             void *user_data) {
    auto c = static_cast<Client *>(user_data);
    Address remote_addr;

    if (c->getQuicConnector().getConfig().no_preferred_addr) {
        return 0;
    }

    if (c->select_preferred_address(remote_addr, paddr) != 0) {
        return 0;
    }

    auto ep = c->endpoint_for(remote_addr);
    if (!ep) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    ngtcp2_addr_copy_byte(&dest->local, &(*ep)->addr.su.sa, (*ep)->addr.len);
    ngtcp2_addr_copy_byte(&dest->remote, &remote_addr.su.sa, remote_addr.len);
    dest->user_data = *ep;

    return 0;
}
} // namespace

namespace {
int extend_max_stream_data(ngtcp2_conn *, int64_t stream_id,
                           uint64_t max_data, void *user_data,
                           void * /*stream_user_data*/) {
  auto c = static_cast<Client *>(user_data);
  if (c->extend_max_stream_data(stream_id, max_data) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::extend_max_stream_data(int64_t stream_id, uint64_t /* max_data */) {
  if (auto rv = nghttp3_conn_unblock_stream(httpconn_, stream_id); rv != 0) {
    std::cerr << "Client nghttp3_conn_unblock_stream: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }
  return 0;
}

namespace {
int recv_new_token(ngtcp2_conn *, const uint8_t *token, size_t tokenlen,
                   void *user_data) {
    auto c = static_cast<Client *>(user_data);
    if (c->getQuicConnector().getConfig().token_file.empty()) {
        return 0;
    }

    util::write_token(c->getQuicConnector().getConfig().token_file, {token, tokenlen});

    return 0;
}
} // namespace

namespace {
int recv_rx_key(ngtcp2_conn *, ngtcp2_encryption_level level,
                void *user_data) {
  if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT) {
    return 0;
  }

  auto c = static_cast<Client *>(user_data);
  if (c->setup_httpconn() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int early_data_rejected(ngtcp2_conn *, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  c->early_data_rejected();

  return 0;
}
} // namespace

void Client::early_data_rejected() {
  nghttp3_conn_del(httpconn_);
  httpconn_ = nullptr;

  nstreams_done_ = 0;
  streams_.clear();
}

int Client::init(int fd, const Address &local_addr, const Address &remote_addr,
                 const char *addr, const char *port,
                 TLSClientContext &tls_ctx) {
  endpoints_.reserve(4);

  endpoints_.emplace_back();
  auto &ep = endpoints_.back();
  ep.addr = local_addr;
  ep.client = this;
  ep.fd = fd;
  ev_io_init(&ep.rev, readcb, fd, EV_READ);
  ep.rev.data = &ep;

  remote_addr_ = remote_addr;
  addr_ = addr;
  port_ = port;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

auto callbacks = ngtcp2_callbacks{
    .client_initial = ngtcp2_crypto_client_initial_cb,
    .recv_crypto_data = ::recv_crypto_data,
    .handshake_completed = ::handshake_completed,
    .recv_version_negotiation = ::recv_version_negotiation,
    .encrypt = ngtcp2_crypto_encrypt_cb,
    .decrypt = ngtcp2_crypto_decrypt_cb,
    .hp_mask = do_hp_mask,
    .recv_stream_data = ::recv_stream_data,
    .acked_stream_data_offset = ::acked_stream_data_offset,
    .stream_close = stream_close,
    .recv_retry = ngtcp2_crypto_recv_retry_cb,
    .extend_max_local_streams_bidi = extend_max_local_streams_bidi,
    .rand = rand,
    .get_new_connection_id = get_new_connection_id,
    .update_key = ::update_key,
    .path_validation = path_validation,
    .select_preferred_addr = ::select_preferred_address,
    .stream_reset = stream_reset,
    .extend_max_stream_data = ::extend_max_stream_data,
    .handshake_confirmed = ::handshake_confirmed,
    .recv_new_token = ::recv_new_token,
    .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
    .stream_stop_sending = stream_stop_sending,
    .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
    .recv_rx_key = ::recv_rx_key,
    .tls_early_data_rejected = ::early_data_rejected,
};

#pragma GCC diagnostic pop

  ngtcp2_cid scid, dcid;
    scid.datalen = 17;
    if (util::generate_secure_random({scid.data, scid.datalen}) != 0) {
        std::cerr << "Client Could not generate source connection ID" << std::endl;
        return -1;
    }
    dcid.datalen = 18;
    if (util::generate_secure_random({dcid.data, dcid.datalen}) != 0) {
      std::cerr << "Client Could not generate destination connection ID" << std::endl;
      return -1;
    }

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.log_printf = quic_connector_.getConfig().quiet ? nullptr : debug::log_printf;
  if (!quic_connector_.getConfig().qlog_file.empty() || !quic_connector_.getConfig().qlog_dir.empty()) {
    std::string path;
    if (!quic_connector_.getConfig().qlog_file.empty()) {
      path = quic_connector_.getConfig().qlog_file;
    } else {
      path = std::string{quic_connector_.getConfig().qlog_dir};
      path += '/';
      path += util::format_hex({scid.data, scid.datalen});
      path += ".sqlog";
    }
    qlog_ = fopen(path.c_str(), "w");
    if (qlog_ == nullptr) {
      std::cerr << "Client Could not open qlog file " << std::quoted(path) << ": "
                << strerror(errno) << std::endl;
      return -1;
    }
    settings.qlog_write = qlog_write_cb;
  }

  settings.cc_algo = quic_connector_.getConfig().cc_algo;
  settings.initial_ts = util::timestamp();
  settings.initial_rtt = quic_connector_.getConfig().initial_rtt;
  settings.max_window = quic_connector_.getConfig().max_window;
  settings.max_stream_window = quic_connector_.getConfig().max_stream_window;
  if (quic_connector_.getConfig().max_udp_payload_size) {
    settings.max_tx_udp_payload_size = quic_connector_.getConfig().max_udp_payload_size;
    settings.no_tx_udp_payload_size_shaping = 1;
  }
  settings.handshake_timeout = quic_connector_.getConfig().handshake_timeout;
  settings.no_pmtud = quic_connector_.getConfig().no_pmtud;
  settings.ack_thresh = quic_connector_.getConfig().ack_thresh;
  if (quic_connector_.getConfig().initial_pkt_num == UINT32_MAX) {
    auto dis = std::uniform_int_distribution<uint32_t>(0, INT32_MAX);
    settings.initial_pkt_num = dis(randgen);
  } else {
    settings.initial_pkt_num = quic_connector_.getConfig().initial_pkt_num;
  }

  std::string token;

  if (!quic_connector_.getConfig().token_file.empty()) {
    std::cerr << "Client Reading token file " << quic_connector_.getConfig().token_file << std::endl;

    auto t = util::read_token(quic_connector_.getConfig().token_file);
    if (t) {
      token = std::move(*t);
      settings.token = reinterpret_cast<const uint8_t *>(token.data());
      settings.tokenlen = token.size();
    }
  }

  if (!quic_connector_.getConfig().available_versions.empty()) {
    settings.available_versions = quic_connector_.getConfig().available_versions.data();
    settings.available_versionslen = quic_connector_.getConfig().available_versions.size();
  }

  if (!quic_connector_.getConfig().preferred_versions.empty()) {
    settings.preferred_versions = quic_connector_.getConfig().preferred_versions.data();
    settings.preferred_versionslen = quic_connector_.getConfig().preferred_versions.size();
  }

  settings.original_version = original_version_;

  if (!quic_connector_.getConfig().pmtud_probes.empty()) {
    settings.pmtud_probes = quic_connector_.getConfig().pmtud_probes.data();
    settings.pmtud_probeslen = quic_connector_.getConfig().pmtud_probes.size();

    if (!quic_connector_.getConfig().max_udp_payload_size) {
      settings.max_tx_udp_payload_size = *std::max_element(
        std::begin(quic_connector_.getConfig().pmtud_probes), std::end(quic_connector_.getConfig().pmtud_probes));
    }
  }

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);
  params.initial_max_stream_data_bidi_local = quic_connector_.getConfig().max_stream_data_bidi_local;
  params.initial_max_stream_data_bidi_remote =
    quic_connector_.getConfig().max_stream_data_bidi_remote;
  params.initial_max_stream_data_uni = quic_connector_.getConfig().max_stream_data_uni;
  params.initial_max_data = quic_connector_.getConfig().max_data;
  params.initial_max_streams_bidi = quic_connector_.getConfig().max_streams_bidi;
  params.initial_max_streams_uni = quic_connector_.getConfig().max_streams_uni;
  params.max_idle_timeout = quic_connector_.getConfig().timeout;
  params.active_connection_id_limit = 7;
  params.grease_quic_bit = 1;

  auto path = ngtcp2_path{
    .local =
      {
        .addr = const_cast<sockaddr *>(&ep.addr.su.sa),
        .addrlen = ep.addr.len,
      },
    .remote =
      {
        .addr = const_cast<sockaddr *>(&remote_addr.su.sa),
        .addrlen = remote_addr.len,
      },
    .user_data = &ep,
  };
  auto rv =
    ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path, client_chosen_version_,
                           &callbacks, &settings, &params, nullptr, this);

  if (rv != 0) {
    std::cerr << "Client ngtcp2_conn_client_new: " << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  if (tls_session_.init(early_data_, tls_ctx, addr_, this,
                        client_chosen_version_, AppProtocol::H3) != 0) {
    return -1;
  }

  ngtcp2_conn_set_tls_native_handle(conn_, tls_session_.get_native_handle());

  if (early_data_ && !quic_connector_.getConfig().tp_file.empty()) {
    auto params = util::read_transport_params(quic_connector_.getConfig().tp_file.c_str());
    if (!params) {
      early_data_ = false;
    } else {
      auto rv = ngtcp2_conn_decode_and_set_0rtt_transport_params(
        conn_, reinterpret_cast<const uint8_t *>(params->data()),
        params->size());
      if (rv != 0) {
        std::cerr << "Client ngtcp2_conn_decode_and_set_0rtt_transport_params: "
                  << ngtcp2_strerror(rv) << std::endl;
        early_data_ = false;
      } else if (make_stream_early() != 0) {
        return -1;
      }
    }
  }

  ev_io_start(loop_, &ep.rev);

  ev_signal_start(loop_, &sigintev_);

  return 0;
}

int Client::feed_data(const Endpoint &ep, const sockaddr *sa, socklen_t salen,
                      const ngtcp2_pkt_info *pi,
                      std::span<const uint8_t> data) {
  auto path = ngtcp2_path{
    .local =
      {
        .addr = const_cast<sockaddr *>(&ep.addr.su.sa),
        .addrlen = ep.addr.len,
      },
    .remote =
      {
        .addr = const_cast<sockaddr *>(sa),
        .addrlen = salen,
      },
    .user_data = const_cast<Endpoint *>(&ep),
  };
  if (auto rv = ngtcp2_conn_read_pkt(conn_, &path, pi, data.data(), data.size(),
                                     util::timestamp());
      rv != 0) {
    std::cerr << "Client ngtcp2_conn_read_pkt: " << ngtcp2_strerror(rv) << std::endl;
    if (!last_error_.error_code) {
      if (rv == NGTCP2_ERR_CRYPTO) {
        ngtcp2_ccerr_set_tls_alert(
          &last_error_, ngtcp2_conn_get_tls_alert(conn_), nullptr, 0);
      } else {
        ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      }
    }
    disconnect();
    return -1;
  }
  return 0;
}

int Client::on_read(const Endpoint &ep) {
  std::array<uint8_t, 64_k> buf;
  sockaddr_union su;
  size_t pktcnt = 0;
  ngtcp2_pkt_info pi;

  iovec msg_iov{
    .iov_base = buf.data(),
    .iov_len = buf.size(),
  };

  uint8_t msg_ctrl[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(int))];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

  msghdr msg{
    .msg_name = &su,
    .msg_iov = &msg_iov,
    .msg_iovlen = 1,
    .msg_control = msg_ctrl,
  };
#pragma GCC diagnostic pop

  for (;;) {
    msg.msg_namelen = sizeof(su);
    msg.msg_controllen = sizeof(msg_ctrl);

    auto nread = recvmsg(ep.fd, &msg, 0);

    if (nread == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "Client recvmsg: " << strerror(errno) << std::endl;
      }
      break;
    }

    // Packets less than 21 bytes never be a valid QUIC packet.
    if (nread < 21) {
      ++pktcnt;

      continue;
    }

    pi.ecn = msghdr_get_ecn(&msg, su.storage.ss_family);
    auto gso_size = msghdr_get_udp_gro(&msg);
    if (gso_size == 0) {
      gso_size = static_cast<size_t>(nread);
    }

    auto data = std::span{buf.data(), static_cast<size_t>(nread)};

    for (;;) {
      auto datalen = std::min(data.size(), gso_size);

      ++pktcnt;

      if (!quic_connector_.getConfig().quiet) {
        std::cerr << "Client Received packet: local="
                  << util::straddr(&ep.addr.su.sa, ep.addr.len)
                  << " remote=" << util::straddr(&su.sa, msg.msg_namelen)
                  << " ecn=0x" << std::hex << static_cast<uint32_t>(pi.ecn)
                  << std::dec << " " << datalen << " bytes" << std::endl;
      }

      // Packets less than 21 bytes never be a valid QUIC packet.
      if (datalen < 21) {
        break;
      }

      if (debug::packet_lost(quic_connector_.getConfig().rx_loss_prob)) {
        if (!quic_connector_.getConfig().quiet) {
          std::cerr << "Client ** Simulated incoming packet loss **" << std::endl;
        }
      } else if (feed_data(ep, &su.sa, msg.msg_namelen, &pi,
                           {data.data(), datalen}) != 0) {
        return -1;
      }

      data = data.subspan(datalen);

      if (data.empty()) {
        break;
      }
    }

    if (pktcnt >= 10) {
      break;
    }
  }

  update_timer();

  return 0;
}

int Client::handle_expiry() {
  auto now = util::timestamp();
  if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0) {
    std::cerr << "Client ngtcp2_conn_handle_expiry: " << ngtcp2_strerror(rv)
              << std::endl;
    ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
    disconnect();
    return -1;
  }

  return 0;
}

void Client::start_check_stream_timer() {
    ev_timer_init(&check_stream_timer_, check_streamcb, 1.0, 1.0); // Check every 1 second
    check_stream_timer_.data = this;
    ev_timer_start(loop_, &check_stream_timer_);
}
  
void Client::stop_check_stream_timer() {
    ev_timer_stop(loop_, &check_stream_timer_);
}
  
void Client::check_and_create_streams() {
    // Check if there are unused stream IDs available
    if (ngtcp2_conn_get_streams_bidi_left(conn_) > 0) {
        on_extend_max_streams();
    }
}

int Client::on_write() {
  if (tx_.send_blocked) {
    if (auto rv = send_blocked_packet(); rv != 0) {
      return rv;
    }

    if (tx_.send_blocked) {
      return 0;
    }
  }

  ev_io_stop(loop_, &wev_);

  if (auto rv = write_streams(); rv != 0) {
    return rv;
  }

  update_timer();
  return 0;
}

int Client::write_streams() {
  std::array<nghttp3_vec, 16> vec;
  ngtcp2_path_storage ps, prev_ps;
  uint32_t prev_ecn = 0;
  auto max_udp_payload_size = ngtcp2_conn_get_max_tx_udp_payload_size(conn_);
  auto path_max_udp_payload_size =
    ngtcp2_conn_get_path_max_tx_udp_payload_size(conn_);
  ngtcp2_pkt_info pi;
  size_t gso_size = 0;
  auto ts = util::timestamp();
  auto txbuf =
    std::span{tx_.data.data(), std::max(ngtcp2_conn_get_send_quantum(conn_),
                                        path_max_udp_payload_size)};
  auto buf = txbuf;

  ngtcp2_path_storage_zero(&ps);
  ngtcp2_path_storage_zero(&prev_ps);

  for (;;) {
    int64_t stream_id = -1;
    int fin = 0;
    nghttp3_ssize sveccnt = 0;

    if (httpconn_ && ngtcp2_conn_get_max_data_left(conn_)) {
      sveccnt = nghttp3_conn_writev_stream(httpconn_, &stream_id, &fin,
                                           vec.data(), vec.size());
      if (sveccnt < 0) {
        std::cerr << "Client nghttp3_conn_writev_stream: " << nghttp3_strerror(sveccnt)
                  << std::endl;
        ngtcp2_ccerr_set_application_error(
          &last_error_, nghttp3_err_infer_quic_app_error_code(sveccnt), nullptr,
          0);
        disconnect();
        return -1;
      }
    }

    ngtcp2_ssize ndatalen;
    auto v = vec.data();
    auto vcnt = static_cast<size_t>(sveccnt);

    uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
    if (fin) {
      flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    auto buflen = buf.size() >= max_udp_payload_size
                    ? max_udp_payload_size
                    : path_max_udp_payload_size;

    auto nwrite = ngtcp2_conn_writev_stream(
      conn_, &ps.path, &pi, buf.data(), buflen, &ndatalen, flags, stream_id,
      reinterpret_cast<const ngtcp2_vec *>(v), vcnt, ts);
    if (nwrite < 0) {
      switch (nwrite) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
        assert(ndatalen == -1);
        nghttp3_conn_block_stream(httpconn_, stream_id);
        continue;
      case NGTCP2_ERR_STREAM_SHUT_WR:
        assert(ndatalen == -1);
        nghttp3_conn_shutdown_stream_write(httpconn_, stream_id);
        continue;
      case NGTCP2_ERR_WRITE_MORE:
        assert(ndatalen >= 0);
        if (auto rv =
              nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
            rv != 0) {
          std::cerr << "Client nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                    << std::endl;
          ngtcp2_ccerr_set_application_error(
            &last_error_, nghttp3_err_infer_quic_app_error_code(rv), nullptr,
            0);
          disconnect();
          return -1;
        }
        continue;
      }

      assert(ndatalen == -1);

      std::cerr << "Client ngtcp2_conn_write_stream: " << ngtcp2_strerror(nwrite)
                << std::endl;
      ngtcp2_ccerr_set_liberr(&last_error_, nwrite, nullptr, 0);
      disconnect();
      return -1;
    } else if (ndatalen >= 0) {
      if (auto rv =
            nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
          rv != 0) {
        std::cerr << "Client nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                  << std::endl;
        ngtcp2_ccerr_set_application_error(
          &last_error_, nghttp3_err_infer_quic_app_error_code(rv), nullptr, 0);
        disconnect();
        return -1;
      }
    }

    if (nwrite == 0) {
      auto data = std::span{std::begin(txbuf), std::begin(buf)};
      if (!data.empty()) {
        auto &ep = *static_cast<Endpoint *>(prev_ps.path.user_data);

        if (auto [rest, rv] =
              send_packet(ep, prev_ps.path.remote, prev_ecn, data, gso_size);
            rv != NETWORK_ERR_OK) {
          assert(NETWORK_ERR_SEND_BLOCKED == rv);

          on_send_blocked(ep, prev_ps.path.remote, prev_ecn, rest, gso_size);
        }
      }

      // We are congestion limited.
      ngtcp2_conn_update_pkt_tx_time(conn_, ts);
      return 0;
    }

    auto last_pkt = buf.first(nwrite);

    buf = buf.subspan(nwrite);

    if (std::begin(last_pkt) == std::begin(txbuf)) {
      ngtcp2_path_copy(&prev_ps.path, &ps.path);
      prev_ecn = pi.ecn;
      gso_size = nwrite;
    } else if (!ngtcp2_path_eq(&prev_ps.path, &ps.path) || prev_ecn != pi.ecn ||
               static_cast<size_t>(nwrite) > gso_size ||
               (gso_size > path_max_udp_payload_size &&
                static_cast<size_t>(nwrite) != gso_size)) {
      auto &ep = *static_cast<Endpoint *>(prev_ps.path.user_data);
      auto data = std::span{std::begin(txbuf), std::begin(last_pkt)};

      if (auto [rest, rv] =
            send_packet(ep, prev_ps.path.remote, prev_ecn, data, gso_size);
          rv != 0) {
        assert(NETWORK_ERR_SEND_BLOCKED == rv);

        on_send_blocked(ep, prev_ps.path.remote, prev_ecn, rest, gso_size);
        on_send_blocked(*static_cast<Endpoint *>(ps.path.user_data),
                        ps.path.remote, pi.ecn, last_pkt, last_pkt.size());
      } else {
        auto &ep = *static_cast<Endpoint *>(ps.path.user_data);

        if (auto [rest, rv] = send_packet(ep, ps.path.remote, pi.ecn, last_pkt,
                                          last_pkt.size());
            rv != 0) {
          assert(rest.size() == last_pkt.size());
          assert(NETWORK_ERR_SEND_BLOCKED == rv);

          on_send_blocked(ep, ps.path.remote, pi.ecn, rest, rest.size());
        }
      }

      ngtcp2_conn_update_pkt_tx_time(conn_, ts);

      return 0;
    }

    if (buf.size() < path_max_udp_payload_size ||
        static_cast<size_t>(nwrite) < gso_size) {
      auto &ep = *static_cast<Endpoint *>(ps.path.user_data);
      auto data = std::span{std::begin(txbuf), std::begin(buf)};

      if (auto [rest, rv] =
            send_packet(ep, ps.path.remote, pi.ecn, data, gso_size);
          rv != 0) {
        assert(NETWORK_ERR_SEND_BLOCKED == rv);

        on_send_blocked(ep, ps.path.remote, pi.ecn, rest, gso_size);
      }

      ngtcp2_conn_update_pkt_tx_time(conn_, ts);

      return 0;
    }
    
  }
}

void Client::update_timer() {
  auto expiry = ngtcp2_conn_get_expiry(conn_);
  auto now = util::timestamp();

  if (expiry <= now) {
    if (!quic_connector_.getConfig().quiet) {
      auto t = static_cast<ev_tstamp>(now - expiry) / NGTCP2_SECONDS;
      std::cerr << "Client Timer has already expired: " << std::fixed << t << "s"
                << std::defaultfloat << std::endl;
    }

    ev_feed_event(loop_, &timer_, EV_TIMER);

    return;
  }

  auto t = static_cast<ev_tstamp>(expiry - now) / NGTCP2_SECONDS;
  if (!quic_connector_.getConfig().quiet) {
    std::cerr << "Client Set timer=" << std::fixed << t << "s" << std::defaultfloat
              << std::endl;
  }
  timer_.repeat = t;
  ev_timer_again(loop_, &timer_);
}

#ifdef HAVE_LINUX_RTNETLINK_H
namespace {
int bind_addr(Address &local_addr, int fd, const in_addr_union *iau,
              int family) {
  addrinfo hints{
    .ai_flags = AI_PASSIVE,
    .ai_family = family,
    .ai_socktype = SOCK_DGRAM,
  };
  addrinfo *res, *rp;
  char *node;
  std::array<char, NI_MAXHOST> nodebuf;

  if (iau) {
    if (inet_ntop(family, iau, nodebuf.data(), nodebuf.size()) == nullptr) {
      std::cerr << "Client inet_ntop: " << strerror(errno) << std::endl;
      return -1;
    }

    node = nodebuf.data();
  } else {
    node = nullptr;
  }

  if (auto rv = getaddrinfo(node, "0", &hints, &res); rv != 0) {
    std::cerr << "Client getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  for (rp = res; rp; rp = rp->ai_next) {
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) != -1) {
      break;
    }
  }

  if (!rp) {
    std::cerr << "Client Could not bind" << std::endl;
    return -1;
  }

  socklen_t len = sizeof(local_addr.su.storage);
  if (getsockname(fd, &local_addr.su.sa, &len) == -1) {
    std::cerr << "Client getsockname: " << strerror(errno) << std::endl;
    return -1;
  }
  local_addr.len = len;
  local_addr.ifindex = 0;

  return 0;
}
} // namespace
#endif // defined(HAVE_LINUX_RTNETLINK_H)

#ifndef HAVE_LINUX_RTNETLINK_H
namespace {
int connect_sock(Address &local_addr, int fd, const Address &remote_addr) {
  if (connect(fd, &remote_addr.su.sa, remote_addr.len) != 0) {
    std::cerr << "Client connect: " << strerror(errno) << std::endl;
    return -1;
  }

  socklen_t len = sizeof(local_addr.su.storage);
  if (getsockname(fd, &local_addr.su.sa, &len) == -1) {
    std::cerr << "Client getsockname: " << strerror(errno) << std::endl;
    return -1;
  }
  local_addr.len = len;
  local_addr.ifindex = 0;

  return 0;
}
} // namespace
#endif // !defined(HAVE_LINUX_RTNETLINK_H)

namespace {
int udp_sock(int family) {
  auto fd = util::create_nonblock_socket(family, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == -1) {
    return -1;
  }

  fd_set_recv_ecn(fd, family);
  fd_set_ip_mtu_discover(fd, family);
  fd_set_ip_dontfrag(fd, family);
  fd_set_udp_gro(fd);

  return fd;
}
} // namespace

namespace {
int create_sock(Address &remote_addr, const char *addr, const char *port) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

  addrinfo hints{
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_DGRAM,
  };
#pragma GCC diagnostic pop
  addrinfo *res, *rp;

  if (auto rv = getaddrinfo(addr, port, &hints, &res); rv != 0) {
    std::cerr << "Client getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  int fd = -1;

  for (rp = res; rp; rp = rp->ai_next) {
    fd = create_flagged_nonblock_socket(rp->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == -1) {
      continue;
    }

    break;
  }

  if (!rp) {
    std::cerr << "Client Could not create socket" << std::endl;
    return -1;
  }

  remote_addr.len = rp->ai_addrlen;
  memcpy(&remote_addr.su, rp->ai_addr, rp->ai_addrlen);
  remote_addr.ifindex = 0;

  return fd;
}
} // namespace

std::optional<Endpoint *> Client::endpoint_for(const Address &remote_addr) {
#ifdef HAVE_LINUX_RTNETLINK_H
  in_addr_union iau;

  if (get_local_addr(iau, remote_addr) != 0) {
    std::cerr << "Client Could not get local address for a selected preferred address"
              << std::endl;
    return nullptr;
  }

  auto current_path = ngtcp2_conn_get_path(conn_);
  auto current_ep = static_cast<Endpoint *>(current_path->user_data);
  if (addreq(&current_ep->addr.su.sa, iau)) {
    return current_ep;
  }
#endif // defined(HAVE_LINUX_RTNETLINK_H)

  auto fd = udp_sock(remote_addr.su.sa.sa_family);
  if (fd == -1) {
    return nullptr;
  }

  Address local_addr;

#ifdef HAVE_LINUX_RTNETLINK_H
  if (bind_addr(local_addr, fd, &iau, remote_addr.su.sa.sa_family) != 0) {
    close(fd);
    return nullptr;
  }
#else  // !defined(HAVE_LINUX_RTNETLINK_H)
  if (connect_sock(local_addr, fd, remote_addr) != 0) {
    close(fd);
    return nullptr;
  }
#endif // !defined(HAVE_LINUX_RTNETLINK_H)

  endpoints_.emplace_back();
  auto &ep = endpoints_.back();
  ep.addr = local_addr;
  ep.client = this;
  ep.fd = fd;
  ev_io_init(&ep.rev, readcb, fd, EV_READ);
  ep.rev.data = &ep;

  ev_io_start(loop_, &ep.rev);

  return &ep;
}

void Client::start_change_local_addr_timer() {
  ev_timer_start(loop_, &change_local_addr_timer_);
}

int Client::change_local_addr() {
  Address local_addr;

  if (!quic_connector_.getConfig().quiet) {
    std::cerr << "Client Changing local address" << std::endl;
  }

  auto nfd = udp_sock(remote_addr_.su.sa.sa_family);
  if (nfd == -1) {
    return -1;
  }

#ifdef HAVE_LINUX_RTNETLINK_H
  in_addr_union iau;

  if (get_local_addr(iau, remote_addr_) != 0) {
    std::cerr << "Client Could not get local address" << std::endl;
    close(nfd);
    return -1;
  }

  if (bind_addr(local_addr, nfd, &iau, remote_addr_.su.sa.sa_family) != 0) {
    close(nfd);
    return -1;
  }
#else  // !defined(HAVE_LINUX_RTNETLINK_H)
  if (connect_sock(local_addr, nfd, remote_addr_) != 0) {
    close(nfd);
    return -1;
  }
#endif // !defined(HAVE_LINUX_RTNETLINK_H)

  if (!quic_connector_.getConfig().quiet) {
    std::cerr << "Client Local address is now "
              << util::straddr(&local_addr.su.sa, local_addr.len) << std::endl;
  }

  endpoints_.emplace_back();
  auto &ep = endpoints_.back();
  ep.addr = local_addr;
  ep.client = this;
  ep.fd = nfd;
  ev_io_init(&ep.rev, readcb, nfd, EV_READ);
  ep.rev.data = &ep;

  ngtcp2_addr addr;
  ngtcp2_addr_init(&addr, &local_addr.su.sa, local_addr.len);

  if (quic_connector_.getConfig().nat_rebinding) {
    ngtcp2_conn_set_local_addr(conn_, &addr);
    ngtcp2_conn_set_path_user_data(conn_, &ep);
  } else {
    auto path = ngtcp2_path{
      .local = addr,
      .remote =
        {
          .addr = const_cast<sockaddr *>(&remote_addr_.su.sa),
          .addrlen = remote_addr_.len,
        },
      .user_data = &ep,
    };
    if (auto rv = ngtcp2_conn_initiate_immediate_migration(conn_, &path,
                                                           util::timestamp());
        rv != 0) {
      std::cerr << "Client ngtcp2_conn_initiate_immediate_migration: "
                << ngtcp2_strerror(rv) << std::endl;
    }
  }

  ev_io_start(loop_, &ep.rev);

  return 0;
}

void Client::start_key_update_timer() {
  ev_timer_start(loop_, &key_update_timer_);
}

int Client::update_key(uint8_t *rx_secret, uint8_t *tx_secret,
                       ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
                       ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
                       const uint8_t *current_rx_secret,
                       const uint8_t *current_tx_secret, size_t secretlen) {
  if (!quic_connector_.getConfig().quiet) {
    std::cerr << "Client Updating traffic key" << std::endl;
  }

  auto crypto_ctx = ngtcp2_conn_get_crypto_ctx(conn_);
  auto aead = &crypto_ctx->aead;
  auto keylen = ngtcp2_crypto_aead_keylen(aead);
  auto ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);

  ++nkey_update_;

  std::array<uint8_t, 64> rx_key, tx_key;

  if (ngtcp2_crypto_update_key(conn_, rx_secret, tx_secret, rx_aead_ctx,
                               rx_key.data(), rx_iv, tx_aead_ctx, tx_key.data(),
                               tx_iv, current_rx_secret, current_tx_secret,
                               secretlen) != 0) {
    return -1;
  }

  if (!quic_connector_.getConfig().quiet && quic_connector_.getConfig().show_secret) {
    std::cerr << "Client application_traffic rx secret " << nkey_update_ << std::endl;
    debug::print_secrets({rx_secret, secretlen}, {rx_key.data(), keylen},
                         {rx_iv, ivlen});
    std::cerr << "Client application_traffic tx secret " << nkey_update_ << std::endl;
    debug::print_secrets({tx_secret, secretlen}, {tx_key.data(), keylen},
                         {tx_iv, ivlen});
  }

  return 0;
}

int Client::initiate_key_update() {
  if (!quic_connector_.getConfig().quiet) {
    std::cerr << "Client Initiate key update" << std::endl;
  }

  if (auto rv = ngtcp2_conn_initiate_key_update(conn_, util::timestamp());
      rv != 0) {
    std::cerr << "Client ngtcp2_conn_initiate_key_update: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

void Client::start_delay_stream_timer() {
  ev_timer_start(loop_, &delay_stream_timer_);
}

int Client::send_packet(const Endpoint &ep, const ngtcp2_addr &remote_addr,
                        unsigned int ecn, std::span<const uint8_t> data) {
  auto [_, rv] = send_packet(ep, remote_addr, ecn, data, data.size());

  return rv;
}

std::pair<std::span<const uint8_t>, int>
Client::send_packet(const Endpoint &ep, const ngtcp2_addr &remote_addr,
                    unsigned int ecn, std::span<const uint8_t> data,
                    size_t gso_size) {
  assert(gso_size);

  if (debug::packet_lost(quic_connector_.getConfig().tx_loss_prob)) {
    if (!quic_connector_.getConfig().quiet) {
      std::cerr << "Client ** Simulated outgoing packet loss **" << std::endl;
    }
    return {{}, NETWORK_ERR_OK};
  }

  if (no_gso_ && data.size() > gso_size) {
    for (; !data.empty();) {
      auto len = std::min(gso_size, data.size());

      auto [_, rv] =
        send_packet(ep, remote_addr, ecn, {std::begin(data), len}, len);
      if (rv != 0) {
        return {data, rv};
      }

      data = data.subspan(len);
    }

    return {{}, 0};
  }

  iovec msg_iov{
    .iov_base = const_cast<uint8_t *>(data.data()),
    .iov_len = data.size(),
  };

  uint8_t msg_ctrl[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(uint16_t))]{};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

  msghdr msg{
#ifdef HAVE_LINUX_RTNETLINK_H
    .msg_name = const_cast<sockaddr *>(remote_addr.addr),
    .msg_namelen = remote_addr.addrlen,
#endif // defined(HAVE_LINUX_RTNETLINK_H)
    .msg_iov = &msg_iov,
    .msg_iovlen = 1,
    .msg_control = msg_ctrl,
    .msg_controllen = sizeof(msg_ctrl),
  };

#pragma GCC diagnostic pop

  size_t controllen = 0;

  auto cm = CMSG_FIRSTHDR(&msg);
  controllen += CMSG_SPACE(sizeof(int));
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &ecn, sizeof(ecn));

  switch (remote_addr.addr->sa_family) {
  case AF_INET:
    cm->cmsg_level = IPPROTO_IP;
    cm->cmsg_type = IP_TOS;

    break;
  case AF_INET6:
    cm->cmsg_level = IPPROTO_IPV6;
    cm->cmsg_type = IPV6_TCLASS;

    break;
  default:
    assert(0);
  }

#ifdef UDP_SEGMENT
  if (data.size() > gso_size) {
    controllen += CMSG_SPACE(sizeof(uint16_t));
    cm = CMSG_NXTHDR(&msg, cm);
    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t n = gso_size;
    memcpy(CMSG_DATA(cm), &n, sizeof(n));
  }
#endif // UDP_SEGMENT

  msg.msg_controllen = controllen;

  ssize_t nwrite = 0;

  do {
    nwrite = sendmsg(ep.fd, &msg, 0);
  } while (nwrite == -1 && errno == EINTR);

  if (nwrite == -1) {
    switch (errno) {
    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
#endif // EAGAIN != EWOULDBLOCK
      return {data, NETWORK_ERR_SEND_BLOCKED};
#ifdef UDP_SEGMENT
    case EIO:
      if (data.size() > gso_size) {
        // GSO failure; send each packet in a separate sendmsg call.
        std::cerr << "Client sendmsg: disabling GSO due to " << strerror(errno)
                  << std::endl;

        no_gso_ = true;

        return send_packet(ep, remote_addr, ecn, data, gso_size);
      }
      break;
#endif // defined(UDP_SEGMENT)
    }

    std::cerr << "Client sendmsg: " << strerror(errno) << std::endl;

    // LEGACY ISSUE We have packet which is expected to fail to send (e.g.,
    // path validation to old path).
    return {{}, NETWORK_ERR_OK};
  }

  assert(static_cast<size_t>(nwrite) == data.size());

  if (!quic_connector_.getConfig().quiet) {
    std::cerr << "Client Sent packet: local="
              << util::straddr(&ep.addr.su.sa, ep.addr.len) << " remote="
              << util::straddr(remote_addr.addr, remote_addr.addrlen)
              << " ecn=0x" << std::hex << ecn << std::dec << " " << nwrite
              << " bytes" << std::endl;
  }

  return {{}, NETWORK_ERR_OK};
}

void Client::on_send_blocked(const Endpoint &ep, const ngtcp2_addr &remote_addr,
                             unsigned int ecn, std::span<const uint8_t> data,
                             size_t gso_size) {
  assert(tx_.num_blocked || !tx_.send_blocked);
  assert(tx_.num_blocked < 2);
  assert(gso_size);

  tx_.send_blocked = true;

  auto &p = tx_.blocked[tx_.num_blocked++];

  memcpy(&p.remote_addr.su, remote_addr.addr, remote_addr.addrlen);

  p.remote_addr.len = remote_addr.addrlen;
  p.endpoint = &ep;
  p.ecn = ecn;
  p.data = data;
  p.gso_size = gso_size;

  start_wev_endpoint(ep);
}

void Client::start_wev_endpoint(const Endpoint &ep) {
  // We do not close ep.fd, so we can expect that each Endpoint has
  // unique fd.
  if (ep.fd != wev_.fd) {
    if (ev_is_active(&wev_)) {
      ev_io_stop(loop_, &wev_);
    }

    ev_io_set(&wev_, ep.fd, EV_WRITE);
  }

  ev_io_start(loop_, &wev_);
}

int Client::send_blocked_packet() {
  assert(tx_.send_blocked);

  for (; tx_.num_blocked_sent < tx_.num_blocked; ++tx_.num_blocked_sent) {
    auto &p = tx_.blocked[tx_.num_blocked_sent];

    ngtcp2_addr remote_addr{
      .addr = &p.remote_addr.su.sa,
      .addrlen = p.remote_addr.len,
    };

    auto [rest, rv] =
      send_packet(*p.endpoint, remote_addr, p.ecn, p.data, p.gso_size);
    if (rv != 0) {
      assert(NETWORK_ERR_SEND_BLOCKED == rv);

      p.data = rest;

      start_wev_endpoint(*p.endpoint);

      return 0;
    }
  }

  tx_.send_blocked = false;
  tx_.num_blocked = 0;
  tx_.num_blocked_sent = 0;

  return 0;
}

int Client::handle_error() {
  if (!conn_ || ngtcp2_conn_in_closing_period(conn_) ||
      ngtcp2_conn_in_draining_period(conn_)) {
    return 0;
  }

  std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> buf;

  ngtcp2_path_storage ps;

  ngtcp2_path_storage_zero(&ps);

  ngtcp2_pkt_info pi;

  auto nwrite = ngtcp2_conn_write_connection_close(
    conn_, &ps.path, &pi, buf.data(), buf.size(), &last_error_,
    util::timestamp());
  if (nwrite < 0) {
    std::cerr << "Client ngtcp2_conn_write_connection_close: "
              << ngtcp2_strerror(nwrite) << std::endl;
    return -1;
  }

  if (nwrite == 0) {
    return 0;
  }

  return send_packet(*static_cast<Endpoint *>(ps.path.user_data),
                     ps.path.remote, pi.ecn,
                     {buf.data(), static_cast<size_t>(nwrite)});
}

int Client::on_stream_close(int64_t stream_id, uint64_t app_error_code) {
  if (httpconn_) {
    if (app_error_code == 0) {
      app_error_code = NGHTTP3_H3_NO_ERROR;
    }
    auto rv = nghttp3_conn_close_stream(httpconn_, stream_id, app_error_code);
    switch (rv) {
    case 0:
      break;
    case NGHTTP3_ERR_STREAM_NOT_FOUND:
      // We have to handle the case when stream opened but no data is
      // transferred.  In this case, nghttp3_conn_close_stream might
      // return error.
      if (!ngtcp2_is_bidi_stream(stream_id)) {
        assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
        ngtcp2_conn_extend_max_streams_uni(conn_, 1);
      }
      break;
    default:
      std::cerr << "Client nghttp3_conn_close_stream: " << nghttp3_strerror(rv)
                << std::endl;
      ngtcp2_ccerr_set_application_error(
        &last_error_, nghttp3_err_infer_quic_app_error_code(rv), nullptr, 0);
      return -1;
    }
  }

  return 0;
}

int Client::on_stream_reset(int64_t stream_id) {
  if (httpconn_) {
    if (auto rv = nghttp3_conn_shutdown_stream_read(httpconn_, stream_id);
        rv != 0) {
      std::cerr << "Client nghttp3_conn_shutdown_stream_read: " << nghttp3_strerror(rv)
                << std::endl;
      return -1;
    }
  }
  return 0;
}

int Client::on_stream_stop_sending(int64_t stream_id) {
  if (!httpconn_) {
    return 0;
  }

  if (auto rv = nghttp3_conn_shutdown_stream_read(httpconn_, stream_id);
      rv != 0) {
    std::cerr << "Client nghttp3_conn_shutdown_stream_read: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

int Client::make_stream_early() {
  if (setup_httpconn() != 0) {
    return -1;
  }

  return on_extend_max_streams();
}

int Client::on_extend_max_streams() {
  int64_t stream_id;

  if ((quic_connector_.getConfig().delay_stream && !handshake_confirmed_) ||
      ev_is_active(&delay_stream_timer_)) {
    return 0;
  }

  // TODONE: Use the requestResolutionQueue to drive the stream creation.
  while (quic_connector_.hasOutstandingRequest()) {
    if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
        rv != 0) {
        cerr << "Client Now Wanted to send request, but it appears blocked: " << stream_id << endl << flush;
        assert(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);
        break;
    }

    Request req = quic_connector_.getOutstandingRequest();
    auto stream = std::make_unique<ClientStream>(req, stream_id, this);

    if (submit_http_request(stream.get(), stream->req.isWWATP()) != 0) {
        break;
    }
    if (!req.isWWATP()) {
        quic_connector_.clearStaticRequest(req);
    }

    streams_.emplace(stream_id, std::move(stream));
    nstreams_done_++;
  }
  return 0;
}

bool Client::lock_outgoing_chunks(chunks &locked_chunks, vector<StreamIdentifier> const &sids, nghttp3_vec *vec, size_t veccnt)
{
    // Unlike the unlock_chunks function, this function has to actually do the buisness logic
    // bit where it pops the chunks from the outgoingChunks queue
    // But first zero out the vec, in case the outgoingChunks queue cannot fill it up
    std::fill(vec, vec + veccnt, nghttp3_vec{nullptr, 0});

    if (quic_connector_.noMoreChunks(sids))
    {
        // If there are no more chunks, we return is_closing
        return true;
    }
    // Ask the quic_connector_ for veccnt chunks
    chunks to_send = quic_connector_.popNOutgoingChunks(sids, veccnt);
    size_t vec_index = 0;

    for (auto chunk: to_send)
    {
        // We assume that the chunk is a shared_span<> and we can just use the base pointer
        // to lock it in the locked_chunks_ map
        auto locked_ptr = reinterpret_cast<uint8_t*>(chunk.use_chunk());
        vec[vec_index].base = locked_ptr;
        vec[vec_index].len = chunk.get_signal_size() + chunk.size();
        vec_index++;
    }
    locked_chunks.insert(locked_chunks.end(), std::make_move_iterator(to_send.begin()), std::make_move_iterator(to_send.end()));
    return false;
}

pair<size_t, vector<StreamIdentifier>> Client::get_pending_chunks_size(int64_t stream_id, size_t veccnt)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end())
    {
        // If the stream is not found, we return 0 size and an empty vector
        return {0, {}};
    }
    auto &stream = it->second;
    // Simply ask the quic_connector_ for the size of the pending chunks
    return quic_connector_.planForNOutgoingChunks(get_dcid(), veccnt, stream->req);
}

void Client::unlock_chunks(nghttp3_vec *vec, size_t veccnt)
{
    // Simply loop through the vec (up to veccnt limit) and call shared_span_decr_rc
    // on each vec[i].base
    for (size_t i = 0; i < veccnt; ++i)
    {
        auto it = locked_chunks_.find(vec[i].base);
        if (it != locked_chunks_.end())
        {
            //shared_span_decr_rc(vec[i].base);
        }
    }
}
void Client::shared_span_incr_rc(uint8_t *locked_ptr, shared_span<> &&to_lock)
{
    // use move semantics to put the to_lock into the locked_chunks_ map at the key == locked_ptr
    // we assume that the caller of shared_span_incr_rc will never use the same locked_ptr twice, which should be a solid assumption 
    // with respect to reading and writing with this class
    locked_chunks_.emplace(locked_ptr, std::move(to_lock));
}
void Client::shared_span_decr_rc(uint8_t *locked_ptr)
{
    locked_chunks_.erase(locked_ptr);
}

void Client::push_incoming_chunk(shared_span<> &&chunk, Request const &req)
{
    auto sid = get_dcid();
    // Push the chunk into the quic_connector_ requestResolutionQueue
    quic_connector_.pushIncomingChunk(sid, move(chunk), req);
}

namespace {
    nghttp3_ssize read_data(nghttp3_conn *, int64_t stream_id, nghttp3_vec *vec,
                        size_t veccnt, uint32_t *pflags, void * /* user_data */,
                        void *stream_user_data) {
        // First zero out the vec, for various cases where not all the data space is used.
        std::fill(vec, vec + veccnt, nghttp3_vec{nullptr, 0});

        // TODONE: Use the outgoingChunks queue instead of dyn_buf
        auto stream = static_cast<ClientStream *>(stream_user_data);
        bool is_closing = false;

        ngtcp2_conn_info ci;
        ngtcp2_conn_get_conn_info(stream->handler->conn(), &ci);
        auto pending = stream->get_pending_chunks_size(stream_id, veccnt);
        if (pending.first > ci.cwnd)
        {
            return NGHTTP3_ERR_WOULDBLOCK;
        }
        if (pending.first == 0) {
            is_closing = true;
        } else {
            is_closing = stream->lock_outgoing_chunks(pending.second, vec, veccnt);
        }
    
        if (is_closing) {
            *pflags |= NGHTTP3_DATA_FLAG_EOF;
        } else {
            *pflags |= NGHTTP3_DATA_FLAG_NO_END_STREAM;
        }
    
        return std::count_if(vec, vec + veccnt, [](const nghttp3_vec &v) {
            return v.base != nullptr; // Check if the base pointer is not nullptr
        });
    }
} // namespace

namespace
{
    nghttp3_ssize dyn_read_data(nghttp3_conn *conn, int64_t stream_id,
                                nghttp3_vec *vec, size_t veccnt, uint32_t *pflags,
                                void */* user_data */, void *stream_user_data)
    {
        return read_data(conn, stream_id, vec, veccnt, pflags, nullptr, stream_user_data);
    }
} // namespace

int Client::submit_http_request(ClientStream *stream, bool live_stream) {
  std::string content_length_str;

  std::array<nghttp3_nv, 6> nva{
    util::make_nv_nn(":method", stream->req.method),
    util::make_nv_nn(":scheme", stream->req.scheme),
    util::make_nv_nn(":authority", stream->req.authority),
    util::make_nv_nn(":path", stream->req.path),
    util::make_nv_nn("user-agent", "nghttp3/ngtcp2 client"),
  };
  size_t nvlen = 5;

  if (!quic_connector_.getConfig().quiet) {
    debug::print_http_request_headers(stream->stream_id, nva.data(), nvlen);
  }

  nghttp3_data_reader *dr_ptr = nullptr;
  nghttp3_data_reader dr{
    .read_data = read_data,
  };
  if (live_stream) {
    dr.read_data = dyn_read_data;
    dr_ptr = &dr;
  }

  if (auto rv = nghttp3_conn_submit_request(
    httpconn_, stream->stream_id, nva.data(), nvlen,
    dr_ptr, stream);
    rv != 0) {
    std::cerr << "Client nghttp3_conn_submit_request: " << nghttp3_strerror(rv)
            << std::endl;
    return -1;
  }

  ev_io_start(loop_, &wev_);
  return 0;
}

int Client::recv_stream_data(uint32_t flags, int64_t stream_id,
                             std::span<const uint8_t> data) {
    // I expect streams to be bidirectional if and only if it is a user space streams from QuicConnector 
    if (ngtcp2_is_bidi_stream(stream_id)) 
    {
        auto it = streams_.find(stream_id);
        assert(it != std::end(streams_));
        //auto &client_stream = (*it).second;
    }
    auto nconsumed =
        nghttp3_conn_read_stream(httpconn_, stream_id, data.data(), data.size(),
                                flags & NGTCP2_STREAM_DATA_FLAG_FIN);
    if (nconsumed < 0) {
        std::cerr << "Client nghttp3_conn_read_stream: " << nghttp3_strerror(nconsumed)
                << std::endl;
        ngtcp2_ccerr_set_application_error(
        &last_error_, nghttp3_err_infer_quic_app_error_code(nconsumed), nullptr,
        0);
        return -1;
    }

    ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
    ngtcp2_conn_extend_max_offset(conn_, nconsumed);

    return 0;
}

int Client::acked_stream_data_offset(int64_t stream_id, uint64_t datalen) {
  if (auto rv = nghttp3_conn_add_ack_offset(httpconn_, stream_id, datalen);
      rv != 0) {
    std::cerr << "Client nghttp3_conn_add_ack_offset: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

int Client::select_preferred_address(Address &selected_addr,
                                     const ngtcp2_preferred_addr *paddr) {
  auto path = ngtcp2_conn_get_path(conn_);

  switch (path->local.addr->sa_family) {
  case AF_INET:
    if (!paddr->ipv4_present) {
      return -1;
    }
    selected_addr.su.in = paddr->ipv4;
    selected_addr.len = sizeof(paddr->ipv4);
    break;
  case AF_INET6:
    if (!paddr->ipv6_present) {
      return -1;
    }
    selected_addr.su.in6 = paddr->ipv6;
    selected_addr.len = sizeof(paddr->ipv6);
    break;
  default:
    return -1;
  }

  if (!quic_connector_.getConfig().quiet) {
    char host[NI_MAXHOST], service[NI_MAXSERV];
    if (auto rv = getnameinfo(&selected_addr.su.sa, selected_addr.len, host,
                              sizeof(host), service, sizeof(service),
                              NI_NUMERICHOST | NI_NUMERICSERV);
        rv != 0) {
      std::cerr << "Client getnameinfo: " << gai_strerror(rv) << std::endl;
      return -1;
    }

    std::cerr << "Client selected server preferred_address is [" << host
              << "]:" << service << std::endl;
  }

  return 0;
}

namespace {
int http_recv_data(nghttp3_conn *, int64_t stream_id, const uint8_t *data,
                   size_t datalen, void *user_data, void *stream_user_data) {
    // TODONE: Put the data into the incomingChunks
    auto stream = static_cast<ClientStream *>(stream_user_data);
    auto client = static_cast<Client *>(user_data);

    ngtcp2_conn_info ci;

    ngtcp2_conn_get_conn_info(stream->handler->conn(), &ci);

    if (!client->getQuicConnector().getConfig().quiet && client->getQuicConnector().getConfig().http_dump) {
        debug::print_http_data(stream_id, {data, datalen});
    }
    client->http_consume(stream_id, datalen);
    // Then we have a full chunk, so we need to push it into the quic_connector_
    auto data_span = std::span<uint8_t>(const_cast<uint8_t*>(data), datalen);
    stream->append_data(data_span);
    return 0;
}
} // namespace

namespace {
int http_deferred_consume(nghttp3_conn *, int64_t stream_id,
                          size_t nconsumed, void *user_data,
                          void */* stream_user_data */) {
  auto c = static_cast<Client *>(user_data);
  c->http_consume(stream_id, nconsumed);
  return 0;
}
} // namespace

void Client::http_consume(int64_t stream_id, size_t nconsumed) {
  ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(conn_, nconsumed);
}

void Client::http_write_data(int64_t stream_id, std::span<const uint8_t> data) {
  auto it = streams_.find(stream_id);
  if (it == std::end(streams_)) {
    return;
  }

  auto &stream = (*it).second;

  if (stream->fd == -1) {
    return;
  }

  ssize_t nwrite;
  do {
    nwrite = write(stream->fd, data.data(), data.size());
  } while (nwrite == -1 && errno == EINTR);
}

namespace {
int http_begin_headers(nghttp3_conn *, int64_t stream_id, void *user_data,
                       void */*stream_user_data*/) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::print_http_begin_response_headers(stream_id);
    }
    return 0;
}
} // namespace

namespace {
int http_recv_header(nghttp3_conn *, int64_t stream_id, int32_t /* token */,
                     nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                     void *user_data, void */*stream_user_data*/) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::print_http_header(stream_id, name, value, flags);
    }
    return 0;
}
} // namespace

namespace {
int http_end_headers(nghttp3_conn *, int64_t stream_id, int /* fin */,
                     void *user_data, void */* stream_user_data */) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::print_http_end_headers(stream_id);
    }
    return 0;
}
} // namespace

namespace {
int http_begin_trailers(nghttp3_conn *, int64_t stream_id, void *user_data,
                        void */* stream_user_data */) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::print_http_begin_trailers(stream_id);
    }
    return 0;
}
} // namespace

namespace {
int http_recv_trailer(nghttp3_conn *, int64_t stream_id, int32_t /* token */,
                      nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                      void *user_data, void */* stream_user_data */) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::print_http_header(stream_id, name, value, flags);
    }
    return 0;
}
} // namespace

namespace {
int http_end_trailers(nghttp3_conn *, int64_t stream_id, int /* fin */,
                      void *user_data, void */*stream_user_data*/) {
    auto c = static_cast<Client *>(user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::print_http_end_trailers(stream_id);
    }
    return 0;
}
} // namespace

namespace {
int http_stop_sending(nghttp3_conn *, int64_t stream_id,
                      uint64_t app_error_code, void *user_data,
                      void */* stream_user_data */) {
  auto c = static_cast<Client *>(user_data);
  if (c->stop_sending(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::stop_sending(int64_t stream_id, uint64_t app_error_code) {
  if (auto rv =
        ngtcp2_conn_shutdown_stream_read(conn_, 0, stream_id, app_error_code);
      rv != 0) {
    std::cerr << "Client ngtcp2_conn_shutdown_stream_read: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }
  return 0;
}

namespace {
int http_reset_stream(nghttp3_conn *, int64_t stream_id,
                      uint64_t app_error_code, void *user_data,
                      void */* stream_user_data */) {
  auto c = static_cast<Client *>(user_data);
  if (c->reset_stream(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::reset_stream(int64_t stream_id, uint64_t app_error_code) {
  if (auto rv =
        ngtcp2_conn_shutdown_stream_write(conn_, 0, stream_id, app_error_code);
      rv != 0) {
    std::cerr << "Client ngtcp2_conn_shutdown_stream_write: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }
  return 0;
}

namespace {
int http_stream_close(nghttp3_conn *, int64_t stream_id,
                      uint64_t app_error_code, void *conn_user_data,
                      void */* stream_user_data */) {
  auto c = static_cast<Client *>(conn_user_data);
  if (c->http_stream_close(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::http_stream_close(int64_t stream_id, uint64_t app_error_code) {
  if (ngtcp2_is_bidi_stream(stream_id)) {
    assert(ngtcp2_conn_is_local_stream(conn_, stream_id));

    ++nstreams_closed_;
  } else {
    assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
    ngtcp2_conn_extend_max_streams_uni(conn_, 1);
  }

  // NOTE: The workaround for the new logical streams means that this even has no significance for the quic_connector_
  // unless it is a static asset

  if (auto it = streams_.find(stream_id); it != std::end(streams_)) {
    if (!quic_connector_.getConfig().quiet) {
      std::cerr << "Client HTTP stream " << stream_id << " closed with error code "
                << app_error_code << std::endl;
    }
    if(!it->second->req.isWWATP()) {
        it->second->sendCloseSignal();
    }
    streams_.erase(it);
  }

  return 0;
}

namespace {
int http_recv_settings(nghttp3_conn *, const nghttp3_settings *settings,
                       void *conn_user_data) {
    auto c = static_cast<Client *>(conn_user_data);
    if (!c->getQuicConnector().getConfig().quiet) {
        debug::print_http_settings(settings);
    }

    return 0;
}
} // namespace

int Client::setup_httpconn() {
  if (httpconn_) {
    return 0;
  }

  if (ngtcp2_conn_get_streams_uni_left(conn_) < 3) {
    std::cerr << "Client peer does not allow at least 3 unidirectional streams."
              << std::endl;
    return -1;
  }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
  nghttp3_callbacks callbacks{
    .stream_close = ::http_stream_close,
    .recv_data = ::http_recv_data,
    .deferred_consume = ::http_deferred_consume,
    .begin_headers = ::http_begin_headers,
    .recv_header = ::http_recv_header,
    .end_headers = ::http_end_headers,
    .begin_trailers = ::http_begin_trailers,
    .recv_trailer = ::http_recv_trailer,
    .end_trailers = ::http_end_trailers,
    .stop_sending = ::http_stop_sending,
    .reset_stream = ::http_reset_stream,
    .recv_settings = ::http_recv_settings,
  };
#pragma GCC diagnostic pop

  nghttp3_settings settings;
  nghttp3_settings_default(&settings);
  settings.qpack_max_dtable_capacity = 4_k;
  settings.qpack_blocked_streams = 100;

  auto mem = nghttp3_mem_default();

  if (auto rv =
        nghttp3_conn_client_new(&httpconn_, &callbacks, &settings, mem, this);
      rv != 0) {
    std::cerr << "Client nghttp3_conn_client_new: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  int64_t ctrl_stream_id;

  if (auto rv = ngtcp2_conn_open_uni_stream(conn_, &ctrl_stream_id, nullptr);
      rv != 0) {
    std::cerr << "Client ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv = nghttp3_conn_bind_control_stream(httpconn_, ctrl_stream_id);
      rv != 0) {
    std::cerr << "Client nghttp3_conn_bind_control_stream: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  if (!quic_connector_.getConfig().quiet) {
    fprintf(stderr, "http: control stream=%" PRIx64 "\n", ctrl_stream_id);
  }

  int64_t qpack_enc_stream_id, qpack_dec_stream_id;

  if (auto rv =
        ngtcp2_conn_open_uni_stream(conn_, &qpack_enc_stream_id, nullptr);
      rv != 0) {
    std::cerr << "Client ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv =
        ngtcp2_conn_open_uni_stream(conn_, &qpack_dec_stream_id, nullptr);
      rv != 0) {
    std::cerr << "Client ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv = nghttp3_conn_bind_qpack_streams(httpconn_, qpack_enc_stream_id,
                                                qpack_dec_stream_id);
      rv != 0) {
    std::cerr << "Client nghttp3_conn_bind_qpack_streams: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  if (!quic_connector_.getConfig().quiet) {
    fprintf(stderr,
            "http: QPACK streams encoder=%" PRIx64 " decoder=%" PRIx64 "\n",
            qpack_enc_stream_id, qpack_dec_stream_id);
  }

  return 0;
}

const std::vector<uint32_t> &Client::get_offered_versions() const {
  return offered_versions_;
}

bool Client::get_early_data() const { return early_data_; }

void Client::writecb_start() {
    ev_io_start(loop_, &wev_);
}

ngtcp2_cid const &Client::get_dcid() const
{
    ngtcp2_cid const *cid_ = ngtcp2_conn_get_client_initial_dcid(conn());
    return *cid_;   
}


namespace {
int run(Client &c, struct ev_loop *loop, const char *addr, const char *port,
        TLSClientContext &tls_ctx, atomic<bool> &stop) {
  Address remote_addr, local_addr;

  auto fd = create_sock(remote_addr, addr, port);
  if (fd == -1) {
    return -1;
  }

#ifdef HAVE_LINUX_RTNETLINK_H
  in_addr_union iau;

  if (get_local_addr(iau, remote_addr) != 0) {
    std::cerr << "Client Could not get local address" << std::endl;
    close(fd);
    return -1;
  }

  if (bind_addr(local_addr, fd, &iau, remote_addr.su.sa.sa_family) != 0) {
    close(fd);
    return -1;
  }
#else  // !defined(HAVE_LINUX_RTNETLINK_H)
  if (connect_sock(local_addr, fd, remote_addr) != 0) {
    close(fd);
    return -1;
  }
#endif // !defined(HAVE_LINUX_RTNETLINK_H)

  if (c.init(fd, local_addr, remote_addr, addr, port, tls_ctx) != 0) {
    return -1;
  }

  // LEGACY ISSUE Do we need this ?
  if (auto rv = c.on_write(); rv != 0) {
    return rv;
  }

  while(!stop.load()) {
    if(ev_run(loop, EVRUN_ONCE) == 0) {
      break;
    }
  }

  return 0;
}
} // namespace


StreamIdentifier QuicConnector::getNewRequestStreamIdentifier(Request const &req) {
    if (!req.isWWATP()) {
        lock_guard<std::mutex> lock(returnPathsMutex);
        // This is not a WWATP request, so check if it is already in the staticRequests
        auto findit = staticRequests.find(req);
        if (findit != staticRequests.end()) {
            return findit->second;
        }
    }
    // This needs to read the cid from the client, increment the global_stream_id per the client,
    // and also add the StreamIdentifier and Request to the stream_return_paths
    ngtcp2_cid cid = client->get_dcid();
    // Use the atomic stream_id_counter to simultaneously increment and return the stream_id
    auto stream_id = stream_id_counter.fetch_add(2);
    auto newStreamIdentifier = StreamIdentifier(cid, stream_id);
    while(returnPaths.find(newStreamIdentifier) != returnPaths.end()) {
        // If the newStreamIdentifier is already in the returnPaths, we need to increment the stream_id
        stream_id = stream_id_counter.fetch_add(2);
        newStreamIdentifier = StreamIdentifier(cid, stream_id);
    }
    lock_guard<std::mutex> lock(returnPathsMutex);
    if (!req.isWWATP()) {
        // This is not a WWATP request, so add it to the staticRequests
        staticRequests.insert(make_pair(req, newStreamIdentifier));
    }
    returnPaths.insert(make_pair(newStreamIdentifier, req));
    return newStreamIdentifier;
}

void QuicConnector::registerResponseHandler(StreamIdentifier sid, stream_callback_fn cb) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    requestorQueue.push_back(std::make_pair(sid, cb));
}

bool QuicConnector::hasResponseHandler(StreamIdentifier sid) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    auto it = std::find_if(requestorQueue.begin(), requestorQueue.end(),
        [&sid](const stream_callback& stream_cb) {
            return stream_cb.first == sid;
        });
    if (it != requestorQueue.end()) {
        // The stream callback is already in the queue
        return true;
    }
    return false;
}

void QuicConnector::deregisterResponseHandler(StreamIdentifier sid) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    auto it = std::remove_if(requestorQueue.begin(), requestorQueue.end(),
        [&sid](const stream_callback& stream_cb) {
            return stream_cb.first == sid;
        });
    requestorQueue.erase(it, requestorQueue.end());
}

bool QuicConnector::processRequestStream() {
    std::unique_lock<std::mutex> requestorLock(requestorQueueMutex);
    if (!requestorQueue.empty()) {
        stream_callback stream_cb = requestorQueue.front();
        requestorQueue.erase(requestorQueue.begin());
        requestorLock.unlock();
        StreamIdentifier stream_id = stream_cb.first;
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        stream_callback_fn fn = stream_cb.second;
        chunks to_process;
        bool signal_closed = false;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_id);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        chunks processed = fn(stream_cb.first, to_process);
        size_t chunk_count = processed.size();
        if(!processed.empty())
        {   
            if (processed.size() == 1 && processed[0].get_signal_type() == signal_chunk_header::GLOBAL_SIGNAL_TYPE) {
                // If the processed chunk is a signal chunk, we need to handle it differently
                auto signal = processed[0].get_signal<signal_chunk_header>();
                if (signal.signal == signal_chunk_header::SIGNAL_CLOSE_STREAM) {
                    // If the signal is to close the stream, we need to set the signal_closed flag
                    signal_closed = true;
                }
            }
            else {
                lock_guard<std::mutex> lock(outgoingChunksMutex);
                auto outgoing = outgoingChunks.find(stream_cb.first);
                if (outgoing == outgoingChunks.end()) {
                    auto inserted = outgoingChunks.insert(make_pair(stream_cb.first, chunks()));
                    inserted.first->second.swap(processed);
                }
                else {
                    for (auto &chunk : processed) {
                        outgoing->second.emplace_back(chunk);
                    }
                }
            }
        }
        if (!signal_closed) {
            // If the stream is not closed, we need to push it back to the requestorQueue
            // This is done by locking the requestorQueueMutex again
            requestorLock.lock();
            requestorQueue.push_back(stream_cb);
            if (chunk_count > 0) {
                // If there are chunks to process, we need to start the write event
                client->writecb_start();
            }
        }
        else {
            // If the stream is closed, we need to also remove any remainders from the incomingChunks and outgoingChunks
            {
                lock_guard<std::mutex> lock(incomingChunksMutex);
                incomingChunks.erase(stream_cb.first);
            }
            {
                lock_guard<std::mutex> lock(outgoingChunksMutex);
                outgoingChunks.erase(stream_cb.first);
            }
            {
                // TODO: Garbage collect old unsused returnPaths
                //lock_guard<std::mutex> lock(returnPathsMutex);
                //returnPaths.erase(stream_cb.first);
                // Don't erase from the staticRequests though
            }
            // And by definition, it is not in the requestorQueue anymore
        }
    } else {
        return false;
    }
    return true;
}

void QuicConnector::registerRequestHandler(named_prepare_fn preparer)
{
    lock_guard<std::mutex> lock(preparerStackMutex);
    preparersStack.push_back(preparer);
}

void QuicConnector::deregisterRequestHandler(string preparer_name)
{
    lock_guard<std::mutex> lock(preparerStackMutex);
    auto it = std::remove_if(preparersStack.begin(), preparersStack.end(),
        [&preparer_name](const named_prepare_fn& preparer) {
            return preparer.first == preparer_name;
        });
    preparersStack.erase(it, preparersStack.end());
}

bool QuicConnector::processResponseStream() {
    std::unique_lock<std::mutex> responderLock(responderQueueMutex);
    if (!responderQueue.empty()) {
        stream_callback stream_cb = responderQueue.front();
        responderQueue.erase(responderQueue.begin());
        responderLock.unlock();
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        stream_callback_fn fn = stream_cb.second;
        chunks to_process;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_cb.first);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        chunks processed = fn(stream_cb.first, to_process);
        if(!processed.empty())
        {   
            lock_guard<std::mutex> lock(outgoingChunksMutex);
            auto outgoing = outgoingChunks.find(stream_cb.first);
            if (outgoing == outgoingChunks.end()) {
                auto inserted = outgoingChunks.insert(make_pair(stream_cb.first, chunks()));
                inserted.first->second.swap(processed);
            }
            else {
                for (auto &chunk : processed) {
                    outgoing->second.emplace_back(chunk);
                }
            }
        }
        responderLock.lock();
        responderQueue.push_back(stream_cb);
    } else {
        return false;
    }
    return true;
}

void QuicConnector::check_deadline() {
    if (timer.expires_at() <= boost::asio::deadline_timer::traits_type::now()) {
        timed_out = true;
        socket.cancel();
        timer.expires_at(boost::posix_time::pos_infin);
    }
    timer.async_wait([this](const boost::system::error_code&) { check_deadline(); });
}

void QuicConnector::listen(const string &, const string&, int) {
    // throw an exception, not supported
    throw std::runtime_error("QuicConnector::listen is not supported");
}

void QuicConnector::close() {
    terminate_.store(true);
    if (reqrep_thread_.joinable()) {
        reqrep_thread_.join();
    }
}

void QuicConnector::connect(const string &peer_name, const string& peer_ip_addr, int peer_port) {
    reqrep_thread_ = thread([this, peer_name, peer_ip_addr, peer_port]() {

        if (!ngtcp2_is_reserved_version(config.version)) {
            if (!config.preferred_versions.empty() &&
                std::find(std::begin(config.preferred_versions),
                            std::end(config.preferred_versions),
                            config.version) == std::end(config.preferred_versions)) {
                std::cerr << "Client preferred-version: must include version " << std::hex
                        << "0x" << config.version << std::dec << std::endl;
                exit(EXIT_FAILURE);
            }

            if (!config.available_versions.empty() &&
                std::find(std::begin(config.available_versions),
                            std::end(config.available_versions),
                            config.version) == std::end(config.available_versions)) {
                std::cerr << "Client available-versions: must include version " << std::hex
                        << "0x" << config.version << std::dec << std::endl;
                exit(EXIT_FAILURE);
            }
        }

        TLSClientConfig tls_config{
            .session_file = config.session_file,
            .groups = config.groups,
            .sni = config.sni,
            .disable_early_data = config.disable_early_data
        };
        TLSClientContext tls_ctx(tls_config);
        if (tls_ctx.init(private_key_file.c_str(), cert_file.c_str()) != 0) {
            exit(EXIT_FAILURE);
        }

        auto ev_loop_d = defer(ev_loop_destroy, loop);

        auto keylog_filename = getenv("SSLKEYLOGFILE");
        if (keylog_filename) {
            keylog_file.open(keylog_filename, std::ios_base::app);
            if (keylog_file) {
            tls_ctx.enable_keylog();
            }
        }

        if (util::generate_secret(config.static_secret) != 0) {
            std::cerr << "Client Unable to generate static secret" << std::endl;
            exit(EXIT_FAILURE);
        }

        auto client_chosen_version = config.version;

        while (!terminate_.load()) {
            Client c(loop, client_chosen_version, config.version, *this);
            client = &c;

            auto port_str = std::to_string(peer_port);
            if (run(c, loop, peer_ip_addr.c_str(), port_str.c_str(), tls_ctx, terminate_) != 0) {
                if(!terminate_.load()) {
                    exit(EXIT_FAILURE);
                }
            }

            if (config.preferred_versions.empty()) {
                break;
            }

            auto &offered_versions = c.get_offered_versions();
            if (offered_versions.empty()) {
                break;
            }

            client_chosen_version = ngtcp2_select_version(
                config.preferred_versions.data(), config.preferred_versions.size(),
                offered_versions.data(), offered_versions.size());

            if (client_chosen_version == 0) {
                std::cerr << "Client Unable to select a version" << std::endl;
                exit(EXIT_FAILURE);
            }

            if (!config.quiet) {
                std::cerr << "Client Client selected version " << std::hex << "0x"
                        << client_chosen_version << std::dec << std::endl;
            }
        }
        return EXIT_SUCCESS;
    });
}

set<Request> QuicConnector::getCurrentRequests()
{
    set<StreamIdentifier> outgoingIDSet;
    {
        lock_guard<std::mutex> lock(outgoingChunksMutex);
        for (auto &outgoing : outgoingChunks) {
            outgoingIDSet.insert(outgoing.first);
        }
    }
    set<Request> requestSet;
    {
        lock_guard<std::mutex> lock(returnPathsMutex);
        for (auto &sid : outgoingIDSet) {
            auto request = returnPaths.find(sid);
            if (request == returnPaths.end()) {
                continue;
            }
            requestSet.insert(request->second);
        }
    }
    return requestSet;
}

bool QuicConnector::hasOutstandingRequest()
{
    set<Request> requestSet = getCurrentRequests();
    for (auto &request : requestSet) {
        if (!client->active_request(request)) {
            return true;
        }
    }
    return false;
}

Request QuicConnector::getOutstandingRequest()
{
    set<Request> requestSet = getCurrentRequests();
    for (auto &request : requestSet) {
        if (!client->active_request(request)) {
            return request;
        }
    }
    throw std::runtime_error("No request found in the requestSet, but getRequest should only be called when there is a request in the set");
}

void QuicConnector::clearStaticRequest(Request &req)
{
    unique_lock<std::mutex> srlock(returnPathsMutex);
    auto findit = staticRequests.find(req);
    if(findit == staticRequests.end()) {
        assert(false && "clearStaticRequest called with a request that is not in the static requests");
        return;
    }
    StreamIdentifier sid = findit->second;
    srlock.unlock();

    unique_lock<std::mutex> oc_lock(outgoingChunksMutex);
    auto it = outgoingChunks.find(sid);
    if (it != outgoingChunks.end()) {
        // Clear placeholder chunks for this request by checking the signal type of the chunk
        // Filter out placeholder chunks from it->second using std::remove_if
        it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
            [](const shared_span<> &chunk) {
                return (chunk.get_signal_type() == payload_chunk_header::GLOBAL_SIGNAL_TYPE) &&
                       (chunk.get_signal_signal() == payload_chunk_header::SIGNAL_PLACEHOLDER);
            }), it->second.end());
        if (it->second.empty())
        {
            outgoingChunks.erase(it); // Remove the entry if empty
        }
    }    
}

void QuicConnector::initializeConfig(const YAML::Node& yaml_config) {
    using namespace std::literals;
    
    if (!yaml_config["log_path"]) {
        throw std::runtime_error("log_path is not specified in the configuration");
    }
    // Initialize paths similar to library_tester.cpp
    string log_path = yaml_config["log_path"].as<std::string>();
    std::string session_filename, tp_filename;
    session_filename = log_path + "/session_log_listener";
    tp_filename = log_path + "/tp_log_listener";
    
    set<string> non_optional_fields = { 
        "private_key_file",
        "cert_file",
        "log_path"
    };

    // Initialize the optional_fields set by looping through the YAML config node, assuming it is a map
    set<string> seen_fields;
    if (yaml_config.IsMap()) {
        for (const auto& field : yaml_config) {
            if (field.first.IsScalar()) {
                // If the field is in the non_optional_fields set, we skip it
                if (non_optional_fields.find(field.first.as<std::string>()) != non_optional_fields.end()) {
                    continue; // already handled
                }
                seen_fields.insert(field.first.as<std::string>());
            }
        }
    }
    set<string> non_provided_fields;

    // Create a templated helper lambda which will either evaluate to the default value, or to the value from the YAML config node,
    // and if it exists in the YAML config node, then also remove it from the optional_fields set
    // and if it does not exist, then add it to the non_provided_fields set.
    auto get = [&yaml_config, &non_provided_fields, &seen_fields](const std::string& field_name, auto default_value) {
        if (yaml_config[field_name]) {
            seen_fields.erase(field_name);
            return yaml_config[field_name].as<decltype(default_value)>();
        } else {
            non_provided_fields.insert(field_name);
            return default_value;
        }
    };


    config = ClientConfig{
        .tx_loss_prob = get("tx_loss_prob", 0.),
        .rx_loss_prob = get("rx_loss_prob", 0.),
        .groups = get("groups", string(ngtcp2::util::crypto_default_groups())),
        .port = get("port", (uint16_t)8443),
        .version = NGTCP2_PROTO_VER_V1, // NOT a user parameter (for now)
        .quiet = get("quiet", true),
        .timeout = get("timeout", 30 * NGTCP2_SECONDS),
        .show_secret = get("show_secret", false), 
        .early_response = get("early_response", false),
        .session_file = get("session_file", session_filename),
        .tp_file = get("tp_file", tp_filename),
        .change_local_addr = get("change_local_addr", (ngtcp2_duration)0),
        .key_update = get("key_update", (ngtcp2_duration)0),
        .delay_stream = get("delay_stream", (ngtcp2_duration)0),
        .nat_rebinding = get("nat_rebinding", false),
        .no_preferred_addr = get("no_preferred_addr", false),
        .qlog_file = get("qlog_file", string("")),
        .qlog_dir = get("qlog_dir", string("")),
        .quic_dump = get("quic_dump", false),
        .http_dump = get("http_dump", false),
        .max_data = get("max_data", 24_m),
        .max_stream_data_bidi_local = get("max_stream_data_bidi_local", 16_m),
        .max_stream_data_bidi_remote = get("max_stream_data_bidi_remote", 256_k),
        .max_stream_data_uni = get("max_stream_data_uni", 256_k),
        .max_streams_bidi = get("max_streams_bidi", 100UL),
        .max_streams_uni = get("max_streams_uni", 100UL),
        .max_window = get("max_window", 6_m),
        .max_stream_window = get("max_stream_window", 6_m),
        .disable_early_data = get("disable_early_data", false),
        .static_secret = {}, // NOT a user parameter
        .cc_algo = NGTCP2_CC_ALGO_CUBIC,  // NOT a user parameter (for now)
        .token_file = get("token_file", string("")),
        .sni = get("sni", string("")),
        .initial_rtt = get("initial_rtt", NGTCP2_DEFAULT_INITIAL_RTT),
        .max_udp_payload_size = get("max_udp_payload_size", (size_t)0),
        .send_trailers = get("send_trailers", false),
        .handshake_timeout = get("handshake_timeout", UINT64_MAX),
        .preferred_versions = get("preferred_versions", std::vector<uint32_t>{}), 
        .available_versions = get("available_versions", std::vector<uint32_t>{}), 
        .no_pmtud = get("no_pmtud", false),
        .ack_thresh = get("ack_thresh", (size_t)2),
        .initial_pkt_num = get("initial_pkt_num", UINT32_MAX),
        .pmtud_probes = get("pmtud_probes", std::vector<uint16_t>{}), // TODO: Default added from Config struct initializers
    };

    map<string, map<const char, size_t>> bag_of_characters;
    for (const auto& field : non_provided_fields) {
        // For each non provided field, we will compute a "bag of characters" and store it in the bag_of_characters map
        // This will be used to compute similarity later
        // Loop or characters in the field name, and construct a map of character to character count;
        map<const char, size_t> field_value;
        for (const char& c : field) {
            field_value[c]++;
        }
        bag_of_characters[field] = std::move(field_value);
    }

    // A lambda function to compute the similarity between two fields, will add up the absolute value of the differences per character
    auto compute_similarity = [](map<const char, size_t> seen_field_chars, map<const char, size_t> other) {
        size_t similarity = 0;
        for (const auto& [c, count] : seen_field_chars) {
            // If the character is not in the other map, we add the count to the similarity
            if (other.find(c) == other.end()) {
                similarity += count*2;
            } else {
                similarity += std::abs(static_cast<int>(count) - static_cast<int>(other.at(c)));
            }
        }
        // Do it again, only reverse the seen_field_chars and other
        for (const auto& [c, count] : other) {
            // If the character is not in the seen_field_chars map, we add the count to the similarity
            if (seen_field_chars.find(c) == seen_field_chars.end()) {
                similarity += count*2;
            } else {
                similarity += std::abs(static_cast<int>(count) - static_cast<int>(seen_field_chars.at(c)));
            }
        }
        return similarity/2;  // Divide by 2, since we counted each character twice
    };

    // A lambda function which will compute the top 5 most similar fields from the non_provided_fields set, and return them as a vector of strings
    auto get_top_similar_fields = [&bag_of_characters, &compute_similarity](const string& seen_field) {
        vector<pair<string, size_t>> similarities;
        map<const char, size_t> seen_field_chars;
        for (auto c : seen_field) {
            seen_field_chars[c]++;
        }
        for (const auto& field : bag_of_characters) {
            // Compute the similarity between the field and each seen field
            size_t similarity = compute_similarity(seen_field_chars, field.second);
            similarities.emplace_back(field.first, similarity);
        }
        // Sort the similarities by the similarity value, in descending order
        std::sort(similarities.begin(), similarities.end(),
            [](const pair<string, size_t>& a, const pair<string, size_t>& b) {
                return a.second > b.second;
            });
        // Drop similiarities past the top 5
        similarities.resize(std::min(similarities.size(), size_t(5)));
        return similarities;
    };

    // Now, print warning messages for any seen_fields that are left.
    // Using "bag of characters" approach to compute similiarity, print out the top 5 most similiar fields from the non_provided_fields set
    if (!seen_fields.empty()) {
        std::cerr << "Warning! QuicConnector::initializeConfig: The following fields were provided in the YAML config, but not found in the ClientConfig struct and will be set to default values: ";
        for (const auto& field : seen_fields) {
            auto similiarities = get_top_similar_fields(field);
            string suggestion_str;
            if (similiarities.size())
               suggestion_str  = " (Did you mean one of : ";
            for (const auto& [suggestion, similarity] : similiarities) {
                suggestion_str += suggestion + ", ";
            }
            if (similiarities.size()) {
                suggestion_str.pop_back(); // Remove the last comma
                suggestion_str.pop_back(); // Remove the last space
                suggestion_str += "?)";
            } else {
                suggestion_str = ".";
            }
            std::cerr << field << suggestion_str << " ";
        }
    }
}
