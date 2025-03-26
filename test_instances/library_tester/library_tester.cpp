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

    //string protocol = "QUIC";
    string protocol = "TCP";
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
            .session_file = move(session_filename),
            .tp_file = move(tp_filename),
            .keylog_filename = move(keylog_filename),
            .http_method = "GET"sv,        
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

    this_thread::sleep_for(chrono::seconds(1));
    cout << "In Main: Server should be started up" << endl;

    if (protocol == "QUIC")
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

    auto theRequest = Request{.scheme = "wwatp://", .authority = "localhost", .path = "/test"};
    bool sentRequest = false;
    auto theClientHandler = [&sentRequest](const StreamIdentifier& stream_id, chunks &response) {
        if (response.empty() && !sentRequest) {
            cout << "Client is idle, so sending Hello Server! message" << endl;
            static string hello_str = "Hello Server!";
            chunks response_chunks;
            response_chunks.emplace_back(span<const char>(hello_str.c_str(), hello_str.size()));
            sentRequest = true;
            return move(response_chunks);
        }
        for (auto& chunk : response) {
            string chunk_str(chunk.begin<const char>(), chunk.end<const char>());
            cout << "Client got response received in callback: " << chunk_str << endl;
        }
        chunks no_chunks;
        return move(no_chunks);
    };
    client_communication->resolveRequestStream(theRequest, theClientHandler);

    int wait_loops = 100;
    if (const char* env_p = std::getenv("DEBUG")) {
        if (std::string(env_p) == "1") {
            wait_loops = 100;
        }
    }

    // Service the client communication for a while to allow the client to send the request.
    for(int i = 0; i < wait_loops; i++) {
        client_communication->processRequestStream();
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    cout << "In Main: Client should have sent request and initialized callback" << endl;

    auto received_request = server_communication->listenForResponseStream();
    uint64_t interval_count = 0;
    uint64_t give_up_seconds = 5;
    auto interval_length = chrono::milliseconds(100);
    while(received_request == IDLE_stream_listener) {
        this_thread::sleep_for(interval_length);
        received_request = server_communication->listenForResponseStream();
        interval_count++;
        if(interval_count >= chrono::seconds(give_up_seconds)/interval_length) {
            cout << "In Main: Waited " << interval_count*interval_length << " for request, giving up." << endl;
            break;
        }
    }
    if(received_request != IDLE_stream_listener) {
        cout << "In Main: Server request received: " << received_request.second << endl;
        auto theServerHandler = [](const StreamIdentifier& stream_id, chunks& request) {
            for (auto& chunk : request) {
                string chunk_str(chunk.begin<const char>(), chunk.end<const char>());
                cout << "Server got request in callback: " << chunk_str << endl;
            }
            if (!request.empty()) {
                static string goodbye_str = "Goodbye Client!";
                chunks response_chunks;
                response_chunks.emplace_back(span<const char>(goodbye_str.c_str(), goodbye_str.size()));
                return move(response_chunks);
            }
            chunks no_chunks;
            return move(no_chunks);
        };
        stream_callback response_stream = std::make_pair(received_request.first, theServerHandler);
        server_communication->setupResponseStream(response_stream);
    }
     
    // Service the server communication for a while to allow the server to send the response.
    for(int i = 0; i < wait_loops; i++) {
        server_communication->processResponseStream();
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    cout << "In Main: Server should have sent response" << endl;

    // Service the client communication for a while to allow the client to process the response.
    for(int i = 0; i < wait_loops; i++) {
        client_communication->processRequestStream();
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    cout << "In Main: Client should have processed response" << endl;
    client_communication->close();
    server_communication->close();
    cout << "In Main: Client and server should be closed" << endl;

    return 0;
}