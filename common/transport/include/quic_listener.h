#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <ev.h>
#include <set>
#include <list>

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
            cout << "Preparing handler for request: " << req << " at stream: " << sid << endl;
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

private:
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