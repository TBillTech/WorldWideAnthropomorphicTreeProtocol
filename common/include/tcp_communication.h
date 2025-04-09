#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>

using namespace std;

class TcpCommunication : public Communication {
public:
    TcpCommunication(boost::asio::io_context& io_context)
        : io_context(io_context), send_socket(io_context), receive_socket(io_context), timer(io_context) {
    };
    ~TcpCommunication() override {
        terminate();
    }
    StreamIdentifier getNewRequestStreamIdentifier(Request const &req) override;
    void registerResponseHandler(StreamIdentifier sid, stream_callback_fn cb) override;
    void deregisterResponseHandler(StreamIdentifier sid) override;
    bool processRequestStream() override;
    void registerRequestHandler(named_prepare_fn preparer) override;
    void deregisterRequestHandler(string preparer_name) override;
    bool processResponseStream() override;
    void setupRequests();
    void setupResponses();
    void send();
    void receive();
    void check_deadline();
    void listen(const string &local_name, const string& local_ip_addr, int local_port) override;
    void close() override;
    void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) override;

private:
    StreamIdentifier theStreamIdentifier() {
        ngtcp2_cid id;
        id.datalen = 2;
        id.data[0] = 0;
        id.data[1] = 11;
        return StreamIdentifier(id, 42ul);
    }

    void terminate() {
        // Set the terminate flag
        terminate_ = true;

        // Wait for the server thread to finish
        if (send_thread_.joinable()) {
            send_thread_.join();
        }
        // Wait for the server thread to finish
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
    }

    boost::asio::io_context& io_context;
    string server_name;
    boost::asio::ip::tcp::socket send_socket;
    boost::asio::ip::tcp::socket receive_socket;
    boost::asio::ip::tcp::endpoint receive_endpoint;
    boost::asio::ip::tcp::endpoint send_endpoint;
    boost::asio::deadline_timer timer;
    bool timed_out;
    boost::asio::streambuf receive_buffer;

    thread receive_thread_;
    thread send_thread_;
    bool terminate_ = false;
    string received_so_far;  // This is a buffer for partially read messages
    bool is_server = false;
    bool prepared_unhandled_response = false;

    stream_callbacks requestorQueue;

    named_prepare_fns preparersStack;
    stream_callbacks responderQueue;
    stream_return_paths returnPaths;

    stream_data_chunks incomingChunks;
    stream_data_chunks outgoingChunks;

    stream_callbacks streamClosingQueue;

    mutex requestorQueueMutex;
    mutex preparerStackMutex;
    mutex responderQueueMutex;
    mutex incomingChunksMutex;
    mutex outgoingChunksMutex;
    mutex streamClosingMutex;
};