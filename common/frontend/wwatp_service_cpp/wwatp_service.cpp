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
            std::cout << "Created backend '" << backend_name << "' of type '" << backend_type << "'" << endl << flush;
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
            std::cout << "Created frontend '" << frontend_name << "' of type '" << frontend_type << "'" << endl << flush;
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
    std::vector<std::string> dep_properties = {"backend"};
    
    for (const std::string& prop : dep_properties) {
        std::string dep = getStringProperty(backend_config, prop);
        if (!dep.empty()) {
            dependencies.push_back(dep);
        }
    }
    
    // Special handling for CompositeBackend - it also depends on all child backends it needs to mount
    std::string backend_type = getStringProperty(backend_config, "type");
    if (backend_type == "composite") {
        // Add all child backends as dependencies
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

    auto underlying_backend = backends_.find(underlying_backend_name);
    if (underlying_backend == backends_.end()) {
        throw std::runtime_error("Underlying backend '" + underlying_backend_name + "' not found");
    }
    
    return std::make_shared<TransactionalBackend>(*underlying_backend->second);
}

std::shared_ptr<Backend> WWATPService::createThreadsafeBackend(const TreeNode& config) {
    std::string underlying_backend_name = getStringProperty(config, "backend");
    if (underlying_backend_name.empty()) {
        throw std::runtime_error("ThreadsafeBackend requires 'backend' property");
    }

    auto underlying_backend = backends_.find(underlying_backend_name);
    if (underlying_backend == backends_.end()) {
        throw std::runtime_error("Underlying backend '" + underlying_backend_name + "' not found");
    }
    
    return std::make_shared<ThreadsafeBackend>(*underlying_backend->second);
}

