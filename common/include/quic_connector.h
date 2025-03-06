#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <ev.h>

using namespace std;

void sigterminatehandler(struct ev_loop *loop, ev_async *watcher, int revents);

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
    RequestIdentifier currentRequestIdentifier;
    boost::asio::deadline_timer timer;
    bool timed_out;
    boost::asio::streambuf receive_buffer;

    thread reqrep_thread_;
    std::atomic<bool> terminate_ = false;
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