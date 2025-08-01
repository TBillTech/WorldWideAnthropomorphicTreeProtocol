#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <cstring>
#include <csignal>
#include <atomic>

#include "wwatp_service.h"
#include "shared_chunk.h"

using namespace std;

// Global flag for signal handling
atomic<bool> running{true};

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        cout << "\nReceived shutdown signal, stopping server..." << endl;
        running = false;
    }
}

void printUsage(const char* program_name) {
    cout << "Usage: " << program_name << " [OPTIONS] <config_yaml_path>" << endl;
    cout << endl;
    cout << "WWATP Server - World Wide Anthropomorphic Tree Protocol Server" << endl;
    cout << endl;
    cout << "Arguments:" << endl;
    cout << "  config_yaml_path    Path to YAML configuration file (required)" << endl;
    cout << endl;
    cout << "Options:" << endl;
    cout << "  -h, --help         Show this help message and exit" << endl;
    cout << "  --usage            Show usage information and exit" << endl;
    cout << "  --pool-size SIZE   Set memory pool size in GB (default: 1)" << endl;
    cout << "  --check-only       Validate configuration without starting server" << endl;
    cout << endl;
    cout << "Examples:" << endl;
    cout << "  " << program_name << " config.yaml" << endl;
    cout << "  " << program_name << " --pool-size 2 config.yaml" << endl;
    cout << "  " << program_name << " --check-only config.yaml" << endl;
}

int main(int argc, char* argv[]) {
    string config_path;
    uint64_t pool_size_gb = 1;
    bool check_only = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg == "--usage") {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg == "--pool-size") {
            if (i + 1 >= argc) {
                cerr << "Error: --pool-size requires a value" << endl;
                return 1;
            }
            try {
                pool_size_gb = stoull(argv[++i]);
                if (pool_size_gb == 0) {
                    cerr << "Error: Pool size must be greater than 0" << endl;
                    return 1;
                }
            } catch (const exception& e) {
                cerr << "Error: Invalid pool size value: " << argv[i] << endl;
                return 1;
            }
        }
        else if (arg == "--check-only") {
            check_only = true;
        }
        else if (arg.starts_with("--")) {
            cerr << "Error: Unknown option: " << arg << endl;
            printUsage(argv[0]);
            return 1;
        }
        else {
            if (!config_path.empty()) {
                cerr << "Error: Multiple config paths specified" << endl;
                printUsage(argv[0]);
                return 1;
            }
            config_path = arg;
        }
    }
    
    // Validate required arguments
    if (config_path.empty()) {
        cerr << "Error: Configuration file path is required" << endl;
        printUsage(argv[0]);
        return 1;
    }

    try {
        // Initialize memory pool for UDPChunk
        uint64_t pool_size_bytes = pool_size_gb * 1024 * 1024 * 1024;
        memory_pool.setPoolSize<UDPChunk>(pool_size_bytes / UDPChunk::chunk_size);

        cout << "WWATP Server v1.0" << endl;
        cout << "Config file: " << config_path << endl;
        cout << "Memory pool size: " << pool_size_gb << " GB" << endl;

        // Create WWATPService with config file path
        auto service = make_unique<WWATPService>(config_path);
        service->initialize();

        cout << "Service initialized successfully" << endl;

        if (check_only) {
            cout << "Configuration validation completed successfully" << endl;
            return 0;
        }

        // Set up signal handlers
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);

        cout << "Starting WWATP server..." << endl;
        cout << "Press Ctrl+C to stop the server" << endl;

        // Run the service in a loop using responseCycle
        double time_secs = 0.0;
        while (running) {
            try {
                service->responseCycle("Server running", 10, 100, time_secs);
            } catch (const exception& e) {
                cerr << "Error in service cycle: " << e.what() << endl;
                break;
            }
        }

        // Stop the service gracefully
        service->stop();

    } catch (const exception& e) {
        cerr << "Server error: " << e.what() << endl;
        return 1;
    }

    cout << "Server shutdown complete" << endl;
    return 0;
}