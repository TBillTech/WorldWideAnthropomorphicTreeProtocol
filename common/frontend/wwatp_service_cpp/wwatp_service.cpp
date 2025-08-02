#include "wwatp_service.h"
#include "config_error.h"

#include <algorithm>
#include <set>
#include <queue>
#include <stdexcept>
#include <iostream>
#include <tuple>
#include <filesystem>
#include <yaml-cpp/yaml.h>

// Backend implementations
#include "simple_backend.h"
#include "transactional_backend.h"
#include "threadsafe_backend.h"
#include "composite_backend.h"
#include "redirected_backend.h"
#include "http3_client_backend.h"
#include "memory_tree.h"

// Frontend implementations
#include "cloning_mediator.h"
#include "yaml_mediator.h"

using namespace fplus;
using namespace std;

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

WWATPService::ConnectorSpecifier getConnectorSpecifier(const YAML::Node& config) {
    std::string name = config["name"].as<std::string>("");
    std::string ip_address = config["ip"].as<std::string>("localhost");
    uint16_t port = config["port"].as<uint16_t>(443); // Default to 443 if not specified
    return std::make_tuple(name, ip_address, port);
}

ostream& operator<<(std::ostream& os, const WWATPService::ConnectorSpecifier& spec) {
    os << "ConnectorSpecifier(name: " << std::get<0>(spec)
       << ", ip: " << std::get<1>(spec)
       << ", port: " << std::get<2>(spec) << ")";
    return os;
}

WWATPService::WWATPService(string name, std::shared_ptr<Backend> config_backend, const std::string& config_label)
    : name_(name), config_backend_(config_backend), config_label_(config_label) {
    if (!config_backend_) {
        throw std::invalid_argument("Config backend cannot be null");
    }
    if (config_label_.empty()) {
        throw std::invalid_argument("Config label cannot be empty");
    }
}

WWATPService::WWATPService(string name, const std::string& yaml_config)
    : name_(name), config_backend_(createConfigBackendFromYAML(yaml_config)), config_label_("config") {
    if (!config_backend_) {
        throw std::invalid_argument("Failed to create config backend from YAML");
    }
    // Automatically initialize since the configuration is ready
    initialize();
}

WWATPService::WWATPService(const std::string& path, bool test_only)
    : config_label_("config") {
    // Get the name from the final directory in the path
    std::filesystem::path fs_path(path);
    name_ = fs_path.filename().string();
    if (name_.empty()) {
        name_ = "service";
    }
    
    // Get the parent directory for root_path
    std::string root_path = fs_path.parent_path().string();
    if (root_path.empty()) {
        root_path = ".";
    }
    
    // Construct config_yaml for the configService
    std::string config_yaml = R"(config:
  child_names: [backends, frontends]
  backends:
    child_names: [config_backend, yaml_file_backend]
    config_backend:
      type: simple
    yaml_file_backend:
      type: file
      root_path: )" + root_path + R"(
  frontends:
    child_names: [yaml_mediator]
    yaml_mediator:
      type: yaml_mediator
      tree_backend: config_backend
      yaml_backend: yaml_file_backend
      node_label: )" + name_ + R"(
      property_name: config
      property_type: yaml
      initialize_from_yaml: true
)";
    
    // Create the configService using the YAML constructor
    configService_ = std::make_shared<WWATPService>("config_service", config_yaml);
    
    // Get the simple backend from configService and set it as our config_backend_
    config_backend_ = configService_->getBackend("config_backend");
    if (!config_backend_) {
        throw std::runtime_error("Failed to get simple_backend from configService");
    }
    
    // Register a listener for all config_backend_ changes (commented out for now to avoid warnings)
    // config_backend_->registerNodeListener("service_config_listener", "", true, 
    //     [](Backend& backend, const std::string& label_rule, const fplus::maybe<TreeNode>& node) {
    //         // TODO: A future clever feature is to update the frontends and backends in the service as configuration changes on disk
    //     });
    
    // Initialize the service
    initialize(test_only);
}

WWATPService::~WWATPService() {
    if (initialized_) {
        // loop through the quic_connectors_ and stop each connector
        for (auto& [__, connector] : quic_connectors_) {
            connector.close();
        }
        // loop through the quic_listeners_ and stop each listener
        for (auto& [__, listener] : quic_listeners_) {
            listener.close();
        }
    }
}

void WWATPService::initialize(bool test_only) {
    if (initialized_) {
        return; // Already initialized
    }

    try {
        // Initialize the factory functions
        initializeFactories();

        // Construct backends first (order matters due to dependencies)
        constructBackends();

        // Then construct frontends
        constructFrontends();

        if (test_only) {
            std::cout << "WWATPService in test mode; skipping listener/connector setup; not initialized" << std::endl;
            return;
        }

        // Loop through the quic_listeners_ and start each listener
        for (auto& [conspec, listener] : quic_listeners_) {
            listener.listen(get<0>(conspec), get<1>(conspec), get<2>(conspec));
        }
        // Loop through the quic_connectors_ and start each connector
        for (auto& [conspec, connector] : quic_connectors_) {
            connector.connect(get<0>(conspec), get<1>(conspec), get<2>(conspec));
        }

        initialized_ = true;
        std::cout << "WWATPService initialized successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize WWATPService: " << e.what() << std::endl;
        throw;
    }
}

std::shared_ptr<Backend> WWATPService::getBackend(const std::string& backend_name) const {
    if (!initialized_) {
        throw std::runtime_error("WWATPService not initialized");
    }
    auto it = backends_.find(backend_name);
    return (it != backends_.end()) ? it->second : nullptr;
}

std::vector<std::string> WWATPService::getBackendNames() const {
    if (!initialized_) {
        throw std::runtime_error("WWATPService not initialized");
    }
    std::vector<std::string> names;
    names.reserve(backends_.size());
    for (const auto& [name, backend] : backends_) {
        names.push_back(name);
    }
    return names;
}

bool WWATPService::hasBackend(const std::string& backend_name) const {
    if (!initialized_) {
        throw std::runtime_error("WWATPService not initialized");
    }
    return backends_.find(backend_name) != backends_.end();
}

void WWATPService::initializeFactories() {

    // Initialize backend factories
    backend_factories_["simple"] = std::make_pair(
        [this](const TreeNode& config) { return this->createSimpleBackend(config); },
        "simple_help"
    );
    
    backend_factories_["transactional"] = std::make_pair(
        [this](const TreeNode& config) { return this->createTransactionalBackend(config); },
        "transactional_help"
    );
    
    backend_factories_["threadsafe"] = std::make_pair(
        [this](const TreeNode& config) { return this->createThreadsafeBackend(config); },
        "threadsafe_help"
    );
    
    backend_factories_["composite"] = std::make_pair(
        [this](const TreeNode& config) { return this->createCompositeBackend(config); },
        "composite_help"
    );
    
    backend_factories_["redirected"] = std::make_pair(
        [this](const TreeNode& config) { return this->createRedirectedBackend(config); },
        "redirected_help"
    );
    
    backend_factories_["http3_client"] = std::make_pair(
        [this](const TreeNode& config) { return this->createHttp3ClientBackend(config); },
        "http3_client_help"
    );
    
    backend_factories_["file"] = std::make_pair(
        [this](const TreeNode& config) { return this->createFileBackend(config); },
        "file_help"
    );

    // Initialize frontend factories
    frontend_factories_["cloning_mediator"] = std::make_pair(
        [this](const TreeNode& config) { return this->createCloningMediator(config); },
        "cloning_mediator_help"
    );
    
    frontend_factories_["yaml_mediator"] = std::make_pair(
        [this](const TreeNode& config) { return this->createYAMLMediator(config); },
        "yaml_mediator_help"
    );
    
    frontend_factories_["http3_server"] = std::make_pair(
        [this](const TreeNode& config) { return this->createHTTP3Server(config); },
        "http3_server_help"
    );
}

