#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <ev.h>
#include <set>
#include <list>
#include <yaml-cpp/yaml.h>
#include "config_base.h"
#include "template.h"
#include "util.h"

using namespace std;

struct ServerConfig {
  ngtcp2::Address preferred_ipv4_addr;
  ngtcp2::Address preferred_ipv6_addr;
  // tx_loss_prob is probability of losing outgoing packet.
  double tx_loss_prob;
  // rx_loss_prob is probability of losing incoming packet.
  double rx_loss_prob;
  // groups is the list of supported groups.
  std::string groups;
  // mime_types_file is a path to "MIME media types and the
  // extensions" file.  Ubuntu mime-support package includes it in
  // /etc/mime/types.
  // mime_types maps file extension to MIME media type.
  std::string mime_types_file;
  std::unordered_map<std::string, std::string> mime_types;
  // port is the port number which server listens on for incoming
  // connections.
  uint16_t port;
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
  // keylog_file is a path to a file to write key logging.
  std::string keylog_filename;
  // qlog_dir is the path to directory where qlog is stored.  qlog_dir
  // and qlog_file are mutually exclusive.
  std::string qlog_dir;
  // quic_dump is false if hexdump of QUIC STREAM and CRYPTO data
  // should be disabled.
  bool quic_dump;
  // http_dump is false if hexdump of HTTP response body should be
  // disabled.
  bool http_dump;
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
  // static_secret is used to derive keying materials for Retry and
  // Stateless Retry token.
  std::array<uint8_t, 32> static_secret;
  // cc_algo is the congestion controller algorithm.
  ngtcp2_cc_algo cc_algo;
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
  // initial_pkt_num is the initial packet number for each packet
  // number space.  If it is set to UINT32_MAX, it is chosen randomly.
  uint32_t initial_pkt_num;
  // pmtud_probes is the array of UDP datagram payload size to probes.
  std::vector<uint16_t> pmtud_probes;
};

void sigterminatehandler(struct ev_loop *loop, ev_async *watcher, int revents);

class Server;

class QuicListener : public Communication {
public:
    void initializeConfig(const YAML::Node& yaml_config);
    
