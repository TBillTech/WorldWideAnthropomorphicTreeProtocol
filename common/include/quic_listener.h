#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <ev.h>

using namespace std;

void sigterminatehandler(struct ev_loop *loop, ev_async *watcher, int revents);

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
    pair<StreamIdentifier, Request> listenForResponseStream() override;
    void setupResponseStream(stream_callback& response) override;
    bool processResponseStream() override;
    void send();
    void receive();
    void check_deadline();
    void listen(const string &local_name, const string& local_ip_addr, int local_port) override;
    void close() override;
    void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) override;

private:
    StreamIdentifier theStreamIdentifier() {
        return StreamIdentifier{false, false, 11, 42};
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
    ev_async async_terminate;
    string private_key_file;
    string cert_file;
    string server_name;
    boost::asio::ip::tcp::socket socket;
    boost::asio::ip::tcp::endpoint endpoint;
    StreamIdentifier currentRequestIdentifier;
    boost::asio::deadline_timer timer;
    bool timed_out;
    boost::asio::streambuf receive_buffer;

    thread reqrep_thread_;
    std::atomic<bool> terminate_ = false;
    string received_so_far;  // This is a buffer for partially read messages
    bool is_server = false;

    request_resolutions requestResolutionQueue;
    stream_callbacks requestorQueue;

    unhandled_response_queue unhandledQueue;
    stream_callbacks responderQueue;

    stream_data_chunks incomingChunks;
    stream_data_chunks outgoingChunks;

    mutex requestResolverMutex;
    mutex requestorQueueMutex;
    mutex unhandledQueueMutex;
    mutex responderQueueMutex;
    mutex incomingChunksMutex;
    mutex outgoingChunksMutex;
};