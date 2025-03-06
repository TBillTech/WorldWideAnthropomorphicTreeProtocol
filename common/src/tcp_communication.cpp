#include "tcp_communication.h"

RequestIdentifier TcpCommunication::getNextRequestIdentifier() {
    // This just returns the currentRequestIdentifier
    // Increment the currentRequestIdentifier
    lock_guard<std::mutex> lock(requestIdentifierLock);
    RequestIdentifier newRequestIdentifier = currentRequestIdentifier;
    currentRequestIdentifier.stream_id += 2;
    return newRequestIdentifier;
}

void TcpCommunication::requestSend(const requestor_callback& request) {
    // This just adds the request to the newRequestQueue
    // Add the request to the requestSentQueue
    lock_guard<std::mutex> lock(newRequestQueueMutex);
    newRequestQueue.push_back(request);
}

void TcpCommunication::processNextResponse() {
    // This pops the response from the responseReceiveQueue and calls the callback function
    // Pop the response from the responseReceiveQueue
    responseReceiveQueueMutex.lock();
    if (!responseReceiveQueue.empty()) {
        completed_callback response = responseReceiveQueue.front();
        responseReceiveQueue.erase(responseReceiveQueue.begin());
        responseReceiveQueueMutex.unlock();
        // Call the callback function OUTSIDE the lock
        auto fn = response.first.second;
        fn(response.first.first, response.second);
    } else {
        responseReceiveQueueMutex.unlock();
    }
}

identified_request TcpCommunication::requestReceive(){
    // This pops the request from the requestReceiveQueue, and returns it
    lock_guard<std::mutex> lock(requestReceiveQueueMutex);
    if (!requestReceiveQueue.empty()) {
        identified_request request = requestReceiveQueue.front();
        requestReceiveQueue.erase(requestReceiveQueue.begin());
        return request;
    } else {
        return IDLE_identified_request;
    }
}

void TcpCommunication::responseSend(const identified_request& request, const string& response) {
    // This just adds the response to the newResponseQueue
    // Add the response to the newResponseQueue
    lock_guard<std::mutex> lock(newResponseQueueMutex);
    newResponseQueue.push_back(make_pair(request, response));
}

void TcpCommunication::send() {
    if (is_server) {
        // lock and pop the first request from the newResponseQueue
        newResponseQueueMutex.lock();
        if (!newResponseQueue.empty()) {
            identified_request request = newResponseQueue.front().first;
            string response = newResponseQueue.front().second;
            newResponseQueue.erase(newResponseQueue.begin());
            newResponseQueueMutex.unlock();
            // send the response (this is hard coded to use the one socket rather than the identified_request info 
            // because this TcpCommunication is only used for testing)
            boost::asio::write(socket, boost::asio::buffer(response + "\n"));
            cout << "Server sent response: " << response << endl;
        } else {
            newResponseQueueMutex.unlock();
        }
    }
    else {
        //lock and pop the first request from the newRequestQueue
        newRequestQueueMutex.lock();
        if (!newRequestQueue.empty()) {
            requestor_callback request = newRequestQueue.front();
            newRequestQueue.erase(newRequestQueue.begin());
            newRequestQueueMutex.unlock();
            // send the request (this is hard coded to use the one socket rather than the identified_request info 
            // because this TcpCommunication is only used for testing)
            boost::asio::write(socket, boost::asio::buffer(request.first.second + "\n"));
            cout << "Client sent request: " << request.first.second << endl;
            // add the request to the requestSentQueue
            requestSentQueueMutex.lock();
            requestSentQueue.push_back(request);
            requestSentQueueMutex.unlock();
        } else {
            newRequestQueueMutex.unlock();
        }
    }
}

void TcpCommunication::receive() {
    socket.async_read_some(boost::asio::buffer(receive_buffer.prepare(1024)),
        [this](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            if (!ec) {
                receive_buffer.commit(bytes_transferred);
                std::istream is(&receive_buffer);
                std::string data;
                std::getline(is, data);

                if (!data.empty()) {
                    if (is_server) {
                        std::lock_guard<std::mutex> lock(requestReceiveQueueMutex);
                        requestReceiveQueue.push_back(std::make_pair(currentRequestIdentifier, data));
                    } else {
                        std::lock_guard<std::mutex> sent_lock(requestSentQueueMutex);
                        if (requestSentQueue.empty()) {
                            return;
                        }
                        requestor_callback currentRequest = requestSentQueue.front();
                        requestSentQueue.erase(requestSentQueue.begin());
                        std::lock_guard<std::mutex> recv_lock(responseReceiveQueueMutex);
                        responseReceiveQueue.push_back(std::make_pair(currentRequest, data));
                    }
                }

                // Continue reading
                receive();
            } else {
                if (ec == boost::asio::error::operation_aborted) {
                    std::cerr << "Receive operation aborted" << std::endl;
                } else if (ec == boost::asio::error::eof) {
                    std::cerr << "Connection closed by peer" << std::endl;
                } else {
                    std::cerr << "Error in receive: " << ec.message() << std::endl;
                }
            }
        });
}

void TcpCommunication::check_deadline() {
    if (timer.expires_at() <= boost::asio::deadline_timer::traits_type::now()) {
        timed_out = true;
        socket.cancel();
        timer.expires_at(boost::posix_time::pos_infin);
    }
    timer.async_wait([this](const boost::system::error_code&) { check_deadline(); });
}

void TcpCommunication::listen(const string &local_name, const string& local_ip_addr, int local_port) {
    is_server = true;
    reqrep_thread_ = thread([this, local_name, local_ip_addr, local_port]() {
        try {
            // Start listening for incoming connections
            cout << "Server starting on " << local_ip_addr << ":" << local_port << endl;
            server_name = local_name;
            endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(local_ip_addr), local_port);    
            socket = boost::asio::ip::tcp::socket(io_context);
        
            boost::asio::ip::tcp::acceptor acceptor(io_context, endpoint);
            acceptor.accept(socket);
            cout << "Server started and connected on " << local_ip_addr << ":" << local_port << endl;
        } catch (const std::exception& e) {
            cerr << "Error starting server: " << e.what() << endl;
            return (-1);
        }
        this->receive();
        while (!terminate_) {
            io_context.poll();
            this->send();
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        return 0;
    });
}

void TcpCommunication::close() {
    socket.close();
}

void TcpCommunication::connect(const string &peer_name, const string& peer_ip_addr, int peer_port) {
    reqrep_thread_ = thread([this, peer_name, peer_ip_addr, peer_port]() {

        if (peer_name == "localhost" || peer_ip_addr == "127.0.0.1") {
            endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), peer_port);
        } else {
            endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(peer_ip_addr), peer_port);
        }

        boost::asio::ip::tcp::resolver resolver(io_context);
        boost::asio::ip::tcp::resolver::results_type endpoints = resolver.resolve(peer_ip_addr, std::to_string(peer_port));
        boost::asio::connect(socket, endpoints);
        this->receive();
        while (!terminate_) {
            io_context.poll();
            this->send();
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        return 0;
    });
}