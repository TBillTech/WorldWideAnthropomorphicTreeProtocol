#include <iostream>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <yaml-cpp/yaml.h>

#include "util.h"
#include "config_base.h"
#include "tcp_communication.h"
#include "quic_connector.h"
#include "quic_listener.h"
#include "shared_chunk.h"

using namespace std;

bool send_trailers = false;

unique_ptr<Communication> createServerCommunication(const string& protocol, boost::asio::io_context& io_context) {
    if (protocol == "QUIC") {
        auto private_key_file = "../test_instances/data/private_key.pem";
        auto cert_file = "../test_instances/data/cert.pem";
        YAML::Node config;
        config["private_key_file"] = private_key_file;
        config["cert_file"] = cert_file;
        config["quiet"] = false;
        config["send_trailers"] = send_trailers;
        config["log_path"] = "../test_instances/sandbox/"; // Ensure sandbox path is set
        return make_unique<QuicListener>(io_context, config);
    } else if (protocol == "TCP") {
        return make_unique<TcpCommunication>(io_context);
    } else {
        throw invalid_argument("Unsupported protocol");
    }
}

unique_ptr<Communication> createClientCommunication(const string& protocol, boost::asio::io_context& io_context) {
    if (protocol == "QUIC") {
        auto private_key_file = "../test_instances/data/private_key.pem";
        auto cert_file = "../test_instances/data/cert.pem";
        YAML::Node config;
        config["private_key_file"] = private_key_file;
        config["cert_file"] = cert_file;
        config["quiet"] = false;
        config["send_trailers"] = send_trailers;
        config["log_path"] = "../test_instances/sandbox/"; // Ensure sandbox path is set
        return make_unique<QuicConnector>(io_context, config);
    } else if (protocol == "TCP") {
        return make_unique<TcpCommunication>(io_context);
    } else {
        throw invalid_argument("Unsupported protocol");
    }
}

