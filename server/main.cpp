#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <cstring>
#include <csignal>
#include <atomic>
#include <map>
#include <fstream>

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

// Help directory strings for navigation
map<string, string> help_directories = {
    {"top_level_help_directory", R"(
WWATP Server Help System

For detailed configuration help, use:
  --help backends    - Show backend configuration help
  --help frontends   - Show frontend configuration help
  
Available help topics:
  - backends: Information about data storage backends
  - frontends: Information about communication frontends
)"},
    
    {"backends_help_directory", R"(
Backend Configuration Help

To get specific help for individual backend types, use:
  --help simple         - Simple in-memory backend
  --help transactional  - Transactional backend with rollback
  --help threadsafe     - Thread-safe backend wrapper
  --help composite      - Composite backend for mounting
  --help redirected     - Redirected backend wrapper
  --help http3_client   - HTTP3 client backend
  --help file           - File system backend
)"},
    
    {"frontends_help_directory", R"(
Frontend Configuration Help

To get specific help for individual frontend types, use:
  --help cloning_mediator  - Cloning mediator frontend
  --help yaml_mediator     - YAML mediator frontend  
  --help http3_server      - HTTP3 server frontend
)"},
    
    // Individual backend help directories - redirect back to backends_help
    {"simple_help_directory", "For backend configuration help, use: --help backends"},
    {"transactional_help_directory", "For backend configuration help, use: --help backends"},
    {"threadsafe_help_directory", "For backend configuration help, use: --help backends"},
    {"composite_help_directory", "For backend configuration help, use: --help backends"},
    {"redirected_help_directory", "For backend configuration help, use: --help backends"},
    {"http3_client_help_directory", R"(For backend configuration help, use: --help backends
  For QUIC Connector configuration help, use: --help quic_connector)"},
    {"file_help_directory", "For backend configuration help, use: --help backends"},
    {"quic_connector_help_directory", "For http3_client configuration help, use: --help http3_client"},
    
    // Individual frontend help directories - redirect back to frontends_help
    {"cloning_mediator_help_directory", "For frontend configuration help, use: --help frontends"},
    {"yaml_mediator_help_directory", "For frontend configuration help, use: --help frontends"},
    {"http3_server_help_directory", R"(For frontend configuration help, use: --help frontends"
For QUIC Listener configuration help, use: --help quic_listener)"},
    {"quic_listener_help_directory", "For http3_server configuration help, use: --help http3_server"},
};

void printUsage(const char* program_name) {
    cout << "Usage: " << program_name << " [OPTIONS] <config_yaml_path>" << endl;
    cout << endl;
    cout << "WWATP Server - World Wide Anthropomorphic Tree Protocol Server" << endl;
    cout << endl;
    cout << "Arguments:" << endl;
    cout << "  config_yaml_path    Path to YAML configuration file (required)" << endl;
    cout << endl;
    cout << "Options:" << endl;
    cout << "  -h, --help [TOPIC] Show help message and exit" << endl;
    cout << "  --usage            Show usage information and exit" << endl;
    cout << "  --pool-size SIZE   Set memory pool size in GB (default: 1)" << endl;
    cout << "  --check-only       Validate configuration without starting server" << endl;
    cout << endl;
    cout << "Examples:" << endl;
    cout << "  " << program_name << " config.yaml" << endl;
    cout << "  " << program_name << " --pool-size 2 config.yaml" << endl;
    cout << "  " << program_name << " --check-only config.yaml" << endl;
    cout << "  " << program_name << " --help backends" << endl;
}

void printHelp(const string& help_topic = "") {
    string topic = help_topic.empty() ? "top_level" : help_topic;
    
    // Get help strings directly from static method
    try {
        const auto help_strings = WWATPService::getHelpStrings();
        
        // Try the topic as-is first, then with _help suffix
        string lookup_topic = topic;
        if (help_strings.find(lookup_topic) == help_strings.end()) {
            if (!lookup_topic.ends_with("_help")) {
                lookup_topic += "_help";
            }
        }
        
        // Print help string content if found
        if (help_strings.find(lookup_topic) != help_strings.end()) {
            cout << help_strings.at(lookup_topic) << endl;
        }
        
        // Check if there's a directory help request
        string directory_key;
        if (topic.ends_with("_help")) {
            directory_key = topic + "_directory";
        } else {
            directory_key = topic + "_help_directory";
        }
        
        // Print directory navigation if found
        if (help_directories.find(directory_key) != help_directories.end()) {
            cout << help_directories[directory_key] << endl;
        }
        
        // If neither help string nor directory was found, show error
        if (help_strings.find(lookup_topic) == help_strings.end() && 
            help_directories.find(directory_key) == help_directories.end()) {
            cout << "Unknown help topic: " << help_topic << endl;
            cout << "Use --help for available topics." << endl;
        }
        
    } catch (const exception& e) {
        cout << "Error accessing help: " << e.what() << endl;
        cout << "Use --help for available topics." << endl;
    }
}

int main(int argc, char* argv[]) {
    string config_path;
    uint64_t pool_size_gb = 1;
    bool check_only = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            // Check if there's a help topic following the help flag
            if (i + 1 < argc && !string(argv[i + 1]).starts_with("--")) {
                string help_topic = argv[++i];
                printHelp(help_topic);
            } else {
                printHelp();
            }
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

        // Create WWATPService: if a .yaml file is provided, read it and use YAML-string ctor;
        // otherwise, fall back to the path-based ctor (FileBackend node label behavior).
        unique_ptr<WWATPService> service;
        if (config_path.ends_with(".yaml")) {
            // Read YAML file into a string
            std::ifstream in(config_path);
            if (!in) {
                throw std::runtime_error(string("Failed to open YAML config file: ") + config_path);
            }
            std::string yaml_config{
                std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
            };
            // Construct service from YAML, honoring check_only for initialization mode
            service = make_unique<WWATPService>("wwatp_server", yaml_config, check_only);
        } else {
            service = make_unique<WWATPService>(config_path, check_only);
        }

        cout << "Service constructed successfully" << endl;

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