void WWATPService::constructBackends() {
    // Get the backends configuration node
    auto config_node = config_backend_->getNode(config_label_);
    if (config_node.is_nothing()) {
        throw ConfigError() << "Configuration node '" << config_label_ << "' not found";
    }

    auto backends_node = config_backend_->getNode(config_label_ + "/backends");
    if (backends_node.is_nothing()) {
        std::cout << "No backends configuration found, skipping backend construction" << std::endl;
        return;
    }

    // Get all backend child nodes
    std::map<std::string, TreeNode> backend_configs;
    for (const auto& child_name : backends_node.lift_def(std::vector<std::string>(), [](auto node){ return node.getChildNames(); })) {
        auto child_node = config_backend_->getNode(config_label_ + "/backends/" + child_name);
        if (child_node.is_just()) {
            backend_configs[child_name] = child_node.unsafe_get_just();
        }
        else {
            std::cerr << "Warning: Backend configuration for '" << child_name << "' not found" << std::endl;
        }
    }

    // Perform topological sort to handle dependencies
    auto construction_order = topologicalSort(backend_configs);

    // Construct backends in dependency order, accumulating errors
    ConfigError accumulated_errors;
    
    for (const std::string& backend_name : construction_order) {
        const TreeNode& backend_config = backend_configs[backend_name];
        
        string backend_type;
        try {
            backend_type = requireStringProperty(backend_config, "type");
        } catch (const ConfigError& e) {
            accumulated_errors << " Backend '" << backend_name << "' missing required 'type' property: " << e.what() << endl;
            continue;
        }

        auto factory_it = backend_factories_.find(backend_type);
        if (factory_it == backend_factories_.end()) {
            accumulated_errors << " Unknown backend type '" << backend_type << "' for backend '" << backend_name << "'." << endl;
            continue;
        }

        try {
            auto backend = factory_it->second.first(backend_config);
            backends_[backend_name] = backend;
            std::cout << "Created backend '" << backend_name << "' of type '" << backend_type << "'" << endl << flush;
        } catch (const std::exception& e) {
            accumulated_errors << " Failed to create backend '" << backend_name << "': " << e.what() << ". See " << factory_it->second.second << endl;
            continue;
        }
    }
    
    // Throw accumulated errors if any occurred
    if (accumulated_errors.HasErrors()) {
        throw accumulated_errors;
    }
}

void WWATPService::constructFrontends() {
    auto frontends_node = config_backend_->getNode(config_label_ + "/frontends");
    if (frontends_node.is_nothing()) {
        std::cout << "No frontends configuration found, skipping frontend construction" << std::endl;
        return;
    }

    // Construct each frontend, accumulating errors
    ConfigError accumulated_errors;
    
    for (const auto& frontend_name : frontends_node.lift_def(std::vector<std::string>(), [](auto node){ return node.getChildNames(); })) {
        auto frontend_node = config_backend_->getNode(config_label_ + "/frontends/" + frontend_name);
        if (frontend_node.is_nothing()) {
            continue;
        }

        std::string frontend_type;
        try {
            frontend_type = requireStringProperty(frontend_node.unsafe_get_just(), "type");
        } catch (const ConfigError& e) {
            accumulated_errors << " Frontend '" << frontend_name << "' missing required 'type' property: " << e.what() << endl;
            continue;
        }

        auto factory_it = frontend_factories_.find(frontend_type);
        if (factory_it == frontend_factories_.end()) {
            accumulated_errors << " Unknown frontend type '" << frontend_type << "' for frontend '" << frontend_name << "'." << endl;
            continue;
        }

        try {
            auto frontend = factory_it->second.first(frontend_node.unsafe_get_just());
            frontends_[frontend_name] = frontend;
            std::cout << "Created frontend '" << frontend_name << "' of type '" << frontend_type << "'" << endl << flush;
        } catch (const std::exception& e) {
            accumulated_errors << " Failed to create frontend '" << frontend_name << "': " << e.what() << ". See " << factory_it->second.second << endl;
            continue;
        }
    }
    
    // Throw accumulated errors if any occurred
    if (accumulated_errors.HasErrors()) {
        throw accumulated_errors;
    }
}

std::vector<std::string> WWATPService::topologicalSort(const std::map<std::string, TreeNode>& backend_configs) {
    std::map<std::string, std::vector<std::string>> dependencies;
    std::map<std::string, int> in_degree;
    
    // Initialize in-degree counts and dependency lists
    for (const auto& [name, config] : backend_configs) {
        auto deps = getBackendDependencies(config);
        dependencies[name] = deps;
        in_degree[name] = 0;
    }
    
    // Calculate in-degrees and check for missing dependencies
    std::vector<std::pair<std::string, std::string>> missing_deps;
    for (const auto& [name, deps] : dependencies) {
        for (const std::string& dep : deps) {
            if (backend_configs.find(dep) != backend_configs.end()) {
                in_degree[name]++;
            } else {
                missing_deps.push_back({name, dep});
            }
        }
    }
    
    // Report missing dependencies
    if (!missing_deps.empty()) {
        ConfigError error;
        error << "Missing backend dependencies detected: ";
        for (size_t i = 0; i < missing_deps.size(); ++i) {
            error << missing_deps[i].first << " -> " << missing_deps[i].second;
            if (i < missing_deps.size() - 1) {
                error << ", ";
            }
        }
        throw error;
    }
    
    // Kahn's algorithm for topological sorting
    std::queue<std::string> queue;
    std::vector<std::string> result;
    
    // Add nodes with no dependencies
    for (const auto& [name, degree] : in_degree) {
        if (degree == 0) {
            queue.push(name);
        }
    }
    
    while (!queue.empty()) {
        std::string current = queue.front();
        queue.pop();
        result.push_back(current);
        
        // Reduce in-degree for dependent nodes
        for (const auto& [name, deps] : dependencies) {
            auto it = std::find(deps.begin(), deps.end(), current);
            if (it != deps.end()) {
                in_degree[name]--;
                if (in_degree[name] == 0) {
                    queue.push(name);
                }
            }
        }
    }
    
    // Check for cycles
    if (result.size() != backend_configs.size()) {
        // Find nodes that are part of the cycle
        std::vector<std::string> cycle_nodes;
        for (const auto& [name, config] : backend_configs) {
            if (std::find(result.begin(), result.end(), name) == result.end()) {
                cycle_nodes.push_back(name);
            }
        }
        
        // Find a cycle by doing DFS from unprocessed nodes
        ConfigError error;
        error << "Circular dependency detected in backend configuration. ";
        if (!cycle_nodes.empty()) {
            // Try to find an actual cycle path using DFS
            std::set<std::string> visited;
            std::vector<std::string> path;
            bool cycle_found = false;
            std::function<bool(const std::string&)> findCycle = [&](const std::string& node) -> bool {
                if (std::find(path.begin(), path.end(), node) != path.end()) {
                    // Found cycle - extract it
                    auto cycle_start = std::find(path.begin(), path.end(), node);
                    std::vector<std::string> cycle_path(cycle_start, path.end());
                    cycle_path.push_back(node); // Close the cycle
                    error << "Cycle: ";
                    for (size_t i = 0; i < cycle_path.size(); ++i) {
                        error << cycle_path[i];
                        if (i < cycle_path.size() - 1) {
                            error << " -> ";
                        }
                    }
                    cycle_found = true;
                    return true;
                }
                
                if (visited.find(node) != visited.end()) {
                    return false;
                }
                
                visited.insert(node);
                path.push_back(node);
                
                auto deps_it = dependencies.find(node);
                if (deps_it != dependencies.end()) {
                    for (const std::string& dep : deps_it->second) {
                        if (backend_configs.find(dep) != backend_configs.end()) {
                            if (findCycle(dep)) {
                                return true;
                            }
                        }
                    }
                }
                
                path.pop_back();
                return false;
            };
            
            // Try to find a cycle starting from any unprocessed node
            for (const std::string& node : cycle_nodes) {
                visited.clear();
                path.clear();
                if (findCycle(node)) {
                    break;
                }
            }
            
            // If we couldn't find a specific cycle, just list the problematic nodes
            if (!cycle_found) {
                error << "Problematic backends: ";
                for (size_t i = 0; i < cycle_nodes.size(); ++i) {
                    error << cycle_nodes[i];
                    if (i < cycle_nodes.size() - 1) {
                        error << ", ";
                    }
                }
            }
        }
        
        throw error;
    }
    
    return result;
}

