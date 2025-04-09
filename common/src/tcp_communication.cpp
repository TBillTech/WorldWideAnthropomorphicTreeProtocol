#include "tcp_communication.h"

StreamIdentifier TcpCommunication::getNewRequestStreamIdentifier(Request const &req) {
    // This is just a test, so we don't need to do anything with the cid.
    // The request is just a placeholder for now.
    return theStreamIdentifier();    
}

void TcpCommunication::registerResponseHandler(StreamIdentifier sid, stream_callback_fn cb) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    requestorQueue.push_back(std::make_pair(sid, cb));
}

void TcpCommunication::deregisterResponseHandler(StreamIdentifier sid) {
    lock_guard<std::mutex> lock(requestorQueueMutex);
    auto it = std::remove_if(requestorQueue.begin(), requestorQueue.end(),
        [&sid](const stream_callback& stream_cb) {
            return stream_cb.first == sid;
        });
    requestorQueue.erase(it, requestorQueue.end());
}

bool TcpCommunication::processRequestStream() {
    std::unique_lock<std::mutex> requestorLock(requestorQueueMutex);
    if (!requestorQueue.empty()) {
        stream_callback stream_cb = requestorQueue.front();
        requestorQueue.erase(requestorQueue.begin());
        requestorLock.unlock();
        requestorLock.lock();
        requestorLock.unlock();
        StreamIdentifier stream_id = stream_cb.first;
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        stream_callback_fn fn = stream_cb.second;
        chunks to_process;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_id);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        chunks processed = fn(stream_cb.first, to_process);
        if(!processed.empty())
        {   
            lock_guard<std::mutex> lock(outgoingChunksMutex);
            auto outgoing = outgoingChunks.find(stream_cb.first);
            if (outgoing == outgoingChunks.end()) {
                auto inserted = outgoingChunks.insert(make_pair(stream_cb.first, chunks()));
                inserted.first->second.swap(processed);
            }
            else {
                int chunk_count = 0;
                for (auto &chunk : processed) {
                    outgoing->second.emplace_back(chunk);
                }
            }
        }
        requestorLock.lock();
        requestorQueue.push_back(stream_cb);
    } else {
        return false;
    }
    return true;
}

void TcpCommunication::registerRequestHandler(named_prepare_fn preparer)
{
    lock_guard<std::mutex> lock(preparerStackMutex);
    preparersStack.push_back(preparer);
}

void TcpCommunication::deregisterRequestHandler(string preparer_name)
{
    lock_guard<std::mutex> lock(preparerStackMutex);
    auto it = std::remove_if(preparersStack.begin(), preparersStack.end(),
        [&preparer_name](const named_prepare_fn& preparer) {
            return preparer.first == preparer_name;
        });
    preparersStack.erase(it, preparersStack.end());
}

bool TcpCommunication::processResponseStream() {
    std::unique_lock<std::mutex> responderLock(responderQueueMutex);
    if (!responderQueue.empty()) {
        stream_callback stream_cb = responderQueue.front();
        responderQueue.erase(responderQueue.begin());
        responderLock.unlock();
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        stream_callback_fn fn = stream_cb.second;
        chunks to_process;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_cb.first);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        chunks processed = fn(stream_cb.first, to_process);
        if(!processed.empty())
        {   
            lock_guard<std::mutex> lock(outgoingChunksMutex);
            auto outgoing = outgoingChunks.find(stream_cb.first);
            if (outgoing == outgoingChunks.end()) {
                auto inserted = outgoingChunks.insert(make_pair(stream_cb.first, chunks()));
                inserted.first->second.swap(processed);
            }
            else {
                for (auto &chunk : processed) {
                    outgoing->second.emplace_back(chunk);
                }
            }
        }
        responderLock.lock();
        responderQueue.push_back(stream_cb);
    } else {
        return false;
    }
    return true;
}