int main() {
    // set to pool size for the type UDPChunk to 4 GB
    memory_pool.setPoolSize<UDPChunk>(static_cast<uint64_t>(4) * 1024 * 1024 * 1024 / UDPChunk::chunk_size);

    string protocol = "QUIC";
    //string protocol = "TCP";
    bool zeroRTT_test = true;
    if(protocol == "QUIC")
    {
        auto data_path = realpath("../test_instances/data/", nullptr);
        assert(data_path);
        auto htdocs = std::string(data_path);
        free(data_path);
      
        auto sandbox_path = realpath("../test_instances/sandbox/", nullptr);
        assert(sandbox_path);
        auto keylog_filename = std::string(sandbox_path) + "/keylog";
        auto session_filename = std::string(sandbox_path) + "/session_log";
        auto tp_filename = std::string(sandbox_path) + "/tp_log";
        auto client_data_path = std::string(sandbox_path) + "/data.client";
        free(sandbox_path);

        // Attempt to open the client data file
        auto fd = open(client_data_path.c_str(), O_RDONLY);
        if (fd == -1) {
            std::cerr << "data: Could not open file " << client_data_path << ": "
                      << strerror(errno) << std::endl;
        } else {
            struct stat st;
            if (fstat(fd, &st) != 0) {
                std::cerr << "data: Could not stat file " << client_data_path << ": "
                          << strerror(errno) << std::endl;
                close(fd);
            } else {
                auto datalen = st.st_size;
                if (datalen) {
                    auto addr = mmap(nullptr, datalen, PROT_READ, MAP_SHARED, fd, 0);
                    if (addr == MAP_FAILED) {
                        std::cerr << "data: Could not mmap file " << client_data_path << ": "
                                  << strerror(errno) << std::endl;
                        close(fd);
                    } else {
                        auto data = static_cast<uint8_t *>(addr);
                        memset(data, 0, datalen);
                    }
                }
            }
        }
    
    }
    boost::asio::io_context io_context;

    auto server_communication = createServerCommunication(protocol, io_context);
    server_communication->listen("localhost", "127.0.0.1", 12345);

    this_thread::sleep_for(chrono::seconds(2));
    cout << "In Main: Server should be started up" << endl;

    struct {
        bool client_sent_data = false;
        bool client_sent_greeting = false;
        bool client_sent_heartbeat = true;
        bool server_sent_data = false;
    } send_states;
    auto theServerHandler = [&send_states](const StreamIdentifier& stream_id, chunks& request) {
        chunks response_chunks;
        if (request.empty() && !send_states.server_sent_data) {
            cout << "Server is idle, so sending data" << endl;
            chunks response_chunks;
            // fill 10 response_chunks with 1200 - sizeof(payload_chunk_header) bytes of data initialized to index of data
            uint8_t data_block[1200];
            for (int i = 0; i < 1200; i++) {
                data_block[i] = 'a' + i % 26;
            }
            size_t data_len = 1200 - sizeof(payload_chunk_header);
            for (int i = 0; i < 10; i++) {
                response_chunks.emplace_back(payload_chunk_header(stream_id.logical_id, payload_chunk_header::SIGNAL_WWATP_RESPONSE_CONTINUE, data_len), span<const uint8_t>(data_block, data_len));
            }
            send_states.server_sent_data = true;
            return move(response_chunks);
        }
        for (auto& chunk : request) {
            if (chunk.size() < 100) {
                if (chunk.get_signal<payload_chunk_header>().signal == payload_chunk_header::SIGNAL_HEARTBEAT) {
                    cout << "Server got heartbeat in callback" << endl;
                    continue;
                }
                string chunk_str(chunk.begin<const char>(), chunk.end<const char>());
                cout << "Server got request in callback: " << chunk_str << endl;
                if (chunk_str == "Hello Server!") {
                    static string goodbye_str = "Goodbye Client!";
                    response_chunks.emplace_back(payload_chunk_header(stream_id.logical_id, payload_chunk_header::SIGNAL_WWATP_RESPONSE_CONTINUE, goodbye_str.size()), span<const char>(goodbye_str.c_str(), goodbye_str.size()));
                }
            } else {
                uint8_t data_block[1200];
                for (int i = 0; i < 1200; i++) {
                    data_block[i] = 'a' + i % 26;
                }
                size_t data_len = 1200 - sizeof(payload_chunk_header);
                // Verify that the chunk contains the expected data block
                if (chunk.size() != data_len) {
                    cout << "Server got request in callback: chunk size mismatch" << endl;
                } else {
                    auto data_ptr = chunk.begin<uint8_t>();
                    for (size_t i = 0; i < data_len; i++) {
                        if (*data_ptr != data_block[i]) {
                            cout << "Server got request in callback: data mismatch at index " << i << endl;
                            break;
                        }
                        data_ptr++;
                    }
                    cout << "Server got request in callback: data matches" << endl;
                }
            }
        }
        return move(response_chunks);
    };
    // Add a named_prepare_fn for theServerHandler to the server_communication
    prepare_stream_callback_fn theServerHandlerWrapper = [&theServerHandler](const Request &req) {
        uri_response_info response_info = {true, true, false, 0};
        return std::make_pair(response_info, theServerHandler);
    };
    server_communication->registerRequestHandler(make_pair("test", theServerHandlerWrapper));

    this_thread::sleep_for(chrono::microseconds(100));
    cout << "In Main: Server should have registered request handler" << endl;

    if ((zeroRTT_test) && (protocol == "QUIC"))
    {
        auto prior_client_communication = createClientCommunication(protocol, io_context);
        prior_client_communication->connect("localhost", "127.0.0.1", 12345);
        this_thread::sleep_for(chrono::milliseconds(100));
        // Note: this is a test of early data and reconnection:  Somewhere in the log above
        // this shutting down line will be see QUIC handshake and "Client early data was rejected by server"
        cout << "In Main: Shutting down prior client" << endl;
        // On  the other hand, After this line, the early data rejected message will not appear, because the
        // session_file is being used to re-initialize the connection.
    }

    auto client_communication = createClientCommunication(protocol, io_context);
    client_communication->connect("localhost", "127.0.0.1", 12345);

    // Add a delay to ensure the server has time to start
    this_thread::sleep_for(chrono::milliseconds(100));
    cout << "In Main: Client should be started up" << endl;

    auto theRequest = Request{.scheme = "https", .authority = "localhost", .path = "/wwatp/", .method = "POST", .pri = {0, 0}};
    auto theClientHandler = [&send_states](const StreamIdentifier& stream_id, chunks &response) {
        if (response.empty() && !send_states.client_sent_data) {
            cout << "Client is idle, so sending data" << endl;
            chunks response_chunks;
            // fill 10 response_chunks with 1200 - sizeof(payload_chunk_header) bytes of data initialized to index of data
            uint8_t data_block[1200];
            for (int i = 0; i < 1200; i++) {
                data_block[i] = 'a' + i % 26;
            }
            size_t data_len = 1200 - sizeof(payload_chunk_header);
            for (int i = 0; i < 10; i++) {
                response_chunks.emplace_back(payload_chunk_header(stream_id.logical_id, payload_chunk_header::SIGNAL_WWATP_REQUEST_CONTINUE, data_len), span<const uint8_t>(data_block, data_len));
            }
            send_states.client_sent_data = true;
            return move(response_chunks);
        }
        if (response.empty() && !send_states.client_sent_greeting) {
            cout << "Client is idle, so sending Hello Server! message" << endl;
            static string hello_str = "Hello Server!";
            chunks response_chunks;
            response_chunks.emplace_back(payload_chunk_header(stream_id.logical_id, payload_chunk_header::SIGNAL_WWATP_REQUEST_CONTINUE, hello_str.size()), span<const char>(hello_str.c_str(), hello_str.size()));
            send_states.client_sent_greeting = true;
            return move(response_chunks);
        }
        if (response.empty() && !send_states.client_sent_heartbeat) {
            cout << "Client is idle, and clientState.second is false, so send heartbeat" << endl;
            chunks response_chunks;
            auto tag = payload_chunk_header(stream_id.logical_id, payload_chunk_header::SIGNAL_HEARTBEAT, 0);
            response_chunks.emplace_back(tag, span<const char>("", 0));
            send_states.client_sent_heartbeat = true;
            return move(response_chunks);
        }
        for (auto& chunk : response) {
            if (chunk.size() < 100) {
                string chunk_str(chunk.begin<const char>(), chunk.end<const char>());
                cout << "Client got response received in callback: " << chunk_str << endl;
            } else {
                uint8_t data_block[1200];
                for (int i = 0; i < 1200; i++) {
                    data_block[i] = 'a' + i % 26;
                }
                size_t data_len = 1200 - sizeof(payload_chunk_header);
                // Verify that the chunk contains the expected data block
                if (chunk.size() != data_len) {
                    cout << "Client got response received in callback: chunk size mismatch" << endl;
                } else {
                    auto data_ptr = chunk.begin<uint8_t>();
                    for (size_t i = 0; i < data_len; i++) {
                        if (*data_ptr != data_block[i]) {
                            cout << "Client got response received in callback: data mismatch at index " << i << endl;
                            break;
                        }
                        data_ptr++;
                    }
                    cout << "Client got response received in callback: data matches" << endl;
                }
            }        
        }
        chunks no_chunks;
        return move(no_chunks);
    };
    StreamIdentifier assigned_stream_id = client_communication->getNewRequestStreamIdentifier(theRequest);
    client_communication->registerResponseHandler(assigned_stream_id, theClientHandler);

    int wait_loops = 100;
    if (const char* env_p = std::getenv("DEBUG")) {
        if (std::string(env_p) == "1") {
            wait_loops = 100;
        }
    }

    // Service the client communication for a while to allow the client to send the request.
    for(int i = 0; i < wait_loops; i++) {
        client_communication->processRequestStream();
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    cout << "In Main: Client should have sent request and initialized callback" << endl;

    // Service the server communication for a while to allow the server to send the response.
    for(int i = 0; i < wait_loops; i++) {
        server_communication->processResponseStream();
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    cout << "In Main: Server should have prepared response" << endl;

    // Send the heartbeat packet.
    send_states.client_sent_heartbeat = false;
    for(int i = 0; i < wait_loops; i++) {
        client_communication->processRequestStream();
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    cout << "In Main: Client should have sent hearbeat (with same callback), yeilding response from server" << endl;

    cout << "In Main: Client should have processed response" << endl;
    client_communication->close();
    server_communication->close();
    cout << "In Main: Client and server should be closed" << endl;

    return 0;
}