std::vector<std::string> WWATPService::getBackendDependencies(const TreeNode& backend_config) {
    std::vector<std::string> dependencies;
    
    // First, require the backend type up front
    std::string backend_type = requireStringProperty(backend_config, "type");
    
    // If it's a simple or file backend, it has no dependencies
    if (backend_type == "simple" || backend_type == "file") {
        return dependencies;
    }
    
    // Every other backend MUST have a backend field
    std::string backend_dependency = requireStringProperty(backend_config, "backend");
    dependencies.push_back(backend_dependency);
    
    // If it's a composite backend, add all child backends as dependencies
    if (backend_type == "composite") {
        for (const std::string& child_name : backend_config.getChildNames()) {
            dependencies.push_back(child_name);
        }
    }
    
    return dependencies;
}

// Helper methods for property extraction
std::string WWATPService::getStringProperty(const TreeNode& node, const std::string& property_name, 
                                           const std::string& default_value) const {
    try {
        auto [__, string_data] = node.getPropertyString(property_name);
        return string_data.empty() ? default_value : string_data;
    } catch (const std::exception&) {
        // If direct property access fails, try config YAML if it exists
        if (!hasConfigProperty(node)) {
            return default_value;
        }
        return getConfigStringProperty(node, property_name, default_value);
    }
}

int64_t WWATPService::getInt64Property(const TreeNode& node, const std::string& property_name, 
                                int64_t default_value) const {
    try {
        return get<1>(node.getPropertyValue<int64_t>(property_name));
    } catch (const std::exception&) {
        // If direct property access fails, try config YAML if it exists
        if (!hasConfigProperty(node)) {
            return default_value;
        }
        return getConfigInt64Property(node, property_name, default_value);
    }
}

uint64_t WWATPService::getUint64Property(const TreeNode& node, const std::string& property_name, 
                                uint64_t default_value) const {
    try {
        return get<1>(node.getPropertyValue<uint64_t>(property_name));
    } catch (const std::exception&) {
        // If direct property access fails, try config YAML if it exists
        if (!hasConfigProperty(node)) {
            return default_value;
        }
        return getConfigUint64Property(node, property_name, default_value);
    }
}

bool WWATPService::getBoolProperty(const TreeNode& node, const std::string& property_name, 
                                  bool default_value) const {
    try {
        return get<1>(node.getPropertyValue<bool>(property_name));
    } catch (const std::exception&) {
        // If direct property access fails, try config YAML if it exists
        if (!hasConfigProperty(node)) {
            return default_value;
        }
        return getConfigBoolProperty(node, property_name, default_value);
    }
}

std::string WWATPService::requireConfigStringProperty(const TreeNode& node, const std::string& property_name) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<std::string>();
        }
        throw ConfigError() << "Required property '" << property_name << "' not found in config YAML";
    } catch (const ConfigError&) {
        throw; // Re-throw ConfigError as-is
    } catch (const std::exception& e) {
        throw ConfigError() << "Error accessing required config property '" << property_name << "': " << e.what();
    }
}

int64_t WWATPService::requireConfigInt64Property(const TreeNode& node, const std::string& property_name) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<int64_t>();
        }
        throw ConfigError() << "Required property '" << property_name << "' not found in config YAML";
    } catch (const ConfigError&) {
        throw; // Re-throw ConfigError as-is
    } catch (const std::exception& e) {
        throw ConfigError() << "Error accessing required config property '" << property_name << "': " << e.what();
    }
}

uint64_t WWATPService::requireConfigUint64Property(const TreeNode& node, const std::string& property_name) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<uint64_t>();
        }
        throw ConfigError() << "Required property '" << property_name << "' not found in config YAML";
    } catch (const ConfigError&) {
        throw; // Re-throw ConfigError as-is
    } catch (const std::exception& e) {
        throw ConfigError() << "Error accessing required config property '" << property_name << "': " << e.what();
    }
}

bool WWATPService::requireConfigBoolProperty(const TreeNode& node, const std::string& property_name) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<bool>();
        }
        throw ConfigError() << "Required property '" << property_name << "' not found in config YAML";
    } catch (const ConfigError&) {
        throw; // Re-throw ConfigError as-is
    } catch (const std::exception& e) {
        throw ConfigError() << "Error accessing required config property '" << property_name << "': " << e.what();
    }
}

std::string WWATPService::requireStringProperty(const TreeNode& node, const std::string& property_name) const {
    try {
        auto [__, string_data] = node.getPropertyString(property_name);
        if (!string_data.empty()) {
            return string_data;
        }
    } catch (const std::exception&) {
        // Fall through to try config YAML
    }
    
    // Try config YAML if direct property access failed or returned empty
    if (!hasConfigProperty(node)) {
        throw ConfigError() << "Required property '" << property_name << "' not found in node";
    }
    
    return requireConfigStringProperty(node, property_name);
}

int64_t WWATPService::requireInt64Property(const TreeNode& node, const std::string& property_name) const {
    try {
        return get<1>(node.getPropertyValue<int64_t>(property_name));
    } catch (const std::exception&) {
        // Fall through to try config YAML
    }
    
    // Try config YAML if direct property access failed
    if (!hasConfigProperty(node)) {
        throw ConfigError() << "Required property '" << property_name << "' not found in node";
    }
    
    return requireConfigInt64Property(node, property_name);
}

uint64_t WWATPService::requireUint64Property(const TreeNode& node, const std::string& property_name) const {
    try {
        return get<1>(node.getPropertyValue<uint64_t>(property_name));
    } catch (const std::exception&) {
        // Fall through to try config YAML
    }
    
    // Try config YAML if direct property access failed
    if (!hasConfigProperty(node)) {
        throw ConfigError() << "Required property '" << property_name << "' not found in node";
    }
    
    return requireConfigUint64Property(node, property_name);
}

