#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <ev.h>
#include <algorithm>

using namespace std;

// Yes, it is fair to say that the Client object in the client-side code performs duties similar to how Handler objects work on the server side. Here are the key points that illustrate this:

// Connection Management:

// The Client object manages the overall QUIC connection, similar to how a Handler object manages a connection on the server side.
// It handles the initialization, reading, writing, and closing of the connection.

// ClientStream Management:

// The Client object manages multiple ClientStream objects, one for each active stream within the connection, similar to how a Handler object manages multiple ClientStream objects on the server side.
// It handles the creation of new streams, sending and receiving data on streams, and closing streams.

// Callbacks and Event Handling:

// The Client object sets up various callbacks for handling QUIC events, such as receiving stream data, acknowledging stream data, and handling stream closures.
// These callbacks are similar to the methods in the Handler class that handle stream-related events on the server side.

void sigterminatehandler(struct ev_loop *loop, ev_async *watcher, int revents);

class Client;

class QuicConnector : public Communication {
public:
    QuicConnector(boost::asio::io_context& io_context, string private_key_file, string cert_file)
        : io_context(io_context), private_key_file(private_key_file), cert_file(cert_file),
          socket(io_context), timer(io_context) {
        loop = ev_loop_new(EVFLAG_AUTO);
        ev_async_init(&async_terminate, sigterminatehandler);
        ev_async_start(loop, &async_terminate);
    };
    ~QuicConnector() override {
        terminate();
    }
    void resolveRequestStream(Request const &req, stream_callback_fn cb) override;
    Request setupRequest(StreamIdentifier sid);
    bool processRequestStream() override;
    void registerRequestHandler(named_prepare_fn preparer) override;
    void deregisterRequestHandler(string preparer_name) override;
    bool processResponseStream() override;
    void check_deadline();
    void listen(const string &local_name, const string& local_ip_addr, int local_port) override;
    void close() override;
    void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) override;
    size_t requestCount() {
        lock_guard<std::mutex> lock(requestResolverMutex);
        return requestResolutionQueue.size();
    }

    void finishRequest(StreamIdentifier const& sid, uint64_t app_error_code) {
        auto it = std::find_if(requestorQueue.begin(), requestorQueue.end(), 
                       [&sid](const auto& pair) { return pair.first == sid; });
        if (it != requestorQueue.end()) {
            requestorQueue.erase(it);
        }
    }
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
    chunks popNOutgoingChunks(StreamIdentifier const& sid, size_t n) {
        lock_guard<std::mutex> lock(outgoingChunksMutex);
        auto outgoing = outgoingChunks.find(sid);
        chunks to_return;

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
        }
        return move(to_return);
    }
    size_t sizeOfNOutgoingChunks(StreamIdentifier const& sid, size_t n) {
        lock_guard<std::mutex> lock(outgoingChunksMutex);
        auto outgoing = outgoingChunks.find(sid);
        size_t size = 0;
        if (outgoing != outgoingChunks.end()) {
            for (auto it = outgoing->second.begin(); it != outgoing->second.begin() + std::min(n, outgoing->second.size()); ++it) {
                auto& chunk = *it;
                size += chunk.size()+chunk.get_signal_size();
            }
        }
        return size;
    }
    void pushIncomingChunk(StreamIdentifier const& sid, std::span<uint8_t> const &chunk)
    {
        lock_guard<std::mutex> lock(incomingChunksMutex);
        auto incoming = incomingChunks.find(sid);
        if (incoming == incomingChunks.end()) {
            auto inserted = incomingChunks.insert(make_pair(sid, chunks()));
            inserted.first->second.emplace_back(chunk);
        } else {
            incoming->second.emplace_back(chunk);
        }
    }

private:
    StreamIdentifier theStreamIdentifier() {
        return StreamIdentifier({11}, 42);
    }

    void terminate() {
        // Set the terminate flag
        terminate_.store(true);

        // Send the terminate signal
        ev_async_send(loop, &async_terminate);

        // Wait for the server thread to finish
        if (reqrep_thread_.joinable()) {
            reqrep_thread_.join();
        }
    }

    boost::asio::io_context& io_context;
    struct ev_loop* loop;
    Client* client;
    ev_async async_terminate;
    string private_key_file;
    string cert_file;
    string server_name;
    boost::asio::ip::tcp::socket socket;
    boost::asio::ip::tcp::endpoint endpoint;
    boost::asio::deadline_timer timer;
    bool timed_out;
    boost::asio::streambuf receive_buffer;

    thread reqrep_thread_;
    std::atomic<bool> terminate_ = false;
    string received_so_far;  // This is a buffer for partially read messages
    bool is_server = false;

    request_resolutions requestResolutionQueue;
    stream_callbacks requestorQueue;

    named_prepare_fns preparersStack;
    stream_callbacks responderQueue;

    stream_data_chunks incomingChunks;
    stream_data_chunks outgoingChunks;

    stream_callbacks streamClosingQueue;

    mutex requestResolverMutex;
    mutex requestorQueueMutex;
    mutex preparerStackMutex;
    mutex responderQueueMutex;
    mutex incomingChunksMutex;
    mutex outgoingChunksMutex;
    mutex streamClosingMutex;
};