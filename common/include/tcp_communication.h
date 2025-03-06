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
        : io_context(io_context), socket(io_context), timer(io_context) {
    };
    ~TcpCommunication() override {
        terminate();
    }
    RequestIdentifier getNextRequestIdentifier() override;
    void requestSend(const requestor_callback& request) override;
    void processNextResponse() override;
    identified_request requestReceive() override;
    void responseSend(const identified_request& request, const string& response) override;
    void send();
    void receive();
    void check_deadline();
    void listen(const string &local_name, const string& local_ip_addr, int local_port) override;
    void close() override;
    void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) override;

private:
    void terminate() {
        // Set the terminate flag
        terminate_ = true;

        // Wait for the server thread to finish
        if (reqrep_thread_.joinable()) {
            reqrep_thread_.join();
        }
    }

    boost::asio::io_context& io_context;
    string server_name;
    boost::asio::ip::tcp::socket socket;
    boost::asio::ip::tcp::endpoint endpoint;
    RequestIdentifier currentRequestIdentifier;
    boost::asio::deadline_timer timer;
    bool timed_out;
    boost::asio::streambuf receive_buffer;

    thread reqrep_thread_;
    bool terminate_ = false;
    string received_so_far;  // This is a buffer for partially read messages
    bool is_server = false;

    requestor_callback_vector newRequestQueue;
    requestor_callback_vector requestSentQueue;
    completed_callback_vector responseReceiveQueue;
    identified_request_vector requestReceiveQueue;
    identified_response_vector newResponseQueue;
    mutex requestIdentifierLock;
    mutex newRequestQueueMutex;
    mutex requestSentQueueMutex;
    mutex responseReceiveQueueMutex;
    mutex requestReceiveQueueMutex;
    mutex newResponseQueueMutex;
};