bool WWATPService::requireBoolProperty(const TreeNode& node, const std::string& property_name) const {
    try {
        return get<1>(node.getPropertyValue<bool>(property_name));
    } catch (const std::exception&) {
        // Fall through to try config YAML
    }
    
    // Try config YAML if direct property access failed
    if (!hasConfigProperty(node)) {
        throw ConfigError() << "Required property '" << property_name << "' not found in node";
    }
    
    return requireConfigBoolProperty(node, property_name);
}

bool WWATPService::hasConfigProperty(const TreeNode& node) const {
    try {
        auto [unused1, unused2, yaml_span] = node.getPropertyValueSpan("config");
        return yaml_span.size() > 0;
    } catch (const std::exception&) {
        return false;
    }
}

YAML::Node WWATPService::getConfigProperty(const TreeNode& node) const {
    try {
        auto [unused1, unused2, yaml_span] = node.getPropertyValueSpan("config");
        if (yaml_span.size() == 0) {
            throw ConfigError() << "Node missing required 'config' property with YAML content";
        }
        string yaml_content(yaml_span.begin<const char>(), yaml_span.end<const char>());
        return YAML::Load(yaml_content);
    } catch (const YAML::ParserException& e) {
        throw ConfigError() << "YAML parsing error in config property: " << e.what();
    } catch (const ConfigError&) {
        throw; // Re-throw ConfigError as-is
    } catch (const std::exception& e) {
        throw ConfigError() << "Error accessing config property: " << e.what();
    }
}

std::string WWATPService::getConfigStringProperty(const TreeNode& node, const std::string& property_name, 
                                                  const std::string& default_value) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<std::string>();
        }
        return default_value;
    } catch (const std::exception&) {
        return default_value;
    }
}

int64_t WWATPService::getConfigInt64Property(const TreeNode& node, const std::string& property_name, 
                                             int64_t default_value) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<int64_t>();
        }
        return default_value;
    } catch (const std::exception&) {
        return default_value;
    }
}

uint64_t WWATPService::getConfigUint64Property(const TreeNode& node, const std::string& property_name, 
                                               uint64_t default_value) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<uint64_t>();
        }
        return default_value;
    } catch (const std::exception&) {
        return default_value;
    }
}

bool WWATPService::getConfigBoolProperty(const TreeNode& node, const std::string& property_name, 
                                         bool default_value) const {
    try {
        YAML::Node yaml_config = getConfigProperty(node);
        if (yaml_config[property_name]) {
            return yaml_config[property_name].as<bool>();
        }
        return default_value;
    } catch (const std::exception&) {
        return default_value;
    }
}

// Backend creation methods
std::shared_ptr<Backend> WWATPService::createSimpleBackend(const TreeNode&) {
    // SimpleBackend requires a MemoryTree
    auto memory_tree = std::make_shared<MemoryTree>();
    return std::make_shared<SimpleBackend>(memory_tree);
}

std::shared_ptr<Backend> WWATPService::createTransactionalBackend(const TreeNode& config) {
    std::string underlying_backend_name = requireStringProperty(config, "backend");

    auto underlying_backend = backends_.find(underlying_backend_name);
    if (underlying_backend == backends_.end()) {
        throw ConfigError() << "Underlying backend '" << underlying_backend_name << "' not found";
    }
    
    return std::make_shared<TransactionalBackend>(*underlying_backend->second);
}

std::shared_ptr<Backend> WWATPService::createThreadsafeBackend(const TreeNode& config) {
    string underlying_backend_name = requireStringProperty(config, "backend");

    auto underlying_backend = backends_.find(underlying_backend_name);
    if (underlying_backend == backends_.end()) {
        throw ConfigError() << "Underlying backend '" << underlying_backend_name << "' not found";
    }
    
    return std::make_shared<ThreadsafeBackend>(*underlying_backend->second);
}

std::shared_ptr<Backend> WWATPService::createCompositeBackend(const TreeNode& config) {
    string root_backend_name = requireStringProperty(config, "backend");

    auto root_backend = backends_.find(root_backend_name);
    if (root_backend == backends_.end()) {
        throw ConfigError() << "Root backend '" << root_backend_name << "' not found";
    }

    auto composite = std::make_shared<CompositeBackend>(*root_backend->second);

    ConfigError accumulated_errors;
    // Iterate through the children nodes, which will have names == backend name, and a string property called "mount_path"
    for (const auto& child_name : config.getChildNames()) {
        auto child_label = config.getLabelRule() + "/" + child_name;
        auto child_node = config_backend_->getNode(child_label);
        if (child_node.is_nothing()) {
            accumulated_errors << "Child backend configuration '" << child_name << "' not found" << endl;
            continue;
        }
        string mount_path;
        try {
            mount_path = requireStringProperty(child_node.unsafe_get_just(), "mount_path");
        } catch (const ConfigError& e) {
            accumulated_errors << "Child backend '" << child_name << "' missing required 'mount_path' property: " << e.what() << endl;
            continue;
        }
        auto backend_name = child_node.unsafe_get_just().getNodeName();
        auto child_backend = backends_.find(child_name);
        if (child_backend == backends_.end()) {
            accumulated_errors << "Child backend '" << child_name << "' not found" << endl;
            continue;
        }
        if (!composite->mountBackend(mount_path, *(child_backend->second)).second) {
            accumulated_errors << "Failed to add child backend '" << child_name << "' to composite backend" << endl;
        } else {
            std::cout << "Added child backend '" << child_name << "' at mount path '" << mount_path << "'" << endl;
        }
    }
    if (accumulated_errors.HasErrors()) {
        throw accumulated_errors;
    }
    
    return composite;
}

std::shared_ptr<Backend> WWATPService::createRedirectedBackend(const TreeNode& config) {
    string underlying_backend_name = requireStringProperty(config, "backend");

    auto underlying_backend = backends_.find(underlying_backend_name);
    if (underlying_backend == backends_.end()) {
        throw ConfigError() << "Underlying backend '" << underlying_backend_name << "' not found";
    }

    string redirect_root = requireStringProperty(config, "redirect_root");
    return std::make_shared<RedirectedBackend>(*(underlying_backend->second), redirect_root);
}

QuicConnector& WWATPService::obtainQuicConnector(const YAML::Node& config) {
    ConnectorSpecifier conspec = getConnectorSpecifier(config);
    auto it = quic_connectors_.find(conspec);
    if (it != quic_connectors_.end()) {
        return it->second;
    }
    auto connector_it = quic_connectors_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(conspec),
        std::forward_as_tuple(io_context_, config)
    );

    if (!connector_it.second) {
        throw ConfigError() << "Failed to create QuicConnector for specifier: " << get<1>(conspec) << ":" << to_string(get<2>(conspec));
    }
    std::cout << "Created QuicConnector for specifier: " << conspec << std::endl;
    return connector_it.first->second;
}

