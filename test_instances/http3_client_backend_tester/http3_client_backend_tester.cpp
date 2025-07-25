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
        YAML::Node config;
        config["private_key_file"] = private_key_file;
        config["cert_file"] = cert_file;
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
        return make_unique<QuicConnector>(io_context, config);
    } else if (protocol == "TCP") {
        return make_unique<TcpCommunication>(io_context);
    } else {
        throw invalid_argument("Unsupported protocol");
    }
}

vector<shared_span<>> readFileChunks(string const& file_path) {
    vector<shared_span<>> chunks;
    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        cerr << "Error opening file: " << file_path << endl;
        return chunks;
    }
    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0) {
        cerr << "Error getting file size: " << file_path << endl;
        close(fd);
        return chunks;
    }
    size_t file_size = file_stat.st_size;
    size_t chunk_size = shared_span<>::chunk_size;
    size_t chunk_payload_size = chunk_size - sizeof(payload_chunk_header);
    size_t num_chunks = (file_size + chunk_payload_size - 1) / chunk_payload_size; // Round up to the nearest chunk size
    for (size_t i = 0; i < num_chunks; ++i) {
        size_t offset = i * chunk_payload_size;
        size_t size_to_read = min(chunk_payload_size, file_size - offset);
        if (size_to_read == 0) {
            break; // No more data to read
        }
        // use pread, and create a span<const uint8_t> from the data read
        char memory[shared_span<>::chunk_size];
        ssize_t bytes_read = pread(fd, memory, size_to_read, offset);
        if (bytes_read < 0) {
            cerr << "Error reading file: " << file_path << endl;
            close(fd);
            return chunks;
        }
        auto memory_span = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(memory), bytes_read);
        payload_chunk_header header(0, payload_chunk_header::SIGNAL_OTHER_CHUNK, bytes_read);
        chunks.emplace_back(header, memory_span);
    }
    close(fd);
    return chunks;
}

void verifyStaticData(TreeNode const& withContents, chunks const& originalStaticData, bool allow_trailers) {
    auto property_data = withContents.getPropertyData();
    // We cannot directly compare property_data chunks with originalStaticData chunks, since the chunk lengths may differ.
    // So use the shared_span flattening to convert the originalStaticData to a shared_span,
    // and then use a uint8_t shared_span<> iterator to compare the individual bytes.
    shared_span<> originalSpan(originalStaticData.begin(), originalStaticData.end());
    auto withContentsIterator = property_data.begin<uint8_t>();
    auto originalSpanIterator = originalSpan.begin<uint8_t>();
    size_t index = 0;
    if (withContentsIterator == property_data.end<uint8_t>() && originalSpanIterator != originalSpan.end<uint8_t>()) {
        throw runtime_error("Static data content mismatch: original span has more data than withContents up to index " + to_string(index));
    }
    if (originalSpanIterator == originalSpan.end<uint8_t>() && withContentsIterator != property_data.end<uint8_t>()) {
        throw runtime_error("Static data content mismatch: withContents has more data than original span up to index " + to_string(index));
    }
    while (withContentsIterator != property_data.end<uint8_t>() && originalSpanIterator != originalSpan.end<uint8_t>()) {
        if (*withContentsIterator != *originalSpanIterator) {
            throw runtime_error("Static data content mismatch at index " + to_string(index));
        }
        ++withContentsIterator;
        ++originalSpanIterator;
        ++index;
        if (withContentsIterator == property_data.end<uint8_t>() && originalSpanIterator != originalSpan.end<uint8_t>()) {
            throw runtime_error("Static data content mismatch: original span has more data than withContents up to index " + to_string(index));
        }
        if (allow_trailers && originalSpanIterator == originalSpan.end<uint8_t>())
        {
            // If trailers are allowed, we can stop here, since the original span may have trailing data.
            break;
        }
        if (originalSpanIterator == originalSpan.end<uint8_t>() && withContentsIterator != property_data.end<uint8_t>()) {
            throw runtime_error("Static data content mismatch: withContents has more data than original span up to index " + to_string(index));
        }
    }
}

