#include <iostream>
#include <memory>
#include <string>
#include <sys/mman.h>

#include "util.h"
#include "config_base.h"
#include "tcp_communication.h"
#include "quic_connector.h"
#include "quic_listener.h"
#include "shared_chunk.h"

using namespace std;

unique_ptr<Communication> createServerCommunication(const string& protocol, boost::asio::io_context& io_context) {
    if (protocol == "QUIC") {
        auto private_key_file = "../test_instances/data/private_key.pem";
        auto cert_file = "../test_instances/data/cert.pem";
        return make_unique<QuicListener>(io_context, private_key_file, cert_file);
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
        return make_unique<QuicConnector>(io_context, private_key_file, cert_file);
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
    bool zeroRTT_test = false;
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
            config.fd = -1;
            config.datalen = 0;
        } else {
            struct stat st;
            if (fstat(fd, &st) != 0) {
                std::cerr << "data: Could not stat file " << client_data_path << ": "
                          << strerror(errno) << std::endl;
                close(fd);
                config.fd = -1;
                config.datalen = 0;
            } else {
                config.fd = fd;
                config.datalen = st.st_size;
                if (config.datalen) {
                    auto addr = mmap(nullptr, config.datalen, PROT_READ, MAP_SHARED, fd, 0);
                    if (addr == MAP_FAILED) {
                        std::cerr << "data: Could not mmap file " << client_data_path << ": "
                                  << strerror(errno) << std::endl;
                        close(fd);
                        config.fd = -1;
                        config.datalen = 0;
                    } else {
                        config.data = static_cast<uint8_t *>(addr);
                        memset(config.data, 0, config.datalen);
                    }
                }
            }
        }

        // config.fd and config.datalen are for data that should be streamed to the server
        // if and only if the client_data_path has a valid data file.

        config = Config{
            .tx_loss_prob = 0.,
            .rx_loss_prob = 0.,
            .fd = -1,
            .ciphers = ngtcp2::util::crypto_default_ciphers(),
            .groups = ngtcp2::util::crypto_default_groups(),
            .htdocs = move(htdocs),
            .mime_types_file = "/etc/mime.types",
            .version = NGTCP2_PROTO_VER_V1,
            .timeout = 30 * NGTCP2_SECONDS,
            .early_response = false,
            .session_file = move(session_filename),
            .tp_file = move(tp_filename),
            .keylog_filename = move(keylog_filename),
            .http_method = "POST"sv,        
            .max_data = 24_m,
            .max_stream_data_bidi_local = 16_m,
            .max_stream_data_bidi_remote = 256_k,
            .max_stream_data_uni = 256_k,
            .max_streams_bidi = 100,
            .max_streams_uni = 100,
            .max_window = 6_m,
            .max_stream_window = 6_m,
            .max_dyn_length = 20_m,
            .cc_algo = NGTCP2_CC_ALGO_CUBIC,
            .initial_rtt = NGTCP2_DEFAULT_INITIAL_RTT,
            .handshake_timeout = UINT64_MAX,
            .ack_thresh = 2,
            .initial_pkt_num = UINT32_MAX,
          };
    
    }
    boost::asio::io_context io_context;

    auto server_communication = createServerCommunication(protocol, io_context);
    server_communication->listen("localhost", "127.0.0.1", 12345);

    this_thread::sleep_for(chrono::seconds(2));
    cout << "In Main: Server should be started up" << endl;

    auto theServerHandler = [](const StreamIdentifier& stream_id, chunks& request) {
        chunks response_chunks;
        for (auto& chunk : request) {
            if (chunk.get_signal<request_identifier_tag>().signal == request_identifier_tag::SIGNAL_HEARTBEAT) {
                cout << "Server got heartbeat in callback" << endl;
                continue;
            }
            string chunk_str(chunk.begin<const char>(), chunk.end<const char>());
            cout << "Server got request in callback: " << chunk_str << endl;
            if (chunk_str == "Hello Server!") {
                static string goodbye_str = "Goodbye Client!";
                response_chunks.emplace_back(request_identifier_tag(stream_id.logical_id, 0, 0), span<const char>(goodbye_str.c_str(), goodbye_str.size()));
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

    auto theRequest = Request{.scheme = "https", .authority = "localhost", .path = "/test"};
    pair<bool, bool> clientState = make_pair(false, true);
    auto theClientHandler = [&clientState](const StreamIdentifier& stream_id, chunks &response) {
        if (response.empty() && !clientState.first) {
            cout << "Client is idle, so sending Hello Server! message" << endl;
            static string hello_str = "Hello Server!";
            chunks response_chunks;
            response_chunks.emplace_back(request_identifier_tag(stream_id.logical_id, 0, 0), span<const char>(hello_str.c_str(), hello_str.size()));
            clientState.first = true;
            return move(response_chunks);
        }
        if (response.empty() && !clientState.second) {
            cout << "Client is idle, and clientState.second is false, so send heartbeat" << endl;
            chunks response_chunks;
            auto tag = request_identifier_tag(stream_id.logical_id, request_identifier_tag::SIGNAL_HEARTBEAT, 0);
            response_chunks.emplace_back(tag, span<const char>("", 0));
            clientState.second = true;
            return move(response_chunks);
        }
        for (auto& chunk : response) {
            string chunk_str(chunk.begin<const char>(), chunk.end<const char>());
            cout << "Client got response received in callback: " << chunk_str << endl;
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
    clientState.second = false;
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