Http3ClientBackendUpdater& WWATPService::obtainHttp3ClientUpdater(const YAML::Node& config) {
    ConnectorSpecifier conspec = getConnectorSpecifier(config);
    auto it = http3_client_updaters_.find(conspec);
    if (it != http3_client_updaters_.end()) {
        return it->second;
    }
    auto updater_it = http3_client_updaters_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(conspec),
        std::forward_as_tuple(get<0>(conspec), get<1>(conspec), get<2>(conspec))
    );

    return updater_it.first->second;
}


std::shared_ptr<Backend> WWATPService::createHttp3ClientBackend(const TreeNode& config) {
    // Get the yaml configuration for the HTTP3 client backend
    if (!hasConfigProperty(config)) {
        throw ConfigError() << "HTTP3ClientBackend requires a 'config' property with YAML content";
    }
    YAML::Node yaml_config = getConfigProperty(config);

    ConnectorSpecifier connspec = getConnectorSpecifier(yaml_config);

    string backend_name = requireStringProperty(config, "backend");
    auto local_backend = backends_.find(backend_name);
    if (local_backend == backends_.end()) {
        throw ConfigError() << "Backend '" << backend_name << "' not found for HTTP3ClientBackend";
    }
    bool blocking_mode = getBoolProperty(config, "blocking_mode", false);
    size_t journalRequestsPerMinute = static_cast<size_t>(getUint64Property(config, "journal_requests_per_minute", 0));
    Request request;
    request.scheme = getStringProperty(config, "scheme", "https");
    string ip = getStringProperty(config, "ip", "127.0.0.1");
    string port_str = getStringProperty(config, "port", "443");
    request.authority = getStringProperty(config, "authority", ip + ":" + port_str);
    request.path = requireStringProperty(config, "path");
    request.method = getStringProperty(config, "method", "GET");
    request.pri.urgency = static_cast<int32_t>(getInt64Property(config, "priority", 0));
    request.pri.inc = getInt64Property(config, "increment", 0);
    maybe<TreeNode> staticNode;
    string static_node_label = getStringProperty(config, "static_node_label", "");
    if (!static_node_label.empty()) {
        auto static_node_description = getStringProperty(config, "static_node_description", static_node_label);
        string type = "string";
        // the static_node_label is of the form "name.type"
        string name = static_node_label.substr(0, static_node_label.find('.'));
        if (name.empty()) {
            throw ConfigError() << "Static node label must have a name before the dot";
        }
        if (static_node_label.find('.') != string::npos) {
            type = static_node_label.substr(static_node_label.find('.') + 1);
            if (type.empty()) {
                throw ConfigError() << "Static node label must have a type after the dot";
            }
        }
        TreeNode::PropertyInfo static_info({type, name});
        TreeNode staticHtmlNode(
            static_node_label,
            static_node_description,
            {static_info},
            DEFAULT_TREE_NODE_VERSION,
            {},
            shared_span<>(global_no_chunk_header, false),
            fplus::nothing<std::string>(),
            fplus::nothing<std::string>()
        );
        staticNode = just(staticHtmlNode);
    }
    obtainQuicConnector(yaml_config); // Ensure the connector is created
    Http3ClientBackendUpdater& http3_client_updater = obtainHttp3ClientUpdater(yaml_config);
    Http3ClientBackend& http3_client_backend = http3_client_updater.addBackend(*(local_backend->second), blocking_mode, request, journalRequestsPerMinute, staticNode);
    // Use shared_ptr with custom deleter that doesn't delete - the updater owns the object
    return shared_ptr<Http3ClientBackend>(&http3_client_backend, [](Http3ClientBackend*){});
}


std::shared_ptr<Backend> WWATPService::createFileBackend(const TreeNode& config) {
    std::string root_path = requireStringProperty(config, "root_path");
    return std::make_shared<FileBackend>(root_path);
}

// Frontend creation methods
pair<shared_ptr<Frontend>, WWATPService::ConnectorSpecifier> WWATPService::createCloningMediator(const TreeNode& config) {
    std::string name = getStringProperty(config, "name", config.getNodeName());
    
    std::string backend_a_name = requireStringProperty(config, "backend_a");
    std::string backend_b_name = requireStringProperty(config, "backend_b");

    if (backend_a_name.empty() || backend_b_name.empty()) {
        throw ConfigError() << "CloningMediator requires backend names to be non-empty";
    }

    auto backend_a = backends_.find(backend_a_name);
    auto backend_b = backends_.find(backend_b_name);

    if (backend_a == backends_.end() || backend_b == backends_.end()) {
        throw ConfigError() << "Required backends not found for CloningMediator '" + name + "'";
    }
    
    bool versioned = getBoolProperty(config, "versioned", false);

    return {make_shared<CloningMediator>(name, *(backend_a->second), *(backend_b->second), versioned),
            tuple<string, string, uint16_t>(name, "", 0)}; // Dummy connector specifier
}

pair<shared_ptr<Frontend>, WWATPService::ConnectorSpecifier> WWATPService::createYAMLMediator(const TreeNode& config) {
    string name = getStringProperty(config, "name", config.getNodeName());
    
    string tree_backend_name = requireStringProperty(config, "tree_backend");
    string yaml_backend_name = requireStringProperty(config, "yaml_backend");

    if (tree_backend_name.empty() || yaml_backend_name.empty()) {
        throw ConfigError() << "YAMLMediator requires 'tree_backend' and 'yaml_backend' properties";
    }
    
    auto tree_backend = backends_.find(tree_backend_name);
    auto yaml_backend = backends_.find(yaml_backend_name);

    if (tree_backend == backends_.end() || yaml_backend == backends_.end()) {
        throw ConfigError() << "Required backends not found for YAMLMediator '" + name + "'";
    }
    
    // PropertySpecifier configuration
    string node_label = requireStringProperty(config, "node_label");
    string property_name = requireStringProperty(config, "property_name");
    string property_type = getStringProperty(config, "property_type", "yaml");
    
    PropertySpecifier specifier(node_label, property_name, property_type);
    
    bool initialize_from_yaml = getBoolProperty(config, "initialize_from_yaml", true);

    return {make_shared<YAMLMediator>(name, *(tree_backend->second), *(yaml_backend->second), specifier, initialize_from_yaml),
            tuple<string, string, uint16_t>(name, "", 0)}; // Dummy connector specifier
}

void WWATPService::updateServerWithChild( 
    std::shared_ptr<HTTP3Server> server, const std::string& child_path, 
    const TreeNode& child_node, bool is_wwatp_route) {
    if (is_wwatp_route) {
        string backend_name = requireStringProperty(child_node, "backend");
        size_t journal_size = getUint64Property(child_node, "journal_size", 1000);
        auto find_backend = backends_.find(backend_name);
        if (find_backend == backends_.end()) {
            throw ConfigError() << "Backend not found: " + backend_name;
        }
        string url_path = child_path;
        if (url_path.back() != '/') {
            url_path += '/';
        }
        server->addBackendRoute(*find_backend->second, journal_size, url_path);
    }
    else
    {
        for (auto & [type, name] : child_node.getPropertyInfo()) {
            auto static_name = name + "." + type;
            auto url = "/" + static_name;
            auto shared_span = child_node.getPropertyValueSpan(name);
            chunks asset = {get<2>(shared_span)};
            server->addStaticAsset(url, asset);
        }
    }
    auto children_names = child_node.getChildNames();
    ConfigError accumulated_errors;
    for (const auto& child_name : children_names) {
        auto child_node_label = child_node.getLabelRule() + "/" + child_name;
        if (child_name.find("/") != std::string::npos) {
            child_node_label = child_name;
        }
        auto child_child_node = config_backend_->getNode(child_node_label);
        if (child_child_node.is_nothing()) {
            accumulated_errors << "Warning: Descendant node '" << child_node_label << "' not found in HTTP3Server configuration";
            continue;
        }
        auto child_child_path = child_path + "/" + child_child_node.unsafe_get_just().getNodeName();
        updateServerWithChild(server, child_child_path, child_child_node.unsafe_get_just(), 
            is_wwatp_route || child_name == "wwatp");
    }
    if (accumulated_errors.HasErrors()) {
        throw accumulated_errors;
    }
}