void TcpCommunication::setupResponses() {
    // For the TCP protocol, this is just testing code, and so there is no real negotiation.
    // So, we need to put something on the unhandledQueue exactly once.
    if (prepared_unhandled_response) {
        return;
    }
    prepared_unhandled_response = true;
    const std::string_view uri = "wwatp://localhost/test";
    Request req = Request{.scheme = "https", .authority = "localhost", .path = "/test"};
    lock_guard<std::mutex> lock(preparerStackMutex);
    for (auto &preparer : preparersStack) {
        auto response = preparer.second(req);
        auto response_info = response.first;
        if (response_info.can_handle) {
            // Also, since this is just testing code, the various flags are not used.
            lock_guard<std::mutex> lock(responderQueueMutex);
            responderQueue.push_back(make_pair(theStreamIdentifier(), response.second));
            break;
        }
    }
}

void TcpCommunication::send() {
    // lock and pop the first request from the newResponseQueue
    outgoingChunksMutex.lock();
    if (!outgoingChunks.empty()) {
        chunks outgoing;
        auto outgoing_pair = *(outgoingChunks.begin());
        outgoing.swap(outgoing_pair.second);
        StreamIdentifier outgoing_id = outgoing_pair.first;
        outgoingChunks.erase(outgoingChunks.begin());
        outgoingChunksMutex.unlock();
        for (auto &chunk : outgoing) {
            // send the response (this is hard coded to use the one socket rather than the identified_request info 
            // because this TcpCommunication is only used for testing and architecture illustration)
            string response_str(chunk.begin<const char>(), chunk.end<const char>());
            boost::system::error_code ec;
            boost::asio::write(send_socket, boost::asio::buffer(response_str + "\n"), ec);
            if (ec) {
                std::cerr << "Error sending response: " << ec.message() << std::endl;
            } else {
                if (is_server) {
                    std::cout << "Server sent response chunk: " << outgoing_id << " ... " << response_str << std::endl;
                } else {
                    std::cout << "Client sent response chunk: " << outgoing_id << " ... " << response_str << std::endl;
                }
            }
        }
    } else {
        outgoingChunksMutex.unlock();
    }
}

void TcpCommunication::receive() {
    boost::system::error_code ec;
    std::size_t bytes_transferred = receive_socket.read_some(boost::asio::buffer(receive_buffer.prepare(1024)), ec);
    if (!ec) {
        receive_buffer.commit(bytes_transferred);
        std::istream is(&receive_buffer);
        std::string data;
        std::getline(is, data);

        if (!data.empty()) {
            std::lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(theStreamIdentifier());
            if (incoming == incomingChunks.end()) {
                auto incoming_pair = incomingChunks.insert(make_pair(theStreamIdentifier(), chunks()));
                incoming = incoming_pair.first;
            }
            incoming->second.emplace_back(global_no_signal, span<const char>(data.c_str(), data.size()));
            std::cerr << "Received data: " << data << std::endl << flush;
        } else {
            std::cerr << "Received empty data" << std::endl;
        }
    } else {
        if (ec == boost::asio::error::operation_aborted) {
            std::cerr << "Receive operation aborted" << std::endl;
        } else if (ec == boost::asio::error::eof) {
            std::cerr << "Connection closed by peer" << std::endl;
        } else {
            std::cerr << "Error in receive: " << ec.message() << std::endl;
        }
    }
}

void TcpCommunication::check_deadline() {
    if (timer.expires_at() <= boost::asio::deadline_timer::traits_type::now()) {
        timed_out = true;
        receive_socket.cancel();
        timer.expires_at(boost::posix_time::pos_infin);
    }
    timer.async_wait([this](const boost::system::error_code&) { check_deadline(); });
}

