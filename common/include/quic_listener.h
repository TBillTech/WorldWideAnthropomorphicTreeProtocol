#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <ev.h>

using namespace std;

void sigterminatehandler(struct ev_loop *loop, ev_async *watcher, int revents);

class Server;

class QuicListener : public Communication {
public:
    QuicListener(boost::asio::io_context& io_context, string private_key_file, string cert_file)
        : io_context(io_context), private_key_file(private_key_file), cert_file(cert_file),
          socket(io_context), timer(io_context) {
        loop = ev_loop_new(EVFLAG_AUTO);
        ev_async_init(&async_terminate, sigterminatehandler);
        ev_async_start(loop, &async_terminate);
    };
    ~QuicListener() override {
        terminate();
    }
    void resolveRequestStream(Request const &req, stream_callback_fn cb) override;
    bool processRequestStream() override;
    void registerRequestHandler(named_prepare_fn preparer) override;
    void deregisterRequestHandler(string preparer_name) override;
    uri_response_info prepareHandler(StreamIdentifier stream_id, const string_view& uri);
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
    Server* server;
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