pair<shared_ptr<Frontend>, WWATPService::ConnectorSpecifier> WWATPService::createHTTP3Server(const TreeNode& config) {
    string name = getStringProperty(config, "name", config.getNodeName());
    
    // Load the YAML configuration from the yaml_string
    YAML::Node yaml_config = getConfigProperty(config);
    ConnectorSpecifier connspec = getConnectorSpecifier(yaml_config);
    // Look for the "static_load" flag in the yaml_config
    bool static_load = getConfigBoolProperty(config, "static_load", false);
    // Next get the name and port from the yaml_config
    std::string server_name = getConfigStringProperty(config, "name", name);
    QuicListener& listener = obtainQuicListener(yaml_config);

    // Create HTTP3Server with empty static assets
    auto server = std::make_shared<HTTP3Server>(name);

    // Now loop through all of the properties in the TreeNode config, and create static assets or wwatp backend routes as configured
    for (const auto& [type, property_name] : config.getPropertyInfo()) {
        if (property_name == "config") {
            // Skip the config property itself, as it is already handled
            continue;
        }
        auto static_name = property_name + "." + type;
        auto url = "/" + static_name;
        chunks asset;
        if (static_load) {
            string path = requireStringProperty(config, property_name);
            asset = readFileChunks(path);
        } else {
            auto shared_span = config.getPropertyValueSpan(property_name);
            asset.push_back(get<2>(shared_span));
        }
        server->addStaticAsset(url, asset);
    }

    auto children_names = config.getChildNames();
    ConfigError accumulated_errors;
    for (const auto& child_name : children_names) {
        auto child_node_label = config.getLabelRule() + "/" + child_name;
        auto child_node = config_backend_->getNode(child_node_label);
        if (child_node.is_nothing()) {
            accumulated_errors << "Warning: Child node '" << child_name << "' not found in HTTP3Server configuration";
            continue;
        }
        auto child_path = "/" + child_node.unsafe_get_just().getNodeName();
        updateServerWithChild(server, child_path, child_node.unsafe_get_just(), child_path == "/wwatp");
    }
    if (accumulated_errors.HasErrors()) {
        throw accumulated_errors;
    }

    prepare_stream_callback_fn theServerHandlerWrapper = [server](const Request &req) {
        return server->getResponseCallback(req);
    };

    listener.registerRequestHandler(make_pair(server_name, theServerHandlerWrapper));


    return {server, connspec};
}

QuicListener& WWATPService::obtainQuicListener(const YAML::Node& config) {
    // Extract configuration parameters for logging
    std::string private_key_file = config["private_key_file"].as<std::string>("");
    std::string cert_file = config["cert_file"].as<std::string>("");
    auto connspec = getConnectorSpecifier(config);

    auto findit = quic_listeners_.find(connspec);
    if(findit != quic_listeners_.end()) {
        std::cout << "QuicListener " << connspec << " already exists, returning existing instance with private key: " 
                  << private_key_file << ", cert: " << cert_file << std::endl;
        return findit->second;
    }
    // Store in the map - the QuicListener constructor now handles config validation
    auto listener = quic_listeners_.emplace(std::piecewise_construct,
                           std::forward_as_tuple(connspec),
                           std::forward_as_tuple(io_context_, config));
    std::cout << "Created QuicListener " << connspec << " with private_key: " << private_key_file
              << ", cert: " << cert_file << std::endl;

    return listener.first->second;
}

void WWATPService::start(Communication&, double time, size_t sleep_milli) { 
    if (!initialized_) {
        throw std::runtime_error("WWATPService not initialized");
    }
    // Loop through the frontends_ and start each frontend
    for (auto& [name, frontend_pair ] : frontends_) {
        auto [frontend, conspec] = frontend_pair;
        // Find the listener by name, which for HTTP3Server is the same as the get<0>(conspec)
        auto listener_it = quic_listeners_.find(conspec);
        if (listener_it != quic_listeners_.end()) {
            frontend->start(listener_it->second, time, sleep_milli);
        } else {
            if (dynamic_cast<HTTP3Server*>(frontend.get()) != nullptr) {
                std::cerr << "Warning: Listener for HTTP3Server frontend '" << name << "' not found, skipping start" << std::endl;
            }
        }
    }
    // loop through the http3_client_updaters_ and start each updater
    for (auto& [conspec, updater] : http3_client_updaters_) {
        auto findit = quic_connectors_.find(conspec);
        if (findit == quic_connectors_.end()) {
            std::cerr << "Warning: QuicConnector for specifier " << conspec << " not found, skipping updater start" << std::endl;
            continue;
        }
        updater.start(findit->second, time, sleep_milli);
    }
    std::cout << "WWATPService started successfully" << std::endl;
    running_ = true;
    initialized_ = true;
}

void WWATPService::stop() {
    if (!initialized_) {
        throw std::runtime_error("WWATPService not initialized");
    }
    // loop through the http3_client_updaters_ and stop each updater
    for (auto& [conspec, updater] : http3_client_updaters_) {
        updater.stop(); 
    }
    // loop through the frontends_ and stop each frontend
    for (auto& [name, frontend_pair] : frontends_) {
        auto [frontend, conspec] = frontend_pair;
        frontend->stop();
    }
    std::cout << "WWATPService stopped successfully" << std::endl;
    running_ = false;
}

bool WWATPService::isRunning() const {
    return running_;
}

void WWATPService::responseCycle(string message, size_t wait_loops, size_t millis, double &time_secs)
{
    
    // Service the client communication for a while to allow the client to send the request.
    if (!message.empty())
        cout << "In Main: Waiting for " << message << endl << flush;
    for(size_t i = 0; i < wait_loops; i++) {
        // Process all HTTP3 client updaters
        for (auto& [conspec, updater] : http3_client_updaters_) {
            auto connector_it = quic_connectors_.find(conspec);
            if (connector_it != quic_connectors_.end()) {
                updater.maintainRequestHandlers(connector_it->second, time_secs);
            }
        }
        
        // Process all connectors (client communication)
        for (auto& [conspec, connector] : quic_connectors_) {
            connector.processRequestStream();
        }
        
        // Process all listeners (server communication)
        for (auto& [conspec, listener] : quic_listeners_) {
            listener.processResponseStream();
        }
        
        this_thread::sleep_for(chrono::milliseconds(millis));
        time_secs += 0.001 * millis;
    }
    if (!message.empty())
        cout << "In Main: Waited for " << message << endl << flush;
}

std::vector<Backend*> WWATPService::getBackends() {
    std::vector<Backend*> result;
    result.reserve(backends_.size());
    for (const auto& [name, backend] : backends_) {
        result.push_back(backend.get());
    }
    return result;
}