    QuicListener(boost::asio::io_context& io_context, const YAML::Node& yaml_config)
        : io_context(io_context), 
          private_key_file(yaml_config["private_key_file"].as<string>("")), 
          cert_file(yaml_config["cert_file"].as<string>("")),
          socket(io_context), timer(io_context) {
        if (private_key_file.empty()) {
            throw std::runtime_error("QuicListener requires 'private_key_file' in yaml_config");
        }
        if (cert_file.empty()) {
            throw std::runtime_error("QuicListener requires 'cert_file' in yaml_config");
        }
        initializeConfig(yaml_config);
        loop = ev_loop_new(EVFLAG_AUTO);
        ev_async_init(&async_terminate, sigterminatehandler);
        ev_async_start(loop, &async_terminate);
    };
    ~QuicListener() override {
        terminate();
    }
    StreamIdentifier getNewRequestStreamIdentifier(Request const &req) override;
    void registerResponseHandler(StreamIdentifier sid, stream_callback_fn cb) override;
    bool hasResponseHandler(StreamIdentifier sid) override;
    void deregisterResponseHandler(StreamIdentifier sid) override;
    bool processRequestStream() override;
    void registerRequestHandler(named_prepare_fn preparer) override;
    void deregisterRequestHandler(string preparer_name) override;
    uri_response_info prepareInfo(const Request& uri);
    uri_response_info prepareHandler(StreamIdentifier stream_id, const Request& uri);
    uri_response_info prepareStaticHandler(ngtcp2_cid stream_id, const Request& uri);
    bool processResponseStream() override;
    void check_deadline();
    void listen(const string &local_name, const string& local_ip_addr, int local_port) override;
    void close() override;
    void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) override;

    void receiveSignal(StreamIdentifier const& sid, shared_span<> && signal) {
        lock_guard<std::mutex> lock(incomingChunksMutex);
        auto outgoing = incomingChunks.find(sid);
        if (outgoing == incomingChunks.end()) {
            auto inserted = incomingChunks.insert(make_pair(sid, chunks()));
            inserted.first->second.emplace_back(std::move(signal)); // Move the signal
        } else {
            outgoing->second.emplace_back(std::move(signal)); // Move the signal
        }
    }

    chunks popNOutgoingChunks(vector<StreamIdentifier> sids, size_t n) {
        lock_guard<std::mutex> lock(outgoingChunksMutex);
        chunks to_return;
        for (auto sid : sids)
        {
            auto outgoing = outgoingChunks.find(sid);
    
            if (outgoing != outgoingChunks.end()) {
                // Determine how many chunks to move (up to n or the size of outgoing->second)
                size_t count = std::min(n, outgoing->second.size());
        
                // Move the first `count` elements from outgoing->second to to_return
                auto begin = outgoing->second.begin();
                auto end = std::next(begin, count);
                to_return.insert(to_return.end(),
                                 std::make_move_iterator(begin),
                                 std::make_move_iterator(end));
        
                // Erase the moved elements from outgoing->second
                outgoing->second.erase(begin, end);
                n -= count; // Decrease n by the number of moved chunks
                if (outgoing->second.empty()) {
                    outgoingChunks.erase(outgoing); // Remove the entry if empty
                }
            }
        }
        return to_return;
    }

    bool noMoreChunks(vector<StreamIdentifier> const &sids) {
        lock_guard<std::mutex> lock(outgoingChunksMutex);
        for (auto sid : sids)
        {
            auto outgoing = outgoingChunks.find(sid);
            if (outgoing != outgoingChunks.end()) {
                return false;
            }
        }
        return true;
    }

    pair<size_t, vector<StreamIdentifier>> planForNOutgoingChunks(ngtcp2_cid const& dcid, size_t n, Request const & req) {
        size_t size = 0;
        auto used_identifiers = vector<StreamIdentifier>();
        auto unused_identifiers = set<StreamIdentifier>();
        auto potential_identifiers = list<StreamIdentifier>();
        {
            lock_guard<std::mutex> lock(outgoingChunksMutex);
            for (const auto& pair : outgoingChunks) {
                if (pair.first.cid == dcid) {
                    potential_identifiers.push_back(pair.first);
                }
            }
        }
        {
            lock_guard<std::mutex> lock(returnPathsMutex);
            for (const auto& sid : potential_identifiers) {
                auto it = returnPaths.find(sid);
                if (it != returnPaths.end()) {
                    // Check if the request matches the return path
                    if (it->second == req) {
                        unused_identifiers.insert(sid);
                    }
                }
            }    
        }
        while((n > 0) && (unused_identifiers.size() > 0)) {
            // choose an unused identifier at random:
            auto it = unused_identifiers.begin();
            std::advance(it, rand() % unused_identifiers.size());
            auto sid = *it;
            unused_identifiers.erase(it);
            used_identifiers.push_back(sid);

            {
                lock_guard<std::mutex> lock(outgoingChunksMutex);
                auto outgoing = outgoingChunks.find(sid);
                if (outgoing != outgoingChunks.end()) {
                    for (auto it = outgoing->second.begin(); it != outgoing->second.begin() + std::min(n, outgoing->second.size()); ++it) {
                        auto& chunk = *it;
                        size += chunk.size()+chunk.get_signal_size();
                    }
                    n -= std::min(n, outgoing->second.size());
                }
            }
        }
        return make_pair(size, vector<StreamIdentifier>(used_identifiers.begin(), used_identifiers.end()));
    }

    void pushIncomingChunk(const Request& req, ngtcp2_cid const& scid, shared_span<> &&chunk)
    {
        StreamIdentifier sid(scid, chunk.get_request_id());
        bool handler_ready = !req.isWWATP();  // static handlers are prepared separately and unconditionally
        {
            lock_guard<std::mutex> lock(responderQueueMutex);
            if(std::find_if(responderQueue.begin(), responderQueue.end(),
                [&sid](const auto& pair) { return pair.first == sid; }) != responderQueue.end()) {
                    handler_ready = true;
            }        
        }
        if (!handler_ready)
        {
            prepareHandler(sid, req);
        }

        lock_guard<std::mutex> lock(incomingChunksMutex);
        auto incoming = incomingChunks.find(sid);
        if (incoming == incomingChunks.end()) {
            auto inserted = incomingChunks.insert(make_pair(sid, chunks()));
            inserted.first->second.emplace_back(move(chunk));
        } else {
            incoming->second.emplace_back(move(chunk));
        }
    }
    
    const ServerConfig& getConfig() const { return config; }
    
    /**
     * Get help string for QuicListener configuration
     * @return Help string describing ServerConfig settings
     */
    static std::string getHelpString() {
        return R"(
QuicListener (QUIC Server) Configuration

The QuicListener uses ServerConfig with the following key settings:

Required YAML Configuration:
  private_key_file: Path to server private key file
  cert_file: Path to server certificate file

Common ServerConfig Options:
  port: Server port number (default varies)
  timeout: Idle timeout for QUIC connections (nanoseconds)
  quiet: Suppress non-error output (boolean)
  show_secret: Print transport secrets for debugging (boolean)
  validate_addr: Require address validation (boolean)
  early_response: Start response before full request received (boolean)
  verify_client: Verify client with X.509 certificates (boolean)
  
Flow Control Settings:
  max_data: Initial connection-level flow control window
  max_stream_data_bidi_local: Bidirectional stream window (local)
  max_stream_data_bidi_remote: Bidirectional stream window (remote)  
  max_stream_data_uni: Unidirectional stream window
  max_streams_bidi: Concurrent bidirectional streams
  max_streams_uni: Concurrent unidirectional streams
  
Advanced Options:
  version: QUIC version to use
  groups: Supported cryptographic groups
  cc_algo: Congestion control algorithm
  initial_rtt: Initial RTT estimate
  max_udp_payload_size: Maximum UDP payload size
  handshake_timeout: QUIC handshake timeout
  
Debugging/Logging:
  keylog_filename: Path for TLS key logging
  qlog_dir: Directory for qlog files
  quic_dump: Enable QUIC data hexdump (boolean)
  http_dump: Enable HTTP data hexdump (boolean)
)";
    }

