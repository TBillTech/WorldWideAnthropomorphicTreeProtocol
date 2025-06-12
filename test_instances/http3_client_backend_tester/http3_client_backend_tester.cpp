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
#include "http3_server.h"
#include "http3_client_backend.h"
#include "backend_testbed.h"
#include "simple_backend.h"
#include "memory_tree.h"

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
    disableCatch2();
    // set to pool size for the type UDPChunk to 4 GB
    memory_pool.setPoolSize<UDPChunk>(static_cast<uint64_t>(4) * 1024 * 1024 * 1024 / UDPChunk::chunk_size);

    double timesecs = 1.0;

    string protocol = "QUIC";
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
        config = Config{
            .tx_loss_prob = 0.,
            .rx_loss_prob = 0.,
            .fd = -1,
            .ciphers = ngtcp2::util::crypto_default_ciphers(),
            .groups = ngtcp2::util::crypto_default_groups(),
            .htdocs = move(htdocs),
            .mime_types_file = "/etc/mime.types",
            .version = NGTCP2_PROTO_VER_V1,
            .quiet = true,
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
#pragma GCC diagnostic pop    
    }
    boost::asio::io_context io_context;

    auto server_communication = createServerCommunication(protocol, io_context);
    server_communication->listen("localhost", "127.0.0.1", 12345);

    this_thread::sleep_for(chrono::seconds(2));
    timesecs += 2.0;
    cout << "In Main: Server should be started up" << endl;

    // Add a named_prepare_fn for theServerHandler to the server_communication
    HTTP3Server theServer({});
    prepare_stream_callback_fn theServerHandlerWrapper = [&theServer](const Request &req) {
        return theServer.getResponseCallback(req);
    };
    server_communication->registerRequestHandler(make_pair("test", theServerHandlerWrapper));

    MemoryTree init_memory_tree;
    SimpleBackend initialized_backend(init_memory_tree);
    {
        BackendTestbed initialized(initialized_backend);
        initialized.addAnimalsToBackend();
        initialized.addNotesPageTree();
    }
    theServer.addBackendRoute(initialized_backend, 1000, "/init/wwatp/");
    MemoryTree uninitialized_memory_tree;
    SimpleBackend uninitialized_backend(uninitialized_memory_tree);
    theServer.addBackendRoute(uninitialized_backend, 1000, "/uninit/wwatp/");
    MemoryTree blocking_test_memory_tree;
    SimpleBackend blocking_test_backend(blocking_test_memory_tree);
    theServer.addBackendRoute(blocking_test_backend, 1000, "/blocking/wwatp/");

    this_thread::sleep_for(chrono::microseconds(100));
    timesecs += 0.1;
    cout << "In Main: Server should have registered request handler" << endl;

    auto client_communication = createClientCommunication(protocol, io_context);
    client_communication->connect("localhost", "127.0.0.1", 12345);

    // Add a delay to ensure the server has time to start
    this_thread::sleep_for(chrono::milliseconds(100));
    timesecs += 0.1;
    cout << "In Main: Client should be started up" << endl;

    auto theReaderRequest = Request{.scheme = "https", .authority = "localhost", .path = "/init/wwatp/", .pri = {0, 0}};
    MemoryTree local_reader_tree;
    SimpleBackend local_reader_backend(local_reader_tree);

    auto theWriterRequest = Request{.scheme = "https", .authority = "localhost", .path = "/uninit/wwatp/", .pri = {0, 0}};
    MemoryTree local_writer_tree;
    SimpleBackend local_writer_backend(local_writer_tree);
    MemoryTree reader_of_writer_tree;
    SimpleBackend reader_of_writer_backend(reader_of_writer_tree);

    auto theBlockingRequest = Request{.scheme = "https", .authority = "localhost", .path = "/blocking/wwatp/", .pri = {0, 0}};
    MemoryTree local_blocking_tree;
    SimpleBackend local_blocking_backend(local_blocking_tree);
    ThreadsafeBackend local_blocking_backend_threadsafe(local_blocking_backend);

    Http3ClientBackendUpdater client_backend_updater;
    Http3ClientBackend& reader_client = client_backend_updater.addBackend(local_reader_backend, false, theReaderRequest);
    reader_client.requestFullTreeSync();
    
    Http3ClientBackend& writer_client = client_backend_updater.addBackend(local_writer_backend, false, theWriterRequest);
    Http3ClientBackend& reader_of_writer_client = client_backend_updater.addBackend(reader_of_writer_backend, false, theWriterRequest);
    
    Http3ClientBackend& blocking_client = client_backend_updater.addBackend(local_blocking_backend_threadsafe, true, theBlockingRequest, 60);

    int wait_loops = 100;
    if (const char* env_p = std::getenv("DEBUG")) {
        if (std::string(env_p) == "1") {
            wait_loops = 100;
        }
    }

    auto response_cycle = [&client_backend_updater, &client_communication, &server_communication, &wait_loops, &timesecs]() {
        // Service the client communication for a while to allow the client to send the request.
        for(int i = 0; i < wait_loops; i++) {
            client_backend_updater.maintainRequestHandlers(*client_communication, timesecs);
            client_communication->processRequestStream();
            server_communication->processResponseStream();
            this_thread::sleep_for(chrono::milliseconds(20));
            timesecs += 0.02;
        }
    };

    response_cycle();
    {
        BackendTestbed reader_tester(reader_client, false, false);
        reader_tester.testBackendLogically();
    }
    {
        BackendTestbed writer_tester(writer_client, false, true);
        writer_tester.addAnimalsToBackend();
        writer_tester.addNotesPageTree();
    }
    response_cycle();
    {
        BackendTestbed writer_tester(writer_client, false, true);
        writer_tester.testBackendLogically();
    }
    response_cycle();
    reader_of_writer_client.requestFullTreeSync();
    response_cycle();
    {
        BackendTestbed reader_tester(reader_of_writer_client, false, false);
        reader_tester.testAnimalNodesNoElephant();
    }

    client_backend_updater.run(*client_communication, timesecs, 100);
    theServer.run(*server_communication, 100);

    {
        blocking_client.requestFullTreeSync();
        BackendTestbed blocking_tester(blocking_client);
        blocking_tester.addAnimalsToBackend();
        blocking_tester.addNotesPageTree();
        blocking_tester.testBackendLogically();
    }
    cout << "In Main:  All tests passed" << endl << flush; 
    client_backend_updater.stop();
    theServer.stop();

    client_communication->close();
    server_communication->close();
    cout << "In Main: Client and server should be closed" << endl;

    return 0;
}