std::shared_ptr<Backend> createConfigBackendFromYAML(const std::string& config_yaml) {
    // Create configuration backends: one for tree structure, one for YAML storage
    auto config_memory_tree = make_shared<MemoryTree>();
    auto config_backend = make_shared<SimpleBackend>(config_memory_tree);
    auto config_yaml_memory_tree = make_shared<MemoryTree>();
    SimpleBackend config_yaml_backend(config_yaml_memory_tree);
    
    // Create a YAML storage node in the config_yaml_backend
    shared_span<> no_content(global_no_chunk_header, false);
    TreeNode yaml_config_node("config_yaml", "YAML Configuration Storage", 
        {}, DEFAULT_TREE_NODE_VERSION, {}, std::move(no_content), 
        nothing<string>(), nothing<string>());
    
    // Insert the YAML property using insertPropertyString
    yaml_config_node.insertPropertyString(0, "config", "yaml", config_yaml);
    
    // Insert the YAML configuration node
    config_yaml_backend.upsertNode({yaml_config_node});
    
    // Create PropertySpecifier for the YAMLMediator
    PropertySpecifier specifier("config_yaml", "config", "yaml");
    
    {
        // Create YAMLMediator to convert YAML to tree structure
        // initialize_from_yaml = true means read from config_yaml_backend and populate config_backend
        YAMLMediator yaml_mediator("config_mediator", *config_backend, config_yaml_backend, specifier, true);
    }
    
    return config_backend;
}

// Help strings for configuration documentation
const std::string WWATPService::top_level_help = R"(
WWATP Service Configuration:
  The WWATP (World Wide Anthropomorphic Tree Protocol) service uses YAML configuration
  to define backends, frontends, and their interconnections. The configuration follows
  a hierarchical structure with the following top-level sections:

  config/
    backends/       - Data storage and management backends
      <name>:         Backend instance configuration
        type: <type>    Backend type (simple, transactional, etc.)
        config.yaml: <cfg>   Backend-specific configuration
    
    frontends/      - Service interfaces and mediators  
      <name>:         Frontend instance configuration
        type: <type>    Frontend type (http3_server, yaml_mediator, etc.)
        config.yaml: <cfg>   Frontend-specific configuration

  Key Concepts:
  - Backends handle data storage, networking, and business logic
  - Frontends provide interfaces, synchronization, and protocol handling
  - Dependencies are resolved automatically through topological sorting
  - Configuration supports both simple values and complex nested structures
  - For backends other than http3_server, the explicit config tag is optional, and
    the options can simply be specified after the type.
  - backends, frontends, and http3_server and routes and paths must explicitly list child names
    in their configuration to define the hierarchy (to distinguish from other possibly 
    nested properties such as the config.yaml).

  Example configuration:
    config:
      backends:
        child_names: [file_store]
        file_store:
          type: file
          base_path: ~/data/root_node/
      frontends:
        child_names: [http3_server]
        http3_server:
          child_names: [root_node]
          config.yaml:
            type: http3_server
            name: localhost
            ip: 127.0.0.1
            port: 12345
            private_key_file: ../test_instances/data/private_key.pem
            cert_file: ../test_instances/data/cert.pem
            log_path: ../test_instances/sandbox/
            static_load: true
          root_node:
            child_names: [wwatp]
            wwatp:
              backend: file_store
          index.html: ../test_instances/data/libtest_index.html
)";

const std::string WWATPService::backends_help = R"(
Backends Configuration:
  Configure data storage and management backends under config/backends/.
  Each backend has a unique name and type with specific configuration options.
  
  Supported backend types:
  - simple: In-memory storage with basic operations
  - transactional: Supports atomic operations with rollback
  - threadsafe: Thread-safe wrapper around another backend
  - composite: Combines multiple backends with routing logic
  - redirected: Redirects operations to another backend
  - http3_client: Network client for remote WWATP services
  - file: File system storage backend
)";

const std::string WWATPService::frontends_help = R"(
Frontends Configuration:
  Configure service frontends under config/frontends/.
  Frontends provide interfaces for accessing and synchronizing backends.
  
  Supported frontend types:
  - cloning_mediator: Synchronizes data between two backends
  - yaml_mediator: Converts between YAML and tree structures
  - http3_server: HTTP/3 server for network access
)";

const std::string WWATPService::simple_help = R"(
Simple Backend Configuration:
  type: simple
  
  A basic in-memory backend that stores tree nodes with no persistence.
  Suitable for temporary data or testing scenarios.
  
  Properties: None required
  
  Example:
    backends:
      memory_store:
        type: simple
)";

const std::string WWATPService::transactional_help = R"(
Transactional Backend Configuration:
  type: transactional
  config.yaml:
    backend: <target_backend_name>
  
  Wraps another backend with transaction support, enabling atomic operations
  and rollback capabilities for complex multi-node operations.
  
  Properties:
    backend: Name of the underlying backend to wrap
  
  Example:
    backends:
      transactional_store:
        type: transactional
        config.yaml:
          backend: memory_store
)";

const std::string WWATPService::threadsafe_help = R"(
Threadsafe Backend Configuration:
  type: threadsafe
  config.yaml:
    backend: <target_backend_name>
  
  Provides thread-safe access to another backend using mutex locking.
  Essential for multi-threaded applications accessing shared backends.
  
  Properties:
    backend: Name of the underlying backend to wrap
  
  Example:
    backends:
      safe_store:
        type: threadsafe
        config.yaml:
          backend: memory_store
)";

const std::string WWATPService::composite_help = R"(
Composite Backend Configuration:
  type: composite
  config.yaml:
    backend: <root_backend_name>
    child_names: [<child_backend_name_1>, <child_backend_name_2>, ...]
    <child_backend_name_1>:
      mount_path: <mount_path_1>
    <child_backend_name_2>:
      mount_path: <mount_path_2>
  
  Grafts multiple backends onto the root backend "tree". Mounting does not
  affect the root backend, but modifications may if they are not directed to the child backends.
  
  Properties:
    backends: List of backend names to combine
  
  Example:
    backends:
      child_nodes: [composite_tree, data_backend, store1, store2]
      composite_tree:
        type: composite
        config.yaml:
          backend: data_backend
          child_names: [store1, store2]
          store1:
            mount_path: /data/store1
          store2:
            mount_path: /data/store2
      data_backend:
        type: simple
      store1:
        type: simple
      store2:
        type: simple    
)";

const std::string WWATPService::redirected_help = R"(
Redirected Backend Configuration:
  type: redirected
  config.yaml:
    backend: <target_backend_name>
    prefix: <path_prefix>
  
  Redirects all operations to a child branch of the backend tree.
  
  Properties:
    backend: Name of the target backend
    prefix: Branch prefix to treat as the root for this backend
  
  Example:
    backends:
      redirected_store:
        type: redirected
        config.yaml:
          backend: main_store
          prefix: /virtual/
)";