std::shared_ptr<Backend> WWATPService::createCompositeBackend(const TreeNode& config) {
    std::string root_backend_name = getStringProperty(config, "backend");
    if (root_backend_name.empty()) {
        throw std::runtime_error("CompositeBackend requires 'backend' property");
    }

    auto root_backend = backends_.find(root_backend_name);
    if (root_backend == backends_.end()) {
        throw std::runtime_error("Root backend '" + root_backend_name + "' not found");
    }

    auto composite = std::make_shared<CompositeBackend>(*root_backend->second);

    // Iterate through the children nodes, which will have names == backend name, and a string property called "mount_path"
    for (const auto& child_name : config.getChildNames()) {
        auto child_label = config.getLabelRule() + "/" + child_name;
        auto child_node = config_backend_->getNode(child_label);
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
        auto child_backend = backends_.find(child_name);
        if (child_backend == backends_.end()) {
            std::cerr << "Warning: Child backend '" << child_name << "' not found" << std::endl;
            continue;
        }
        if (!composite->mountBackend(mount_path, *(child_backend->second)).second) {
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

    auto underlying_backend = backends_.find(underlying_backend_name);
    if (underlying_backend == backends_.end()) {
        throw std::runtime_error("Underlying backend '" + underlying_backend_name + "' not found");
    }
    
    std::string redirect_root = getStringProperty(config, "redirect_root", "");
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
        throw std::runtime_error("Failed to create QuicConnector for specifier: " + get<1>(conspec) + ":" + to_string(get<2>(conspec)));
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
    auto [unused1, unused2, yaml_span] = config.getPropertyValueSpan("config");
    if (yaml_span.size() == 0) {
        throw std::runtime_error("HTTP3ClientBackend requires 'config' property with YAML content");
    }
    string yaml_content(yaml_span.begin<const char>(), yaml_span.end<const char>());
    YAML::Node yaml_config = YAML::Load(yaml_content);

    ConnectorSpecifier connspec = getConnectorSpecifier(yaml_config);

    string backend_name = yaml_config["backend"].as<string>("http3_client_cache");
    auto local_backend = backends_.find(backend_name);
    if (local_backend == backends_.end()) {
        throw std::runtime_error("Backend '" + backend_name + "' not found for HTTP3ClientBackend");
    }
    bool blocking_mode = yaml_config["blocking_mode"].as<bool>(true);
    size_t journalRequestsPerMinute = yaml_config["journal_requests_per_minute"].as<size_t>(60);
    Request request;
    request.scheme = yaml_config["scheme"].as<string>("https"); // Assuming HTTPS for HTTP3
    string ip = yaml_config["ip"].as<string>("127.0.0.1");
    string port_str = yaml_config["port"].as<string>("443");
    request.authority = yaml_config["authority"].as<string>(ip + ":" + port_str);
    request.path = yaml_config["path"].as<string>("/wwatp/api");
    request.method = yaml_config["method"].as<string>("GET");
    request.pri.urgency = static_cast<int32_t>(yaml_config["priority"].as<int64_t>(0));
    request.pri.inc = yaml_config["increment"].as<int64_t>(0);
    maybe<TreeNode> staticNode;
    Http3ClientBackendUpdater& http3_client_updater = obtainHttp3ClientUpdater(yaml_config);
    Http3ClientBackend& http3_client_backend = http3_client_updater.addBackend(*(local_backend->second), blocking_mode, request, journalRequestsPerMinute, staticNode);
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
pair<shared_ptr<Frontend>, WWATPService::ConnectorSpecifier> WWATPService::createCloningMediator(const TreeNode& config) {
    std::string name = getStringProperty(config, "name");
    if (name.empty()) {
        name = config.getNodeName();
    }
    
    std::string backend_a_name = getStringProperty(config, "backend_a");
    std::string backend_b_name = getStringProperty(config, "backend_b");
    
    if (backend_a_name.empty() || backend_b_name.empty()) {
        throw std::runtime_error("CloningMediator requires 'backend_a' and 'backend_b' properties");
    }

    auto backend_a = backends_.find(backend_a_name);
    auto backend_b = backends_.find(backend_b_name);

    if (backend_a == backends_.end() || backend_b == backends_.end()) {
        throw std::runtime_error("Required backends not found for CloningMediator '" + name + "'");
    }
    
    bool versioned = getBoolProperty(config, "versioned", false);

    return {make_shared<CloningMediator>(name, *(backend_a->second), *(backend_b->second), versioned),
            tuple<string, string, uint16_t>(name, "", 0)}; // Dummy connector specifier
}

pair<shared_ptr<Frontend>, WWATPService::ConnectorSpecifier> WWATPService::createYAMLMediator(const TreeNode& config) {
    std::string name = getStringProperty(config, "name");
    if (name.empty()) {
        name = config.getNodeName();
    }
    
    std::string tree_backend_name = getStringProperty(config, "tree_backend");
    std::string yaml_backend_name = getStringProperty(config, "yaml_backend");
    
    if (tree_backend_name.empty() || yaml_backend_name.empty()) {
        throw std::runtime_error("YAMLMediator requires 'tree_backend' and 'yaml_backend' properties");
    }
    
    auto tree_backend = backends_.find(tree_backend_name);
    auto yaml_backend = backends_.find(yaml_backend_name);

    if (tree_backend == backends_.end() || yaml_backend == backends_.end()) {
        throw std::runtime_error("Required backends not found for YAMLMediator '" + name + "'");
    }
    
    // PropertySpecifier configuration
    std::string node_label = getStringProperty(config, "node_label", "");
    std::string property_name = getStringProperty(config, "property_name", "yaml");
    std::string property_type = getStringProperty(config, "property_type", "string");
    
    PropertySpecifier specifier(node_label, property_name, property_type);
    
    bool initialize_from_yaml = getBoolProperty(config, "initialize_from_yaml", true);

    return {make_shared<YAMLMediator>(name, *(tree_backend->second), *(yaml_backend->second), specifier, initialize_from_yaml),
            tuple<string, string, uint16_t>(name, "", 0)}; // Dummy connector specifier
}

void WWATPService::updateServerWithChild( 
    std::shared_ptr<HTTP3Server> server, const std::string& child_path, 
    const TreeNode& child_node, bool is_wwatp_route) {
    if (is_wwatp_route) {
        string backend_name = getStringProperty(child_node, "backend", "");
        if (backend_name.empty()) {
            throw std::runtime_error("WWATP route requires 'backend' property");
        }
        size_t journal_size = getUint64Property(child_node, "journal_size", 1000);
        auto find_backend = backends_.find(backend_name);
        if (find_backend == backends_.end()) {
            throw std::runtime_error("Backend not found: " + backend_name);
        }
        server->addBackendRoute(*find_backend->second, journal_size, child_path);
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
    for (const auto& child_name : children_names) {
        auto child_node_label = child_node.getLabelRule() + "/" + child_name;
        if (child_name.find("/") != std::string::npos) {
            child_node_label = child_name;
        }
        auto child_child_node = config_backend_->getNode(child_name);
        if (child_child_node.is_nothing()) {
            std::cerr << "Warning: Child node '" << child_name << "' not found in HTTP3Server configuration" << std::endl;
            continue;
        }
        auto child_child_path = child_path + "/" + child_child_node.unsafe_get_just().getNodeName();
        updateServerWithChild(server, child_child_path, child_child_node.unsafe_get_just(), 
            is_wwatp_route || child_child_path == "/wwatp");
    }
}

pair<shared_ptr<Frontend>, WWATPService::ConnectorSpecifier> WWATPService::createHTTP3Server(const TreeNode& config) {
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
    YAML::Node yaml_config = YAML::Load(yaml_string);
    ConnectorSpecifier connspec = getConnectorSpecifier(yaml_config);
    // Next get the name and port from the yaml_config
    std::string server_name = yaml_config["name"].as<std::string>(name);
    QuicListener& listener = obtainQuicListener(yaml_config);

    // Create HTTP3Server with empty static assets
    auto server = std::make_shared<HTTP3Server>(name);

    // Now loop through all of the properties in the TreeNode config, and create static assets or wwatp backend routes as configured
    for (const auto& [type, name] : config.getPropertyInfo()) {
        auto static_name = name + "." + type;
        auto url = "/" + static_name;
        auto shared_span = config.getPropertyValueSpan(name);
        chunks asset = {get<2>(shared_span)};
        server->addStaticAsset(url, asset);
    }

    auto children_names = config.getChildNames();
    for (const auto& child_name : children_names) {
        auto child_node = config_backend_->getNode(child_name);
        if (child_node.is_nothing()) {
            std::cerr << "Warning: Child node '" << child_name << "' not found in HTTP3Server configuration" << std::endl;
            continue;
        }
        auto child_path = "/" + child_node.unsafe_get_just().getNodeName();
        updateServerWithChild(server, child_path, child_node.unsafe_get_just(), child_path == "/wwatp");
    }

    prepare_stream_callback_fn theServerHandlerWrapper = [&server](const Request &req) {
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
        std::cerr << "QuicListener " << connspec << " already exists, returning existing instance with private key: " 
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
    // Loop through the quic_listeners_ and start each listener
    for (auto& [conspec, listener] : quic_listeners_) {
        listener.listen(get<0>(conspec), get<1>(conspec), get<2>(conspec));
    }
    // Loop through the quic_connectors_ and start each connector
    for (auto& [conspec, connector] : quic_connectors_) {
        connector.connect(get<0>(conspec), get<1>(conspec), get<2>(conspec));
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
    // loop through the quic_connectors_ and stop each connector
    for (auto& [__, connector] : quic_connectors_) {
        connector.close();
    }
    // loop through the quic_listeners_ and stop each listener
    for (auto& [__, listener] : quic_listeners_) {
        listener.close();
    }
    std::cout << "WWATPService stopped successfully" << std::endl;
    running_ = false;
}

bool WWATPService::isRunning() const {
    return running_;
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
    
    // Create YAMLMediator to convert YAML to tree structure
    // initialize_from_yaml = true means read from config_yaml_backend and populate config_backend
    YAMLMediator yaml_mediator("config_mediator", *config_backend, config_yaml_backend, specifier, true);
    
    return config_backend;
}