void TcpCommunication::listen(const string &local_name, const string& local_ip_addr, int local_port) {
    is_server = true;
    receive_thread_ = thread([this, local_name, local_ip_addr, local_port]() {
        try {
            // Start listening for incoming connections
            cout << "Server starting on " << local_ip_addr << ":" << local_port << endl;
            server_name = local_name;
            receive_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(local_ip_addr), local_port);    
            send_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(local_ip_addr), local_port+1);
            receive_socket = boost::asio::ip::tcp::socket(io_context);
        
            boost::asio::ip::tcp::acceptor receive_acceptor(io_context, receive_endpoint);
            receive_acceptor.accept(receive_socket);
            cout << "Server started and connected on " << local_ip_addr << ":" << local_port << "," << local_port+1 << endl;
        } catch (const std::exception& e) {
            cerr << "Error starting server: " << e.what() << endl;
            return (-1);
        }
        while (!terminate_) {
            io_context.poll();
            this->receive();
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        return 0;
    });
    send_thread_ = thread([this, local_name, local_ip_addr, local_port]() {
        try {
            // Start listening for incoming connections
            cout << "Server starting on " << local_ip_addr << ":" << local_port << endl;
            server_name = local_name;
            receive_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(local_ip_addr), local_port);    
            send_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(local_ip_addr), local_port+1);
            send_socket = boost::asio::ip::tcp::socket(io_context);
        
            boost::asio::ip::tcp::acceptor send_acceptor(io_context, send_endpoint);
            send_acceptor.accept(send_socket);
            cout << "Server started and connected on " << local_ip_addr << ":" << local_port << "," << local_port+1 << endl;
        } catch (const std::exception& e) {
            cerr << "Error starting server: " << e.what() << endl;
            return (-1);
        }
        while (!terminate_) {
            io_context.poll();
            this->setupResponses();
            this->send();
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        return 0;
    });
}

void TcpCommunication::close() {
    send_socket.close();
    receive_socket.close();
}

void TcpCommunication::connect(const string &peer_name, const string& peer_ip_addr, int peer_port) {
    receive_thread_ = thread([this, peer_name, peer_ip_addr, peer_port]() {

        if (peer_name == "localhost" || peer_ip_addr == "127.0.0.1") {
            receive_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), peer_port+1);
            send_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), peer_port);
        } else {
            receive_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(peer_ip_addr), peer_port+1);
            send_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(peer_ip_addr), peer_port);
        }

        boost::asio::ip::tcp::resolver resolver(io_context);
        boost::asio::ip::tcp::resolver::results_type receive_endpoints = resolver.resolve(peer_ip_addr, std::to_string(peer_port+1));
        boost::asio::ip::tcp::resolver::results_type send_endpoints = resolver.resolve(peer_ip_addr, std::to_string(peer_port));
        boost::asio::connect(receive_socket, receive_endpoints);
        cout << "Client connected to " << peer_ip_addr << ":" << peer_port+1 << "," << peer_port << endl;
        while (!terminate_) {
            io_context.poll();
            this->receive();
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        return 0;
    });

    send_thread_ = thread([this, peer_name, peer_ip_addr, peer_port]() {

        if (peer_name == "localhost" || peer_ip_addr == "127.0.0.1") {
            receive_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), peer_port+1);
            send_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), peer_port);
        } else {
            receive_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(peer_ip_addr), peer_port+1);
            send_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(peer_ip_addr), peer_port);
        }

        boost::asio::ip::tcp::resolver resolver(io_context);
        boost::asio::ip::tcp::resolver::results_type receive_endpoints = resolver.resolve(peer_ip_addr, std::to_string(peer_port+1));
        boost::asio::ip::tcp::resolver::results_type send_endpoints = resolver.resolve(peer_ip_addr, std::to_string(peer_port));
        boost::asio::connect(send_socket, send_endpoints);
        cout << "Client connected to " << peer_ip_addr << ":" << peer_port+1 << "," << peer_port << endl;
        while (!terminate_) {
            io_context.poll();
            this->send();
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        return 0;
    });

}