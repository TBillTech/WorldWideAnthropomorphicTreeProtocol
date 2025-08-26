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
#include "wwatp_service.h"

using namespace std;

bool send_trailers = false;

// YAML configuration for WWATPService that matches the current HTTP3 client backend tester implementation
string yaml_config = R"(config:
  child_names: [backends, frontends]
  backends:
    child_names: [
      initialized_simple, uninitialized_simple, blocking_simple,
      local_reader_simple, local_writer_simple, reader_of_writer_simple,
      local_blocking_simple, local_blocking_threadsafe, static_simple,
      static_html_client, random_bytes_client, static_blocking_html_client,
      reader_client, writer_client, reader_of_writer_client, blocking_client
    ]
    initialized_simple:
      type: simple
    uninitialized_simple:
      type: simple
    blocking_simple:
      type: simple
    local_reader_simple:
      type: simple
    local_writer_simple:
      type: simple
    reader_of_writer_simple:
      type: simple
    local_blocking_simple:
      type: simple
    local_blocking_threadsafe:
      type: threadsafe
      backend: local_blocking_simple
    static_simple:
      type: simple
    static_html_client:
      config.yaml:
        type: http3_client
        ip: 127.0.0.1
        port: 12345
        name: localhost
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        log_path: ../test_instances/sandbox/
        path: /index.html
        backend: static_simple
        static_node_label: index.html
        static_node_description: Static HTML file
    random_bytes_client:
      config.yaml:
        type: http3_client
        ip: 127.0.0.1
        port: 12345
        name: localhost
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        log_path: ../test_instances/sandbox/
        path: /randombytes.bin
        backend: static_simple
        static_node_label: randombytes.bin
        static_node_description: Random bytes file
    static_blocking_html_client:
      config.yaml:
        type: http3_client
        ip: 127.0.0.1
        port: 12345
        name: localhost
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        log_path: ../test_instances/sandbox/
        blocking_mode: true
        path: /index.html
        priority: 1
        increment: 0
        backend: static_simple
        static_node_label: index.html
        static_node_description: Static HTML file
    reader_client:
      config.yaml:
        type: http3_client
        ip: 127.0.0.1
        port: 12345
        name: localhost
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        log_path: ../test_instances/sandbox/
        backend: local_reader_simple
        path: /init/wwatp/
    writer_client:
      config.yaml:
        type: http3_client
        ip: 127.0.0.1
        port: 12345
        name: localhost
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        log_path: ../test_instances/sandbox/
        backend: local_writer_simple
        path: /uninit/wwatp/
    reader_of_writer_client:
      config.yaml:
        type: http3_client
        ip: 127.0.0.1
        port: 12345
        name: localhost
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        log_path: ../test_instances/sandbox/
        backend: reader_of_writer_simple
        path: /uninit/wwatp/
        journal_requests_per_minute: 60
    blocking_client:
      config.yaml:
        type: http3_client
        ip: 127.0.0.1
        port: 12345
        name: localhost
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        log_path: ../test_instances/sandbox/
        backend: local_blocking_threadsafe
        blocking_mode: true
        path: /blocking/wwatp/
  frontends:
    child_names: [http3_server, http3_client_updater]
    http3_server:
      config.yaml:
        type: http3_server
        name: localhost
        ip: 127.0.0.1
        port: 12345
        private_key_file: ../test_instances/data/private_key.pem
        cert_file: ../test_instances/data/cert.pem
        quiet: true
        send_trailers: false
        log_path: ../test_instances/sandbox/
        static_load: true
      child_names: [init, uninit, blocking]
      init:
        child_names: [wwatp]
        wwatp:
          backend: initialized_simple
      uninit:
        child_names: [wwatp]
        wwatp:
          backend: uninitialized_simple
      blocking:
        child_names: [wwatp]
        wwatp:
          backend: blocking_simple
      index.html: ../test_instances/data/libtest_index.html
      randombytes.bin: ../test_instances/data/randombytes.bin
)";

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

    TreeNode staticHtmlNode(
        "index.html",
        "Static HTML file",
        {{"html", "index"}},
        DEFAULT_TREE_NODE_VERSION,
        {},
        shared_span<>(global_no_chunk_header, false),
        fplus::nothing<std::string>(),
        fplus::nothing<std::string>()
    );
    TreeNode randomBytesNode(
        "randombytes.bin",
        "Random bytes file",
        {{"bin", "randombytes"}},
        DEFAULT_TREE_NODE_VERSION,
        {},
        shared_span<>(global_no_chunk_header, false),
        fplus::nothing<std::string>(),
        fplus::nothing<std::string>()
    );

    // Create a WWATPService instance
    WWATPService wwatp_service("http3_client_tester", yaml_config);
    
    auto initialized_backend = wwatp_service.getBackend("initialized_simple");
    assert(initialized_backend != nullptr && "Initialized backend should not be null");
    {
        BackendTestbed initialized(*initialized_backend);
        initialized.addAnimalsToBackend();
        initialized.addNotesPageTree();
    }
    auto uninitialized_backend = wwatp_service.getBackend("uninitialized_simple");
    assert(uninitialized_backend != nullptr && "Uninitialized backend should not be null");
    auto blocking_test_backend = wwatp_service.getBackend("blocking_simple");
    assert(blocking_test_backend != nullptr && "Blocking backend should not be null");
    auto index_chunks = readFileChunks("../test_instances/data/libtest_index.html");
    auto random_chunks = readFileChunks("../test_instances/data/randombytes.bin");

    Request staticBlockingHtmlRequest{.scheme = "https", .authority = "localhost", .path = "/index.html", .method = "GET", .pri = {1, 0}};
    auto static_backend = wwatp_service.getBackend("static_simple");
    assert(static_backend != nullptr && "Static backend should not be null");

    auto local_reader_backend = wwatp_service.getBackend("local_reader_simple");
    assert(local_reader_backend != nullptr && "Local reader backend should not be null");

    auto local_writer_backend = wwatp_service.getBackend("local_writer_simple");
    assert(local_writer_backend != nullptr && "Local writer backend should not be null");
    auto reader_of_writer_backend = wwatp_service.getBackend("reader_of_writer_simple");
    assert(reader_of_writer_backend != nullptr && "Reader of writer backend should not be null");

    auto local_blocking_backend = wwatp_service.getBackend("local_blocking_threadsafe");
    assert(local_blocking_backend != nullptr && "Local blocking backend should not be null");
    auto local_blocking_backend_threadsafe = wwatp_service.getBackend("local_blocking_threadsafe");
    assert(local_blocking_backend_threadsafe != nullptr && "Local blocking backend threadsafe should not be null");

    auto static_html_client = dynamic_cast<Http3ClientBackend*>(wwatp_service.getBackend("static_html_client").get());
    assert(static_html_client != nullptr && "Static HTML client should not be null");
    auto random_bytes_client = dynamic_cast<Http3ClientBackend*>(wwatp_service.getBackend("random_bytes_client").get());
    assert(random_bytes_client != nullptr && "Random bytes client should not be null");
    auto static_blocking_html_client = dynamic_cast<Http3ClientBackend*>(wwatp_service.getBackend("static_blocking_html_client").get());
    assert(static_blocking_html_client != nullptr && "Static blocking HTML client should not be null");

    auto reader_client = dynamic_cast<Http3ClientBackend*>(wwatp_service.getBackend("reader_client").get());
    assert(reader_client != nullptr && "Reader client should not be null");
    
    auto writer_client = dynamic_cast<Http3ClientBackend*>(wwatp_service.getBackend("writer_client").get());
    assert(writer_client != nullptr && "Writer client should not be null");
    auto reader_of_writer_client = dynamic_cast<Http3ClientBackend*>(wwatp_service.getBackend("reader_of_writer_client").get());
    assert(reader_of_writer_client != nullptr && "Reader of writer client should not be null");
    auto blocking_client = dynamic_cast<Http3ClientBackend*>(wwatp_service.getBackend("blocking_client").get());
    assert(blocking_client != nullptr && "Blocking client should not be null");

    this_thread::sleep_for(chrono::seconds(2));
    timesecs += 2.0;
    cout << "In Main: Server should be started up" << endl;

    int wait_loops = 100;
    if (const char* env_p = std::getenv("DEBUG")) {
        if (std::string(env_p) == "1") {
            wait_loops = 100;
        }
    }

    reader_client->requestFullTreeSync();

    auto response_cycle = [&wwatp_service, &wait_loops, &timesecs](string label = "") {
        wwatp_service.responseCycle(label, wait_loops, 20, timesecs);
    };

    response_cycle("static requests and reader_tester requestFullTreeSync");
    TreeNode const& readStaticHtmlNode = static_html_client->getStaticNode().get_or_throw(runtime_error("Failed to get static HTML node"));
    verifyStaticData(readStaticHtmlNode, index_chunks, send_trailers);
    TreeNode const& readRandomBytesNode = random_bytes_client->getStaticNode().get_or_throw(runtime_error("Failed to get random bytes node"));
    verifyStaticData(readRandomBytesNode, random_chunks, send_trailers);
    {
        BackendTestbed reader_tester(*reader_client, false, false);
        reader_tester.testBackendLogically();
    }
    {
        BackendTestbed writer_tester(*writer_client, false, true);
        writer_tester.addAnimalsToBackend();
        writer_tester.addNotesPageTree();
    }
    response_cycle("writer_tester add animals and notes");
    {
        BackendTestbed writer_tester(*writer_client, false, true);
        writer_tester.testBackendLogically();
    }
    response_cycle("writer_tester testBackendLogically");
    reader_of_writer_client->requestFullTreeSync();
    response_cycle("reader_of_writer_client requestFullTreeSync");
    {
        BackendTestbed reader_tester(*reader_of_writer_client, false, false);
        reader_tester.testAnimalNodesNoElephant();
    }
    {
        BackendTestbed reader_tester(*reader_of_writer_client, true, true);
        reader_tester.testPeerNotification(*writer_client, [response_cycle]() {
            response_cycle("background_service for testPeerNotification");
        });
    }

    wwatp_service.run();

    {
        TreeNode const& readBlockingHtmlNode = static_blocking_html_client->getStaticNode().get_or_throw(runtime_error("Failed to get blocking HTML node"));
        verifyStaticData(readBlockingHtmlNode, index_chunks, send_trailers);
        test_static_with_curl(staticBlockingHtmlRequest, 12345, "../test_instances/sandbox/temporary_index.html",
            "../test_instances/data/libtest_index.html", "../test_instances/data/");
        blocking_client->requestFullTreeSync();
        BackendTestbed blocking_tester(*blocking_client, false, true);
        blocking_tester.addAnimalsToBackend();
        blocking_tester.addNotesPageTree();
        blocking_tester.testBackendLogically();
    }
    cout << "In Main:  All tests passed" << endl << flush; 

    wwatp_service.stop();
    cout << "In Main: Client and server should be closed" << endl;

    return 0;
}