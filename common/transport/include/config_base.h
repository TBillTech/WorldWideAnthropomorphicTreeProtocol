#pragma once
#include <string>
#include <string_view>
#include <deque>
#include <unordered_map>
#include <vector>
#include <array>
#include <ngtcp2/ngtcp2.h>
#include <iostream>
#include <ngtcp2/ngtcp2_crypto.h>

#include "network.h"
#include "util.h"
#include "request.h"

struct Config {
  ngtcp2::Address preferred_ipv4_addr;
  ngtcp2::Address preferred_ipv6_addr;
  ngtcp2_cid dcid;
  ngtcp2_cid scid;
  bool scid_present;
  // tx_loss_prob is probability of losing outgoing packet.
  double tx_loss_prob;
  // rx_loss_prob is probability of losing incoming packet.
  double rx_loss_prob;
  // ciphers is the list of enabled ciphers.
  int fd;
  // ciphers is the list of enabled ciphers.
  std::string ciphers;
  // groups is the list of supported groups.
  std::string groups;
  // htdocs is a root directory to serve documents.
  std::string htdocs;
  // mime_types_file is a path to "MIME media types and the
  // extensions" file.  Ubuntu mime-support package includes it in
  // /etc/mime/types.
  // mime_types maps file extension to MIME media type.
  std::string mime_types_file;
  std::unordered_map<std::string, std::string> mime_types;
  // port is the port number which server listens on for incoming
  // connections.
  uint16_t port;
  // nstreams is the number of streams to open.
  size_t nstreams;
  // data is the pointer to memory region which maps file denoted by
  // fd.
  uint8_t *data;
  // datalen is the length of file denoted by fd.
  size_t datalen;
  // version is a QUIC version to use.
  uint32_t version;
  // quiet suppresses the output normally shown except for the error
  // messages.
  bool quiet;
  // timeout is an idle timeout for QUIC connection.
  ngtcp2_duration timeout;
  // show_secret is true if transport secrets should be printed out.
  bool show_secret;
  // validate_addr is true if server requires address validation.
  bool validate_addr;
  // early_response is true if server starts sending response when it
  // receives HTTP header fields without waiting for request body.  If
  // HTTP response data is written before receiving request body,
  // STOP_SENDING is sent.
  bool early_response;
  // verify_client is true if server verifies client with X.509
  // certificate based authentication.
  bool verify_client;
  // session_file is a path to a file to write, and read TLS session.
  std::string session_file;
  // tp_file is a path to a file to write, and read QUIC transport
  // parameters.
  std::string tp_file;
  // keylog_file is a path to a file to write key logging.
  std::string keylog_filename;
  // change_local_addr is the duration after which client changes
  // local address.
  ngtcp2_duration change_local_addr;
  // key_update is the duration after which client initiates key
  // update.
  ngtcp2_duration key_update;
  // delay_stream is the duration after which client sends the first
  // 1-RTT stream.
  ngtcp2_duration delay_stream;
  // nat_rebinding is true if simulated NAT rebinding is enabled.
  bool nat_rebinding;
  // no_preferred_addr is true if client do not follow preferred
  // address offered by server.
  bool no_preferred_addr;
  // download is a path to a directory where a downloaded file is
  // saved.  If it is empty, no file is saved.
  std::string_view download;
  // requests contains URIs to request.
  std::vector<Request> requests;
  // qlog_dir is the path to directory where qlog is stored.
  // qlog_file is the path to write qlog.
  std::string_view qlog_file;
  // qlog_dir is the path to directory where qlog is stored.  qlog_dir
  // and qlog_file are mutually exclusive.
  std::string_view qlog_dir;
  // no_quic_dump is true if hexdump of QUIC STREAM and CRYPTO data
  // should be disabled.
  bool no_quic_dump;
  // no_http_dump is true if hexdump of HTTP response body should be
  // disabled.
  bool no_http_dump;
  // max_data is the initial connection-level flow control window.
  uint64_t max_data;
  // max_stream_data_bidi_local is the initial stream-level flow
  // control window for a bidirectional stream that the local endpoint
  // initiates.
  uint64_t max_stream_data_bidi_local;
  // max_stream_data_bidi_remote is the initial stream-level flow
  // control window for a bidirectional stream that the remote
  // endpoint initiates.
  uint64_t max_stream_data_bidi_remote;
  // max_stream_data_uni is the initial stream-level flow control
  // window for a unidirectional stream.
  uint64_t max_stream_data_uni;
  // max_streams_bidi is the number of the concurrent bidirectional
  // streams.
  uint64_t max_streams_bidi;
  // max_streams_uni is the number of the concurrent unidirectional
  // streams.
  uint64_t max_streams_uni;
  // max_window is the maximum connection-level flow control window
  // size if auto-tuning is enabled.
  uint64_t max_window;
  // max_stream_window is the maximum stream-level flow control window
  // size if auto-tuning is enabled.
  uint64_t max_stream_window;
  // max_dyn_length is the maximum length of dynamically generated
  // response.
  uint64_t max_dyn_length;
  // exit_on_first_stream_close is the flag that if it is true, client
  // exits when a first HTTP stream gets closed.  It is not
  // necessarily the same time when the underlying QUIC stream closes
  // due to the QPACK synchronization.
  bool exit_on_first_stream_close;
  // exit_on_all_streams_close is the flag that if it is true, client
  // exits when all HTTP streams get closed.
  bool exit_on_all_streams_close;
  // disable_early_data disables early data.
  bool disable_early_data;
  // static_secret is used to derive keying materials for Retry and
  // Stateless Retry token.
  std::array<uint8_t, 32> static_secret;
  // cc_algo is the congestion controller algorithm.
  ngtcp2_cc_algo cc_algo;
  // token_file is a path to file to read or write token from
  // NEW_TOKEN frame.
  std::string_view token_file;
  // sni is the value sent in TLS SNI, overriding DNS name of the
  // remote host.
  std::string_view sni;
  // initial_rtt is an initial RTT.
  ngtcp2_duration initial_rtt;
  // max_udp_payload_size is the maximum UDP payload size that server
  // transmits.
  size_t max_udp_payload_size;
  // send_trailers controls whether server sends trailer fields or
  // not.
  bool send_trailers;
  // handshake_timeout is the period of time before giving up QUIC
  // connection establishment.
  ngtcp2_duration handshake_timeout;
  // preferred_versions includes QUIC versions in the order of
  // preference.  Server negotiates one of those versions if a client
  // initially selects a less preferred version.
  std::vector<uint32_t> preferred_versions;
  // available_versions includes QUIC versions that are sent in
  // available_versions field of version_information
  // transport_parameter.
  std::vector<uint32_t> available_versions;
  // no_pmtud disables Path MTU Discovery.
  bool no_pmtud;
  // ack_thresh is the minimum number of the received ACK eliciting
  // packets that triggers immediate acknowledgement.
  size_t ack_thresh;
  bool wait_for_ticket;
  // initial_pkt_num is the initial packet number for each packet
  // number space.  If it is set to UINT32_MAX, it is chosen randomly.
  uint32_t initial_pkt_num;
  // pmtud_probes is the array of UDP datagram payload size to probes.
  std::vector<uint16_t> pmtud_probes;
};

extern Config config;
extern std::ofstream keylog_file;
