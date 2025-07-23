#include <iostream>
#include <memory>
#include <string>
#include <map>
#include <boost/asio.hpp>

#include "util.h"
#include "config_base.h"
#include "quic_listener.h"
#include "shared_chunk.h"
#include "simple_backend.h"
#include "http3_server.h"
#include "memory_tree.h"

using namespace std;

int main() {
    cout << "WWATP Server Stub v0.1" << endl;
    cout << "Starting minimal server with no features..." << endl;

    try {
        // Initialize memory pool for UDPChunk to 1 GB (smaller than test setup)
        memory_pool.setPoolSize<UDPChunk>(static_cast<uint64_t>(1) * 1024 * 1024 * 1024 / UDPChunk::chunk_size);

        // Create IO context
        boost::asio::io_context io_context;

        // Create simple backend for tree storage
        auto memoryTree = make_shared<MemoryTree>();
        auto backend = make_shared<SimpleBackend>(memoryTree);

        // Create HTTP3 server frontend with empty static assets
        auto server = make_unique<HTTP3Server>("wwatp_main_server", map<string, chunks>{});
        
        // Add backend route for WWATP protocol
        server->addBackendRoute(*backend, 1000, "/wwatp/");

        // Create QUIC listener for network communication
        auto private_key_file = "../test_instances/data/private_key.pem";
        auto cert_file = "../test_instances/data/cert.pem";
        auto listener = make_unique<QuicListener>(io_context, private_key_file, cert_file);

        // Register server handler with the listener
        prepare_stream_callback_fn serverHandler = [&server](const Request &req) {
            return server->getResponseCallback(req);
        };
        listener->registerRequestHandler(make_pair("wwatp_server", serverHandler));

        cout << "Server initialized successfully" << endl;
        cout << "Server is running in stub mode - no additional features enabled" << endl;
        cout << "WWATP backend route available at /wwatp/" << endl;
        cout << "Press Ctrl+C to stop the server" << endl;

        // Run the server
        io_context.run();

    } catch (const exception& e) {
        cerr << "Server error: " << e.what() << endl;
        return 1;
    }

    cout << "Server shutdown complete" << endl;
    return 0;
}