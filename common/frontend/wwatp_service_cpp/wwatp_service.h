#pragma once

#include <memory>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <boost/asio.hpp>

// Forward declarations
namespace YAML {
    class Node;
}

#include "backend.h"
#include "tree_node.h"
#include "file_backend.h"
#include "frontend_base.h"
#include "http3_client_backend.h"
#include "http3_server.h"
#include "quic_listener.h"
#include "quic_connector.h"


/**
 * WWATPService - A service class that constructs and manages WWATP backends and frontends
 * 
 * The WWATPService class is designed to set up various backends, frontends, and import plugins
 * to initialize a functional WWATP-based capability. It acts as a frontend that takes a backend
 * and configuration node to orchestrate the construction of a complete service architecture.
 * 
 * Key Responsibilities:
 * - Parse configuration from a config node
 * - Construct backends in proper dependency order (topological sort)
 * - Initialize supported frontends (mediators, plugins)
 * - Provide access to constructed backends by name
 * - Manage the lifecycle of all constructed components
 */
class WWATPService : public Frontend {
public:
    using ConnectorSpecifier = std::tuple<std::string, std::string, uint16_t>;

    /**
     * Constructor
     * @param config_backend Backend containing the configuration tree
     * @param config_label The label of the configuration node (default: "config")
     */
    WWATPService(std::string name, std::shared_ptr<Backend> config_backend, 
                 const std::string& config_label = "config");

    /**
     * Constructor with YAML configuration string
     * @param name Service name
     * @param yaml_config YAML configuration string
     * This constructor automatically creates the config backend from YAML and initializes the service
     */
    WWATPService(std::string name, const std::string& yaml_config);

    /**
     * Constructor with file path
     * @param path Path to YAML configuration file
     * This constructor recursively creates a config service to load the file, then initializes this service
     */
    WWATPService(const std::string& path);
    
    /**
     * Destructor - cleanup all constructed components
     */
    ~WWATPService();

    /**
     * Initialize the service by parsing config and constructing all components
     * This must be called after construction before using the service
     */
    void initialize();

    /**
     * Get a backend by name
     * @param backend_name Name of the backend as defined in config/backends/
     * @return Shared pointer to the backend, or nullptr if not found
     */
    std::shared_ptr<Backend> getBackend(const std::string& backend_name) const;

    /**
     * Get all available backend names
     * @return Vector of backend names
     */
    std::vector<std::string> getBackendNames() const;

    /**
     * Check if a backend exists
     * @param backend_name Name to check
     * @return True if backend exists
     */
    bool hasBackend(const std::string& backend_name) const;

    /**
     * Get the configuration backend used by this service
     * @return The configuration backend
     */
    std::shared_ptr<Backend> getConfigBackend() const { return config_backend_; }

    /**
     * Get the configuration label
     * @return The config label string
     */
    const std::string& getConfigLabel() const { return config_label_; }

    // Frontend interface implementation
    std::string getName() const { return name_; }
    std::string getType() const { return "wwatp_service"; }

    void run(size_t sleep_milli = 100) {
        if (!initialized_) {
            throw std::runtime_error("WWATPService not initialized");
        }
        if (quic_listeners_.empty() && quic_connectors_.empty()) {
            return;
        }
        auto& ignored = quic_listeners_.begin()->second;
        start(ignored, 0.0, sleep_milli);
    }

    void start(Communication& ignored, double time, size_t sleep_milli = 100) override;

    void stop() override; 

    bool isRunning() const override;

    std::vector<Backend*> getBackends() override; 

private:
    // Configuration
    std::string name_;
    std::shared_ptr<Backend> config_backend_;
    std::string config_label_;
    bool initialized_ = false;
    bool running_ = false;
    std::shared_ptr<WWATPService> configService_;
    
    // Network I/O context
    boost::asio::io_context io_context_;

    // Constructed components
    std::map<std::string, std::shared_ptr<Backend>> backends_;
    std::map<std::string, std::pair<std::shared_ptr<Frontend>, ConnectorSpecifier>> frontends_;
    std::map<ConnectorSpecifier, QuicListener> quic_listeners_;