void test_static_with_curl(Request const& req, const uint16_t port, const string temporary_file_path,
    const string expected_file_path, const string certificates_path)
{
    // This function creates a system call to curl to fetch the static file
    // and compares the result with the expected file.  It waits 2 seconds for the curl command to complete.
    //string curl_command = "curl -s -o " + temporary_file_path + " http://localhost:" + to_string(port) + req.path;
    // curl command also needs to be provided with the --http3 option and the certificates
    // files in the certificates_path are cert.pem and private_key.pem
    string curl_command = "curl -v -s --http3 --insecure --cert " + certificates_path + "/cert.pem --key " + certificates_path + "/private_key.pem -o " + temporary_file_path +
        " https://127.0.0.1:" + to_string(port) + req.path;
    // example: curl -v -s --http3 --insecure --cert ../test_instances/data//cert.pem --key ../test_instances/data//private_key.pem -o ../test_instances/sandbox/temporary_index.html https://127.0.0.1:12345/index.html
    cout << "Executing curl command: " << curl_command << endl; 
    int result = system(curl_command.c_str());
    if (result != 0) {
        cerr << "Curl command failed with error code: " << result << endl;
        throw runtime_error("Curl command failed"); 
    }
    this_thread::sleep_for(chrono::seconds(2)); // Wait for curl to finish
    ifstream expected_file(expected_file_path, ios::binary);
    if (!expected_file) {
        cerr << "Expected file not found: " << expected_file_path << endl;
        throw runtime_error("Expected file not found"); 
    }
    ifstream temporary_file(temporary_file_path, ios::binary);
    if (!temporary_file) {
        cerr << "Temporary file not found: " << temporary_file_path << endl;
        throw runtime_error("Temporary file not found");
    }
    vector<uint8_t> expected_data((istreambuf_iterator<char>(expected_file)), istreambuf_iterator<char>());
    vector<uint8_t> temporary_data((istreambuf_iterator<char>(temporary_file)), istreambuf_iterator<char>());
    if (expected_data.size() != temporary_data.size()) {
        cerr << "File sizes do not match: expected " << expected_data.size() << ", got " << temporary_data.size() << endl;
        throw runtime_error("File sizes do not match");
    }
    for (size_t i = 0; i < expected_data.size(); ++i) {
        if (expected_data[i] != temporary_data[i]) {
            cerr << "File content mismatch at byte " << i << ": expected " << static_cast<int>(expected_data[i])
                 << ", got " << static_cast<int>(temporary_data[i]) << endl;
            throw runtime_error("File content mismatch");
        }
    }
    // cleanup temporary file
    if (remove(temporary_file_path.c_str()) != 0) {
        cerr << "Error deleting temporary file: " << temporary_file_path << endl;
        throw runtime_error("Error deleting temporary file");
    }
    cout << "Static file test passed for " << req.path << endl;
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
            .send_trailers = false,
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
    HTTP3Server theServer("theServer", {});
    prepare_stream_callback_fn theServerHandlerWrapper = [&theServer](const Request &req) {
        return theServer.getResponseCallback(req);
    };
    server_communication->registerRequestHandler(make_pair("test", theServerHandlerWrapper));

    auto init_memory_tree = make_shared<MemoryTree>();
    SimpleBackend initialized_backend(init_memory_tree);
    {
        BackendTestbed initialized(initialized_backend);
        initialized.addAnimalsToBackend();
        initialized.addNotesPageTree();
    }
    theServer.addBackendRoute(initialized_backend, 1000, "/init/wwatp/");
    auto uninitialized_memory_tree = make_shared<MemoryTree>();
    SimpleBackend uninitialized_backend(uninitialized_memory_tree);
    theServer.addBackendRoute(uninitialized_backend, 1000, "/uninit/wwatp/");
    auto blocking_test_memory_tree = make_shared<MemoryTree>();
    SimpleBackend blocking_test_backend(blocking_test_memory_tree);
    theServer.addBackendRoute(blocking_test_backend, 1000, "/blocking/wwatp/");
    auto index_chunks = readFileChunks("../test_instances/data/libtest_index.html");
    theServer.addStaticAsset("/index.html", index_chunks);
    auto random_chunks = readFileChunks("../test_instances/data/randombytes.bin");
    theServer.addStaticAsset("/randombytes.bin", random_chunks);

    this_thread::sleep_for(chrono::microseconds(100));
    timesecs += 0.1;
    cout << "In Main: Server should have registered request handler" << endl;

    auto client_communication = createClientCommunication(protocol, io_context);
    client_communication->connect("localhost", "127.0.0.1", 12345);

    // Add a delay to ensure the server has time to start
    this_thread::sleep_for(chrono::milliseconds(100));
    timesecs += 0.1;
    cout << "In Main: Client should be started up" << endl;

    Request staticHtmlRequest{.scheme = "https", .authority = "localhost", .path = "/index.html", .method = "GET", .pri = {0, 0}};
    Request staticBlockingHtmlRequest{.scheme = "https", .authority = "localhost", .path = "/index.html", .method = "GET", .pri = {1, 0}};
    TreeNodeVersion aVersion;
    TreeNode staticHtmlNode(
        "index.html",
        "Static HTML file",
        {{"html", "index"}},
        aVersion,
        {},
        shared_span<>(global_no_chunk_header, false),
        fplus::nothing<std::string>(),
        fplus::nothing<std::string>()
    );
    Request randomBytesRequest{.scheme = "https", .authority = "localhost", .path = "/randombytes.bin", .method = "GET", .pri = {0, 0}};
    TreeNode randomBytesNode(
        "randombytes.bin",
        "Random bytes file",
        {{"bin", "randombytes"}},
        aVersion,
        {},
        shared_span<>(global_no_chunk_header, false),
        fplus::nothing<std::string>(),
        fplus::nothing<std::string>()
    );
    auto static_tree = make_shared<MemoryTree>();
    SimpleBackend static_backend(static_tree);

    Request theReaderRequest{.scheme = "https", .authority = "localhost", .path = "/init/wwatp/", .method = "POST", .pri = {0, 0}};
    auto local_reader_tree = make_shared<MemoryTree>();
    SimpleBackend local_reader_backend(local_reader_tree);

    Request theWriterRequest{.scheme = "https", .authority = "localhost", .path = "/uninit/wwatp/", .method = "POST", .pri = {0, 0}};
    auto local_writer_tree = make_shared<MemoryTree>();
    SimpleBackend local_writer_backend(local_writer_tree);
    auto reader_of_writer_tree = make_shared<MemoryTree>();
    SimpleBackend reader_of_writer_backend(reader_of_writer_tree);

    Request theBlockingRequest{.scheme = "https", .authority = "localhost", .path = "/blocking/wwatp/", .method = "POST", .pri = {0, 0}};
    auto local_blocking_tree = make_shared<MemoryTree>();
    SimpleBackend local_blocking_backend(local_blocking_tree);
    ThreadsafeBackend local_blocking_backend_threadsafe(local_blocking_backend);

    Http3ClientBackendUpdater client_backend_updater;
    Http3ClientBackend& static_html_client = client_backend_updater.addBackend(static_backend, false, staticHtmlRequest, 0, fplus::just(staticHtmlNode));
    Http3ClientBackend& random_bytes_client = client_backend_updater.addBackend(static_backend, false, randomBytesRequest, 0, fplus::just(randomBytesNode));
    Http3ClientBackend& static_blocking_html_client = client_backend_updater.addBackend(static_backend, true, staticBlockingHtmlRequest, 0, fplus::just(staticHtmlNode));

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

    auto response_cycle = [&client_backend_updater, &client_communication, &server_communication, &wait_loops, &timesecs](string label = "") {
        // Service the client communication for a while to allow the client to send the request.
        cerr << "In Main: Waiting for " << label << endl << flush;
        for(int i = 0; i < wait_loops; i++) {
            client_backend_updater.maintainRequestHandlers(*client_communication, timesecs);
            client_communication->processRequestStream();
            server_communication->processResponseStream();
            this_thread::sleep_for(chrono::milliseconds(20));
            timesecs += 0.02;
        }
        cerr << "In Main: Waited for " << label << endl << flush;
    };

    response_cycle("static requests and reader_tester requestFullTreeSync");
    TreeNode const& readStaticHtmlNode = static_html_client.getStaticNode().get_or_throw(runtime_error("Failed to get static HTML node"));
    verifyStaticData(readStaticHtmlNode, index_chunks, config.send_trailers);
    TreeNode const& readRandomBytesNode = random_bytes_client.getStaticNode().get_or_throw(runtime_error("Failed to get random bytes node"));
    verifyStaticData(readRandomBytesNode, random_chunks, config.send_trailers);
    {
        BackendTestbed reader_tester(reader_client, false, false);
        reader_tester.testBackendLogically();
    }
    {
        BackendTestbed writer_tester(writer_client, false, true);
        writer_tester.addAnimalsToBackend();
        writer_tester.addNotesPageTree();
    }
    response_cycle("writer_tester add animals and notes");
    {
        BackendTestbed writer_tester(writer_client, false, true);
        writer_tester.testBackendLogically();
    }
    response_cycle("writer_tester testBackendLogically");
    reader_of_writer_client.requestFullTreeSync();
    response_cycle("reader_of_writer_client requestFullTreeSync");
    {
        BackendTestbed reader_tester(reader_of_writer_client, false, false);
        reader_tester.testAnimalNodesNoElephant();
    }

    client_backend_updater.run(*client_communication, timesecs, 100);
    theServer.start(*server_communication, 0, 100);

    {
        TreeNode const& readBlockingHtmlNode = static_blocking_html_client.getStaticNode().get_or_throw(runtime_error("Failed to get blocking HTML node"));
        verifyStaticData(readBlockingHtmlNode, index_chunks, config.send_trailers);
        test_static_with_curl(staticBlockingHtmlRequest, 12345, "../test_instances/sandbox/temporary_index.html",
            "../test_instances/data/libtest_index.html", "../test_instances/data/");
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