const std::string WWATPService::http3_client_help = R"(
HTTP3 Client Backend Configuration:
  type: http3_client
  config.yaml:
    name: <connection_name>
    ip: <ip_address>
    port: <port_number>
    private_key_file: <path_to_private_key>
    cert_file: <path_to_certificate>
    log_path: <log_directory>
    backend: <local_backend_name>
    path: <remote_path>
    blocking_mode: <true/false>
    journal_requests_per_minute: <rate_limit>
    scheme: <http/https>
    authority: <host:port>
    method: <HTTP_method>
    priority: <request_priority>
    increment: <priority_increment>
    static_node_label: <node_name.type>
    static_node_description: <description>
  
  Connects to a remote WWATP service over HTTP/3 for distributed operations.
  Can operate in blocking or non-blocking mode. Journal requests per minute
  defines how often the client will poll the server for updates.

  Note that any configuration settings intended for the QuicListener should be
  specified as well in this config.yaml; see listener_help for details.
  
  Required Properties:
    name: Connection identifier name
    ip: Remote server IP address
    port: Remote server port number
    backend: Local backend to use for caching/storage
    path: Remote path to access on the server
    private_key_file: Path to TLS private key (for client cert auth)
    cert_file: Path to TLS certificate (for client cert auth)
    log_path: Directory for connection logs
  
  Optional Properties:
    blocking_mode: Enable blocking request mode (default: false)
    journal_requests_per_minute: Rate limit for requests (default: 0)
    scheme: HTTP scheme (default: "https")
    authority: Override authority header (default: ip:port)
    method: HTTP method (default: "GET")
    priority: Request priority level (default: 0)
    increment: Priority increment (default: 0)
    static_node_label: Static content label as "name.type"
    static_node_description: Description for static content
  
  Example of monitoring WWATP client backend:
    backends:
      child_names: [local_cache, remote_wwatp]
      local_cache:
        type: simple
      remote_wwatp:
        type: http3_client
        config.yaml:
          name: production_server
          ip: 192.168.1.100
          port: 8443
          backend: local_cache
          path: /api/wwatp/
          blocking_mode: false
  
  Example of direct static asset fetching from a remote server:
    backends:
      child_names: [static_cache, html_client]
      static_cache:
        type: simple
      html_client:
        type: http3_client
        config.yaml:
          name: web_server
          ip: 127.0.0.1
          port: 12345
          backend: static_cache
          path: /index.html
          static_node_label: index.html
          static_node_description: Static HTML file
          blocking_mode: true
          priority: 1
)";

const std::string WWATPService::file_help = R"(
File Backend Configuration:
  type: file
  config.yaml:
    base_path: <directory_path>
  
  Stores tree nodes as files in the filesystem with configurable synchronization.
  
  Properties:
    base_path: Root directory for file storage
  
  Example:
    backends:
      file_store:
        type: file
        config.yaml:
          base_path: /var/lib/wwatp/data
)";

const std::string WWATPService::cloning_mediator_help = R"(
Cloning Mediator Frontend Configuration:
  type: cloning_mediator
  config.yaml:
    backend_a: <backend_name>
    backend_b: <backend_name>
    versioned: <true/false>
  
  Synchronizes data between two backends, maintaining consistency through
  real-time replication.  Uses notifications. If versioned is true,
  attempted updates to older versions will be ignored.

  Properties:
    backend_a: A backend name
    backend_b: Another backend name
    versioned: If true, updates to older versions will be ignored
  
  Example:
    backends:
      child_names: [main_store, backup_store]
      main_store:
        type: simple
      backup_store:
        type: simple
    frontends:
      backup_sync:
        type: cloning_mediator
        config.yaml:
          backend_a: main_store
          backend_b: backup_store
          versioned: true
)";

const std::string WWATPService::yaml_mediator_help = R"(
YAML Mediator Frontend Configuration:
  type: yaml_mediator
  config.yaml:
    tree_backend: <tree_backend_name>
    yaml_backend: <yaml_backend_name>
    node_label: <node_label>
    property_name: <property_name>
    property_type: <property_type>
    initialize_from_yaml: <true/false>
  
  Converts between YAML configuration and tree node structures, enabling
  human-readable configuration that maps to internal tree representation.
  Note that the YAML backend is still technically a tree, but one where the 
  tree_backend has been marshalled to the property called property_name
  at the node_label in the YAML backend.
  
  Properties:
    tree_backend: Backend storing tree structure
    yaml_backend: Backend storing YAML data
    node_label: Label of the tree node to store the YAML representation of the tree backend
    property_name: Specifies which property to store the YAML representation in
    property_type: Type of the YAML property (default: "yaml")
    initialize_from_yaml: If true, load tree from YAML on startup, if false, load YAML from tree on startup
  
  Example:
    frontends:
      child_names: [config_converter]
      config_converter:
        type: yaml_mediator
        config.yaml:
          tree_backend: config_tree
          yaml_backend: config_files
          node_label: zoo
          property_name: config
          initialize_from_yaml: true
)";

const std::string WWATPService::http3_server_help = R"(
HTTP3 Server Frontend Configuration:
  config.yaml:
    type: http3_server
    name: <server_name>
    ip: <bind_address>
    port: <port_number>
    private_key_file: <path_to_private_key>
    cert_file: <path_to_certificate>
    log_path: <log_directory>
    static_load: <true/false>
  child_names: [<route_name>, ...]
  <route_name>:
    child_names: [<subroute>, ...]
    <subroute>:
      backend: <backend_name>
      journal_size: <journal_size>
    <subroute_static_file>.<type>: <file_path_or_content>
  <static_file>.<type>: <file_path_or_content>
    
  
  Provides HTTP/3 network access with support for both WWATP backend routes
  and static file serving. Routes ending with "/wwatp" become WWATP API endpoints.
  
  Required Properties:
    name: Server identifier name
    ip: Address to bind server (e.g., 127.0.0.1, 0.0.0.0)
    port: Port number to listen on
    private_key_file: Path to TLS private key file
    cert_file: Path to TLS certificate file
    log_path: Directory for server logs
  
  Optional Properties:
    static_load: Load static files from filesystem (default: false)

  Additional properties intended to configure the QuicListener can be added to the config as well:
    quiet: Suppress debug output (default: true)
    send_trailers: Send HTTP trailers (default: false)
    timeout: Connection timeout in seconds (default: 30)
    show_secret: Log TLS secrets for debugging (default: false)
    validate_addr: Enable address validation (default: false)
    verify_client: Require client certificate verification (default: false)
    max_streams_bidi: Maximum bidirectional streams (default: system)
    max_streams_uni: Maximum unidirectional streams (default: system)
  
  Configuration Notes:
    child_names: List of route path segments
    Routes with "wwatp" child become WWATP API endpoints
    Static assets: Use property syntax like "index.html" or "style.css" and are not listed in child_names
    If static_load is true, the static asset values are taken to be file names and loaded from the filesystem.
    Unlike other frontends and backends, the config.yaml property is required, and no other properties
    can be specified (since they are interpreted as static assets).

  Example with WWATP routes and static files:
    frontends:
      child_names: [web_server]
      web_server:
        config.yaml:
          type: http3_server
          name: localhost
          ip: 127.0.0.1
          port: 8443
          private_key_file: /etc/ssl/private/server.key
          cert_file: /etc/ssl/certs/server.crt
          log_path: /var/log/wwatp/
          static_load: true
          quiet: false
        child_names: [api, admin]
        api:
          child_names: [wwatp]
          wwatp:
            backend: main_store
            journal_size: 1000
        admin:
          child_names: [wwatp]
          wwatp:
            backend: admin_store
        index.html: /var/www/index.html
        style.css: /var/www/style.css
)";

