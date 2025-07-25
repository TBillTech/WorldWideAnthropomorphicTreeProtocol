#include "wwatp_service.h"

#include <algorithm>
#include <set>
#include <queue>
#include <stdexcept>
#include <iostream>
#include <tuple>
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

WWATPService::WWATPService(string name, std::shared_ptr<Backend> config_backend, const std::string& config_label)
    : name_(name), config_backend_(config_backend), config_label_(config_label) {
    if (!config_backend_) {
        throw std::invalid_argument("Config backend cannot be null");
    }
    if (config_label_.empty()) {
        throw std::invalid_argument("Config label cannot be empty");
    }
}

WWATPService::~WWATPService() {
    // Cleanup happens automatically with shared_ptr and unique_ptr
}

void WWATPService::initialize() {
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

        initialized_ = true;
        std::cout << "WWATPService initialized successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize WWATPService: " << e.what() << std::endl;
        throw;
    }
}

std::shared_ptr<Backend> WWATPService::getBackend(const std::string& backend_name) const {
    auto it = backends_.find(backend_name);
    return (it != backends_.end()) ? it->second : nullptr;
}

std::vector<std::string> WWATPService::getBackendNames() const {
    std::vector<std::string> names;
    names.reserve(backends_.size());
    for (const auto& [name, backend] : backends_) {
        names.push_back(name);
    }
    return names;
}

bool WWATPService::hasBackend(const std::string& backend_name) const {
    return backends_.find(backend_name) != backends_.end();
}

QuicListener& WWATPService::getQuicListener(const std::string& server_name) {
    auto it = quic_listeners_.find(server_name);
    if (it == quic_listeners_.end()) {
        throw std::runtime_error("QuicListener '" + server_name + "' not found");
    }
    return it->second;
}

std::vector<std::string> WWATPService::getQuicListenerNames() const {
    std::vector<std::string> names;
    names.reserve(quic_listeners_.size());
    for (const auto& [name, listener] : quic_listeners_) {
        names.push_back(name);
    }
    return names;
}

bool WWATPService::hasQuicListener(const std::string& server_name) const {
    return quic_listeners_.find(server_name) != quic_listeners_.end();
}

void WWATPService::initializeFactories() {
    // Initialize backend factories
    backend_factories_["simple"] = [this](const TreeNode& config) {
        return this->createSimpleBackend(config);
    };
    
    backend_factories_["transactional"] = [this](const TreeNode& config) {
        return this->createTransactionalBackend(config);
    };
    
    backend_factories_["threadsafe"] = [this](const TreeNode& config) {
        return this->createThreadsafeBackend(config);
    };
    
    backend_factories_["composite"] = [this](const TreeNode& config) {
        return this->createCompositeBackend(config);
    };
    
    backend_factories_["redirected"] = [this](const TreeNode& config) {
        return this->createRedirectedBackend(config);
    };
    
    backend_factories_["http3_client"] = [this](const TreeNode& config) {
        return this->createHttp3ClientBackend(config);
    };
    
    backend_factories_["file"] = [this](const TreeNode& config) {
        return this->createFileBackend(config);
    };

    // Initialize frontend factories
    frontend_factories_["cloning_mediator"] = [this](const TreeNode& config) {
        return this->createCloningMediator(config);
    };
    
    frontend_factories_["yaml_mediator"] = [this](const TreeNode& config) {
        return this->createYAMLMediator(config);
    };
    
    frontend_factories_["http3_server"] = [this](const TreeNode& config) {
        return this->createHTTP3Server(config);
    };
}

