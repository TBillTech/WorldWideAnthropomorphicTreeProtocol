#include "tcp_communication.h"

void TcpCommunication::resolveRequestStream(Request const &req, stream_callback_fn cb) {
    // This just returns the currentRequestIdentifier
    // Increment the currentRequestIdentifier
    lock_guard<std::mutex> lock(requestResolverMutex);
    requestResolutionQueue.push_back(make_pair(req, cb));
}

bool TcpCommunication::processRequestStream() {
    requestorQueueMutex.lock();
    if (!requestorQueue.empty()) {
        stream_callback stream_cb = requestorQueue.front();
        requestorQueue.erase(requestorQueue.begin());
        requestorQueueMutex.unlock();
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        auto fn = stream_cb.second;
        vector<std::span<const uint8_t>> to_process;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_cb.first);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        auto processed = fn(stream_cb.first, to_process);
        if(!processed.empty())
        {   
            lock_guard<std::mutex> lock(outgoingChunksMutex);
            auto outgoing = outgoingChunks.find(stream_cb.first);
            if (outgoing != outgoingChunks.end()) {
                outgoing->second.insert(outgoing->second.end(), processed.begin(), processed.end());
            } else {
                outgoingChunks.insert(make_pair(stream_cb.first, processed));
            }
        }
        lock_guard<std::mutex> lock(requestorQueueMutex);
        requestorQueue.push_back(stream_cb);
    } else {
        requestorQueueMutex.unlock();
        return false;
    }
    return true;
}

pair<StreamIdentifier, Request> TcpCommunication::listenForResponseStream(){
    // This pops the request from the requestReceiveQueue, and returns it
    lock_guard<std::mutex> lock(unhandledQueueMutex);
    if (!unhandledQueue.empty()) {
        pair<StreamIdentifier, Request> request = unhandledQueue.front();
        unhandledQueue.erase(unhandledQueue.begin());
        return request;
    } else {
        return IDLE_stream_listener;
    }
}

void TcpCommunication::setupResponseStream(stream_callback& response)
{
    // This just adds the response to the responderQueue
    lock_guard<std::mutex> lock(responderQueueMutex);
    responderQueue.push_back(response);
}

bool TcpCommunication::processResponseStream() {
    responderQueueMutex.lock();
    if (!responderQueue.empty()) {
        stream_callback stream_cb = responderQueue.front();
        responderQueue.erase(responderQueue.begin());
        responderQueueMutex.unlock();
        // Now the stream callback is not on the queue, so no other worker will try to service it while this is operational.
        // Call the callback function OUTSIDE the lock
        auto fn = stream_cb.second;
        vector<std::span<const uint8_t>> to_process;
        {
            lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(stream_cb.first);
            if (incoming != incomingChunks.end()) {
                to_process.swap(incoming->second);
            }
        }
        auto processed = fn(stream_cb.first, to_process);
        if(!processed.empty())
        {   
            lock_guard<std::mutex> lock(outgoingChunksMutex);
            auto outgoing = outgoingChunks.find(stream_cb.first);
            if (outgoing != outgoingChunks.end()) {
                outgoing->second.insert(outgoing->second.end(), processed.begin(), processed.end());
            } else {
                outgoingChunks.insert(make_pair(stream_cb.first, processed));
            }
        }
        lock_guard<std::mutex> lock(responderQueueMutex);
        responderQueue.push_back(stream_cb);
    } else {
        responderQueueMutex.unlock();
        return false;
    }
    return true;
}

void TcpCommunication::setupRequests() {
    // For the TCP protocol, this is just testing code, and so there is no real negotiation.
    // So, if there a thing on the requestResolutionQueue, then pop it and add it to the requestorQueue.
    RequestCallback request_cb;
    {
        lock_guard<std::mutex> lock(requestResolverMutex);
        if (!requestResolutionQueue.empty()) {
            request_cb = requestResolutionQueue.front();
            requestResolutionQueue.erase(requestResolutionQueue.begin());
        } else {
            return;
        }
    }

    stream_callback request_stream = std::make_pair(theStreamIdentifier(), request_cb.second);
    lock_guard<std::mutex> lock(requestorQueueMutex);
    requestorQueue.push_back(request_stream);
}

void TcpCommunication::setupResponses() {
    // For the TCP protocol, this is just testing code, and so there is no real negotiation.
    // So, we need to put something on the unhandledQueue exactly once.
    if (prepared_unhandled_response) {
        return;
    }
    prepared_unhandled_response = true;
    Request request = {"http", "localhost", "/test", {0, 0}};
    pair<StreamIdentifier, Request> unhandled = make_pair(theStreamIdentifier(), request);
    lock_guard<std::mutex> lock(unhandledQueueMutex);
    std::cerr << "setupResponses has created one unhandled" << std::endl << std::flush;
    unhandledQueue.push_back(unhandled);
}

void TcpCommunication::send() {
    // lock and pop the first request from the newResponseQueue
    outgoingChunksMutex.lock();
    if (!outgoingChunks.empty()) {
        pair<StreamIdentifier, chunks> outgoing = *(outgoingChunks.begin());
        outgoingChunks.erase(outgoingChunks.begin());
        outgoingChunksMutex.unlock();
        for (auto &chunk : outgoing.second) {
            // send the response (this is hard coded to use the one socket rather than the identified_request info 
            // because this TcpCommunication is only used for testing and architecture illustration)
            auto response_str = std::string(chunk.begin(), chunk.end());
            boost::system::error_code ec;
            boost::asio::write(send_socket, boost::asio::buffer(response_str + "\n"), ec);
            if (ec) {
                std::cerr << "Error sending response: " << ec.message() << std::endl;
            } else {
                if (is_server) {
                    std::cout << "Server sent response chunk: " << outgoing.first << " ... " << response_str << std::endl;
                } else {
                    std::cout << "Client sent response chunk: " << outgoing.first << " ... " << response_str << std::endl;
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
            // TODO:  Create a datatype to manage chunk memory safely
            auto data_ptr = new std::string(data);
            std::lock_guard<std::mutex> lock(incomingChunksMutex);
            auto incoming = incomingChunks.find(theStreamIdentifier());
            if (incoming != incomingChunks.end()) {
                incoming->second.push_back(std::span<const uint8_t>((uint8_t*)data_ptr->c_str(), data_ptr->size()));
            } else {
                incomingChunks.insert(make_pair(theStreamIdentifier(), vector<std::span<const uint8_t>>{std::span<const uint8_t>((uint8_t*)data_ptr->c_str(), data_ptr->size())}));
            }
            std::cerr << "Received data: " << data << std::endl;
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
            this->setupRequests();
            this->send();
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        return 0;
    });

}