private:
    uint16_t getNextStaticLogicalId() {
        // Increment the static stream ID counter and return the next logical ID
        return static_stream_id_counter.fetch_add(2); // Use odd IDs for static streams
    }

    void terminate() {
        // Set the terminate flag
        terminate_.store(true);

        if (loop == nullptr) {
            return; // If the loop is already destroyed, nothing to do
        }
        // Send the terminate signal
        ev_async_send(loop, &async_terminate);

        // Wait for the server thread to finish
        if (reqrep_thread_.joinable()) {
            reqrep_thread_.join();
        }
        // Clean up the event loop
        ev_async_stop(loop, &async_terminate);
        ev_loop_destroy(loop);
        loop = nullptr;
    }

    std::atomic<uint16_t> static_stream_id_counter = 1; // Start from 1, and use odd IDs for static stream logical Ids

    boost::asio::io_context& io_context;
    struct ev_loop* loop;
    Server* server;
    ev_async async_terminate;
    string private_key_file;
    string cert_file;
    string server_name;
    ServerConfig config;
    boost::asio::ip::tcp::socket socket;
    boost::asio::ip::tcp::endpoint endpoint;
    boost::asio::deadline_timer timer;
    bool timed_out;
    boost::asio::streambuf receive_buffer;

    thread reqrep_thread_;
    std::atomic<bool> terminate_ = false;
    string received_so_far;  // This is a buffer for partially read messages
    bool is_server = false;

    stream_callbacks requestorQueue;

    named_prepare_fns preparersStack;
    stream_callbacks responderQueue;
    stream_return_paths returnPaths;

    stream_data_chunks incomingChunks;
    stream_data_chunks outgoingChunks;

    stream_callbacks streamClosingQueue;

    mutex requestResolverMutex;
    mutex requestorQueueMutex;
    mutex preparerStackMutex;
    mutex responderQueueMutex;
    mutex returnPathsMutex;
    mutex incomingChunksMutex;
    mutex outgoingChunksMutex;
    mutex streamClosingMutex;
};