void WWATPService::constructBackends() {
    // Get the backends configuration node
    auto config_node = config_backend_->getNode(config_label_);
    if (config_node.is_nothing()) {
        throw std::runtime_error("Configuration node '" + config_label_ + "' not found");
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

    // Construct backends in dependency order
    for (const std::string& backend_name : construction_order) {
        const TreeNode& backend_config = backend_configs[backend_name];
        
        std::string backend_type = getStringProperty(backend_config, "type");
        if (backend_type.empty()) {
            throw std::runtime_error("Backend '" + backend_name + "' missing required 'type' property");
        }

        auto factory_it = backend_factories_.find(backend_type);
        if (factory_it == backend_factories_.end()) {
            throw std::runtime_error("Unknown backend type '" + backend_type + "' for backend '" + backend_name + "'");
        }

        try {
            auto backend = factory_it->second(backend_config);
            backends_[backend_name] = backend;
            std::cout << "Created backend '" << backend_name << "' of type '" << backend_type << "'" << std::endl;
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to create backend '" + backend_name + "': " + e.what());
        }
    }
}

void WWATPService::constructFrontends() {
    auto frontends_node = config_backend_->getNode(config_label_ + "/frontends");
    if (frontends_node.is_nothing()) {
        std::cout << "No frontends configuration found, skipping frontend construction" << std::endl;
        return;
    }

    // Construct each frontend
    for (const auto& frontend_name : frontends_node.lift_def(std::vector<std::string>(), [](auto node){ return node.getChildNames(); })) {
        auto frontend_node = config_backend_->getNode(config_label_ + "/frontends/" + frontend_name);
        if (frontend_node.is_nothing()) {
            continue;
        }

        std::string frontend_type = getStringProperty(frontend_node.unsafe_get_just(), "type");
        if (frontend_type.empty()) {
            throw std::runtime_error("Frontend '" + frontend_name + "' missing required 'type' property");
        }

        auto factory_it = frontend_factories_.find(frontend_type);
        if (factory_it == frontend_factories_.end()) {
            throw std::runtime_error("Unknown frontend type '" + frontend_type + "' for frontend '" + frontend_name + "'");
        }

        try {
            auto frontend = factory_it->second(frontend_node.unsafe_get_just());
            frontends_[frontend_name] = frontend;
            std::cout << "Created frontend '" << frontend_name << "' of type '" << frontend_type << "'" << std::endl;
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to create frontend '" + frontend_name + "': " + e.what());
        }
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
    
    // Calculate in-degrees
    for (const auto& [name, deps] : dependencies) {
        for (const std::string& dep : deps) {
            if (backend_configs.find(dep) != backend_configs.end()) {
                in_degree[name]++;
            }
        }
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
        throw std::runtime_error("Circular dependency detected in backend configuration");
    }
    
    return result;
}

std::vector<std::string> WWATPService::getBackendDependencies(const TreeNode& backend_config) {
    std::vector<std::string> dependencies;
    
    // Check common dependency property names
    std::vector<std::string> dep_properties = {"backend", "underlying_backend", "base_backend", "parent_backend"};
    
    for (const std::string& prop : dep_properties) {
        std::string dep = getStringProperty(backend_config, prop);
        if (!dep.empty()) {
            dependencies.push_back(dep);
        }
    }
    
    return dependencies;
}

// Helper methods for property extraction
std::string WWATPService::getStringProperty(const TreeNode& node, const std::string& property_name, 
                                           const std::string& default_value) const {
    try {
        auto string_data = node.getPropertyValueSpan(property_name);
        string result(get<1>(string_data).begin<const char>(), get<1>(string_data).end<const char>());
        return result.empty() ? default_value : result;
    } catch (const std::exception&) {
        return default_value; // Return default if property not found
    }
}

int64_t WWATPService::getInt64Property(const TreeNode& node, const std::string& property_name, 
                                int64_t default_value) const {
    try {
        return get<1>(node.getPropertyValue<int64_t>(property_name));
    } catch (const std::exception&) {
        return default_value; // Return default if property not found
    }
}

uint64_t WWATPService::getUint64Property(const TreeNode& node, const std::string& property_name, 
                                uint64_t default_value) const {
    try {
        return get<1>(node.getPropertyValue<uint64_t>(property_name));
    } catch (const std::exception&) {
        return default_value; // Return default if property not found
    }
}

bool WWATPService::getBoolProperty(const TreeNode& node, const std::string& property_name, 
                                  bool default_value) const {
    try {
        return get<1>(node.getPropertyValue<bool>(property_name));
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
    std::string underlying_backend_name = getStringProperty(config, "backend");
    if (underlying_backend_name.empty()) {
        throw std::runtime_error("TransactionalBackend requires 'backend' property");
    }
    
    auto underlying_backend = getBackend(underlying_backend_name);
    if (!underlying_backend) {
        throw std::runtime_error("Underlying backend '" + underlying_backend_name + "' not found");
    }
    
    return std::make_shared<TransactionalBackend>(*underlying_backend);
}

std::shared_ptr<Backend> WWATPService::createThreadsafeBackend(const TreeNode& config) {
    std::string underlying_backend_name = getStringProperty(config, "backend");
    if (underlying_backend_name.empty()) {
        throw std::runtime_error("ThreadsafeBackend requires 'backend' property");
    }
    
    auto underlying_backend = getBackend(underlying_backend_name);
    if (!underlying_backend) {
        throw std::runtime_error("Underlying backend '" + underlying_backend_name + "' not found");
    }
    
    return std::make_shared<ThreadsafeBackend>(*underlying_backend);
}

std::shared_ptr<Backend> WWATPService::createCompositeBackend(const TreeNode& config) {
    std::string root_backend_name = getStringProperty(config, "root_backend");
    if (root_backend_name.empty()) {
        throw std::runtime_error("CompositeBackend requires 'root_backend' property");
    }
    
    auto root_backend = getBackend(root_backend_name);
    if (!root_backend) {
        throw std::runtime_error("Root backend '" + root_backend_name + "' not found");
    }
    
    auto composite = std::make_shared<CompositeBackend>(*root_backend);

    // Iterate through the children nodes, which will have names == backend name, and a string property called "mount_path"
    for (const auto& child_name : config.getChildNames()) {
        auto child_label = config.getLabelRule() + "/" + child_name;
        auto child_node = config_backend_->getNode(child_name);
        if (child_node.is_nothing()) {
            std::cerr << "Warning: Child backend configuration '" << child_name << "' not found" << std::endl;
            continue;
        }
        std::string mount_path = getStringProperty(child_node.unsafe_get_just(), "mount_path");
        if (mount_path.empty()) {
            std::cerr << "Warning: Child backend '" << child_name << "' missing 'mount_path' property" << std::endl;
            continue;   
        }
        auto backend_name = child_node.unsafe_get_just().getNodeName();
        auto child_backend = getBackend(child_name);
        if (!child_backend) {
            std::cerr << "Warning: Child backend '" << child_name << "' not found" << std::endl;
            continue;
        }
        if (!composite->mountBackend(mount_path, *child_backend).second) {
            std::cerr << "Warning: Failed to add child backend '" << child_name << "' to composite backend" << std::endl;
        } else {
            std::cout << "Added child backend '" << child_name << "' at mount path '" << mount_path << "'" << std::endl;
        }
    }
    
    return composite;
}

std::shared_ptr<Backend> WWATPService::createRedirectedBackend(const TreeNode& config) {
    std::string underlying_backend_name = getStringProperty(config, "backend");
    if (underlying_backend_name.empty()) {
        throw std::runtime_error("RedirectedBackend requires 'backend' property");
    }
    
    auto underlying_backend = getBackend(underlying_backend_name);
    if (!underlying_backend) {
        throw std::runtime_error("Underlying backend '" + underlying_backend_name + "' not found");
    }
    
    std::string redirect_root = getStringProperty(config, "redirect_root", "");
    return std::make_shared<RedirectedBackend>(*underlying_backend, redirect_root);
}

std::shared_ptr<Backend> WWATPService::createHttp3ClientBackend(const TreeNode& config) {
    string server_url = getStringProperty(config, "server_url");
    if (server_url.empty()) {
        throw std::runtime_error("HTTP3ClientBackend requires 'server_url' property");
    }

    string backend_name = getStringProperty(config, "local_backend", "http3_client_cache");
    auto local_backend = getBackend(backend_name);
    if (!local_backend) {
        throw std::runtime_error("Backend '" + backend_name + "' not found for HTTP3ClientBackend");
    }
    bool blocking_mode = getBoolProperty(config, "blocking_mode", true);
    size_t journalRequestsPerMinute = getUint64Property(config, "journal_requests_per_minute", 60);
    Request request;
    request.scheme = getStringProperty(config, "scheme", "https"); // Assuming HTTPS for HTTP3
    string ip = getStringProperty(config, "ip", "127.0.0.1");
    string port = getStringProperty(config, "port", "443");
    request.authority = getStringProperty(config, "authority", ip + ":" + port);
    request.path = getStringProperty(config, "path", "/wwatp/api");
    request.method = getStringProperty(config, "method", "GET");
    request.pri.urgency = static_cast<int32_t>(getInt64Property(config, "priority", 0));
    request.pri.inc = getInt64Property(config, "increment", 0);
    maybe<TreeNode> staticNode;
    Http3ClientBackend& http3_client_backend = http3_client_updater_.addBackend(*local_backend, blocking_mode, request, journalRequestsPerMinute, staticNode);
    // Use shared_ptr with custom deleter that doesn't delete - the updater owns the object
    return shared_ptr<Http3ClientBackend>(&http3_client_backend, [](Http3ClientBackend*){});
}

std::shared_ptr<Backend> WWATPService::createFileBackend(const TreeNode& config) {
    std::string root_path = getStringProperty(config, "root_path");
    if (root_path.empty()) {
        throw std::runtime_error("FileBackend requires 'root_path' property");
    }
    
    return std::make_shared<FileBackend>(root_path);
}

// Frontend creation methods
std::shared_ptr<Frontend> WWATPService::createCloningMediator(const TreeNode& config) {
    std::string name = getStringProperty(config, "name");
    if (name.empty()) {
        throw std::runtime_error("CloningMediator requires 'name' property");
    }
    
    std::string backend_a_name = getStringProperty(config, "backend_a");
    std::string backend_b_name = getStringProperty(config, "backend_b");
    
    if (backend_a_name.empty() || backend_b_name.empty()) {
        throw std::runtime_error("CloningMediator requires 'backend_a' and 'backend_b' properties");
    }
    
    auto backend_a = getBackend(backend_a_name);
    auto backend_b = getBackend(backend_b_name);
    
    if (!backend_a || !backend_b) {
        throw std::runtime_error("Required backends not found for CloningMediator '" + name + "'");
    }
    
    bool versioned = getBoolProperty(config, "versioned", true);
    
    return std::make_shared<CloningMediator>(name, *backend_a, *backend_b, versioned);
}

std::shared_ptr<Frontend> WWATPService::createYAMLMediator(const TreeNode& config) {
    std::string name = getStringProperty(config, "name");
    if (name.empty()) {
        throw std::runtime_error("YAMLMediator requires 'name' property");
    }
    
    std::string tree_backend_name = getStringProperty(config, "tree_backend");
    std::string yaml_backend_name = getStringProperty(config, "yaml_backend");
    
    if (tree_backend_name.empty() || yaml_backend_name.empty()) {
        throw std::runtime_error("YAMLMediator requires 'tree_backend' and 'yaml_backend' properties");
    }
    
    auto tree_backend = getBackend(tree_backend_name);
    auto yaml_backend = getBackend(yaml_backend_name);
    
    if (!tree_backend || !yaml_backend) {
        throw std::runtime_error("Required backends not found for YAMLMediator '" + name + "'");
    }
    
    // PropertySpecifier configuration
    std::string node_label = getStringProperty(config, "node_label", "");
    std::string property_name = getStringProperty(config, "property_name", "yaml");
    std::string property_type = getStringProperty(config, "property_type", "string");
    
    PropertySpecifier specifier(node_label, property_name, property_type);
    
    bool initialize_from_yaml = getBoolProperty(config, "initialize_from_yaml", true);
    
    return std::make_shared<YAMLMediator>(name, *tree_backend, *yaml_backend, specifier, initialize_from_yaml);
}

std::shared_ptr<Frontend> WWATPService::createHTTP3Server(const TreeNode& config) {
    std::string name = getStringProperty(config, "name");
    if (name.empty()) {
        name = config.getNodeName();
    }
    
    // The const TreeNode& config _IS_ the Server Config Node as described in server/REQUIREMENTS.md
    // So first we need to get the <node>.0.config.yaml property:
    auto config_property = config.getPropertyValueSpan("config");
    auto yaml_span = get<2>(config_property);
    if (yaml_span.size() == 0) {
        throw std::runtime_error("HTTP3Server requires 'config' property");
    }
    string yaml_string(yaml_span.begin<const char>(), yaml_span.end<const char>());
    // Load the YAML configuration from the yaml_string
    auto yaml_config = YAML::Load(yaml_string);
    // Next get the name and port from the yaml_config
    std::string server_name = yaml_config["name"].as<std::string>(name);
    int port = yaml_config["port"].as<int>(8443); // Default to 8443

    // Create HTTP3Server with empty static assets
    std::map<std::string, chunks> static_assets;
    auto server = std::make_shared<HTTP3Server>(name, std::move(static_assets));
    
    
    return server;
}

void WWATPService::createQuicListener(const std::string& server_name, const YAML::Node& config) {
    // Store in the map - the QuicListener constructor now handles config validation
    quic_listeners_.emplace(std::piecewise_construct,
                           std::forward_as_tuple(server_name),
                           std::forward_as_tuple(io_context_, config));
    
    // Extract configuration parameters for logging
    std::string private_key_file = config["private_key_file"].as<std::string>("");
    std::string cert_file = config["cert_file"].as<std::string>("");

    std::cout << "Created QuicListener '" << server_name << "' with private_key: " << private_key_file
              << ", cert: " << cert_file << std::endl;
}