    // The Http3ClientBackends need to be tracked by their updater
    std::map<ConnectorSpecifier, QuicConnector> quic_connectors_;
    std::map<ConnectorSpecifier, Http3ClientBackendUpdater> http3_client_updaters_;

    // Backend factory function type
    using BackendFactory = std::function<std::shared_ptr<Backend>(const TreeNode&)>;
    std::map<std::string, BackendFactory> backend_factories_;

    // Frontend factory function type
    using FrontendFactory = std::function<std::pair<std::shared_ptr<Frontend>, ConnectorSpecifier>(const TreeNode&)>;
    std::map<std::string, FrontendFactory> frontend_factories_;

    /**
     * Initialize backend and frontend factories
     */
    void initializeFactories();

    /**
     * Parse and construct all backends from config/backends/
     */
    void constructBackends();

    /**
     * Parse and construct all frontends from config/frontends/
     */
    void constructFrontends();

    /**
     * Perform topological sort on backend dependencies
     * @param backend_configs Map of backend name to configuration node
     * @return Ordered list of backend names for construction
     */
    std::vector<std::string> topologicalSort(
        const std::map<std::string, TreeNode>& backend_configs);

    /**
     * Get dependencies for a backend configuration
     * @param backend_config The backend configuration node
     * @return Set of backend names this backend depends on
     */
    std::vector<std::string> getBackendDependencies(const TreeNode& backend_config);

    /**
     * Create a specific backend type from configuration
     */
    std::shared_ptr<Backend> createSimpleBackend(const TreeNode& config);
    std::shared_ptr<Backend> createTransactionalBackend(const TreeNode& config);
    std::shared_ptr<Backend> createThreadsafeBackend(const TreeNode& config);
    std::shared_ptr<Backend> createCompositeBackend(const TreeNode& config);
    std::shared_ptr<Backend> createRedirectedBackend(const TreeNode& config);
    QuicConnector& obtainQuicConnector(const YAML::Node& config);
    Http3ClientBackendUpdater& obtainHttp3ClientUpdater(const YAML::Node& config);
    std::shared_ptr<Backend> createHttp3ClientBackend(const TreeNode& config);
    std::shared_ptr<Backend> createFileBackend(const TreeNode& config);

    /**
     * Create specific frontend types from configuration
     */
    std::pair<std::shared_ptr<Frontend>, ConnectorSpecifier> createCloningMediator(const TreeNode& config);
    std::pair<std::shared_ptr<Frontend>, ConnectorSpecifier> createYAMLMediator(const TreeNode& config);
    void updateServerWithChild(
        std::shared_ptr<HTTP3Server> server, const std::string& child_path, 
        const TreeNode& child_node, bool is_wwatp_route);
    std::pair<std::shared_ptr<Frontend>, ConnectorSpecifier> createHTTP3Server(const TreeNode& config);
    std::shared_ptr<Frontend> createHTTP3ClientUpdater(const TreeNode& config);
    
    /**
     * Create a QuicListener from configuration
     * @param server_name Name of the server for lookup
     * @param config YAML configuration node
     * @return Reference to the created QuicListener
     */
    QuicListener& obtainQuicListener(const YAML::Node& config);

    /**
     * Helper to get a string property from a TreeNode
     */
    std::string getStringProperty(const TreeNode& node, const std::string& property_name, 
                                 const std::string& default_value = "") const;

    /**
     * Helper to get an integer property from a TreeNode
     */
    int64_t getInt64Property(const TreeNode& node, const std::string& property_name, 
                      int64_t default_value = 0) const;

    /**
     * Helper to get an unsigned integer property from a TreeNode
     */
    uint64_t getUint64Property(const TreeNode& node, const std::string& property_name, 
                      uint64_t default_value = 0) const;

                      /**
     * Helper to get a boolean property from a TreeNode
     */
    bool getBoolProperty(const TreeNode& node, const std::string& property_name, 
                        bool default_value = false) const;

};

/**
 * Helper function to create a config backend from a YAML string
 * @param config_yaml The YAML configuration string
 * @return Shared pointer to the created configuration backend
 */
std::shared_ptr<Backend> createConfigBackendFromYAML(const std::string& config_yaml);
