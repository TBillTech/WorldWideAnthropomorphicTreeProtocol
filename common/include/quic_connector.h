#pragma once
#include "communication.h"

#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <ev.h>

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
    bool processRequestStream() override;
    void registerRequestHandler(named_prepare_fn preparer) override;
    void deregisterRequestHandler(string preparer_name) override;
    bool processResponseStream() override;
    void check_deadline();
    void listen(const string &local_name, const string& local_ip_addr, int local_port) override;
    void close() override;
    void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) override;

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

    mutex requestResolverMutex;
    mutex requestorQueueMutex;
    mutex preparerStackMutex;
    mutex responderQueueMutex;
    mutex incomingChunksMutex;
    mutex outgoingChunksMutex;
};