#pragma once

#include <memory>
#include <string>
#include <map>
#include <vector>
#include <functional>

#include "backend.h"
#include "tree_node.h"
#include "file_backend.h"
#include "frontend_base.h"
#include "http3_client_backend.h"


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
    /**
     * Constructor
     * @param config_backend Backend containing the configuration tree
     * @param config_label The label of the configuration node (default: "config")
     */
    WWATPService(std::string name, std::shared_ptr<Backend> config_backend, 
                 const std::string& config_label = "config");
    
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

    void start(Communication& connector, double time, size_t sleep_milli = 100) override { 
        if (!initialized_) {
            throw std::runtime_error("WWATPService not initialized");
        }
        if (http3_client_updater_.size()) {
            http3_client_updater_.run(connector, time, sleep_milli);
        }
        for (const auto& frontend : frontends_) {
            frontend.second->start(connector, time, sleep_milli);
        }
    }

    void stop() override {
        if (!initialized_) {
            throw std::runtime_error("WWATPService not initialized");
        }
        if (http3_client_updater_.size()) {
            http3_client_updater_.stop();
        }
        for (const auto& frontend : frontends_) {
            frontend.second->stop();
        }
    }

private:
    // Configuration
    std::string name_;
    std::shared_ptr<Backend> config_backend_;
    std::string config_label_;
    bool initialized_ = false;

    // Constructed components
    std::map<std::string, std::shared_ptr<Backend>> backends_;
    std::map<std::string, std::shared_ptr<Frontend>> frontends_;

    // The Http3ClientBackends need to be tracked by their updater
    Http3ClientBackendUpdater http3_client_updater_;

    // Backend factory function type
    using BackendFactory = std::function<std::shared_ptr<Backend>(const TreeNode&)>;
    std::map<std::string, BackendFactory> backend_factories_;

    // Frontend factory function type
    using FrontendFactory = std::function<std::shared_ptr<Frontend>(const TreeNode&)>;
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
    std::shared_ptr<Backend> createHttp3ClientBackend(const TreeNode& config);
    std::shared_ptr<Backend> createFileBackend(const TreeNode& config);

    /**
     * Create specific frontend types from configuration
     */
    std::shared_ptr<Frontend> createCloningMediator(const TreeNode& config);
    std::shared_ptr<Frontend> createYAMLMediator(const TreeNode& config);

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