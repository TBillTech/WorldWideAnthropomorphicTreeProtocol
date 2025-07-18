#include "yaml_mediator.h"
#include "tree_node.h"
#include "simple_backend.h"

using namespace std;
using namespace fplus;

static MemoryTree global_empty_memory_tree;
static SimpleBackend global_null_backend(global_empty_memory_tree); 

vector<string> split(const string& s, char delimiter) {
    vector<string> tokens;
    stringstream ss(s);
    string item;
    while (getline(ss, item, delimiter)) {
        tokens.push_back(item);
    }
    return tokens;
}

void updateYAMLFrom(YAML::Node yaml, string nodeLabel, const maybe<TreeNode>& m_from, Backend &from, bool loadChildren) {
    // Update the YAML node with the data from the TreeNode
    // First, get the chain of parent/child names from the label_rule by splitting it by the '/' character.
    vector<string> parts = nodeLabel.empty() ? vector<string>{} : split(nodeLabel, '/');
    // Since you can't have an empty label rule in a node, parts has to always contain at least one element.
    if (yaml.IsNull()) {
        if (m_from.is_nothing()) {
            return; // If the YAML node is null and m_from is nothing, do nothing
        }
        yaml = YAML::Node();
    }
    YAML::Node curNode = yaml;
    for (size_t i = 0; i < parts.size(); ++i) {
        YAML::Node parentNode = curNode;
        curNode = parentNode[parts[i]];
        if (i == parts.size() - 1) 
        {   
            if (m_from.is_just()) {
                parentNode[parts[i]] = m_from.unsafe_get_just().asYAMLNode(from, loadChildren);
            } else {
                if (!curNode.IsNull()) {
                    // If the node does not exist, remove it from YAML
                    parentNode.remove(parts[i]);
                }
            }
        }
        else {
            if (curNode.IsNull()) {
                if (m_from.is_nothing())
                    break;
                parentNode[parts[i]] = YAML::Node();
                curNode = parentNode[parts[i]];
            }
        }
    }
}

vector<shared_span<>> createSharedSpanFromYAML(const YAML::Node yaml) {
    vector<shared_span<>> data_spans;
    ostringstream ss;
    ss << yaml;
    string yaml_string = ss.str();
    size_t yaml_size = yaml_string.size();
    payload_chunk_header header(0, payload_chunk_header::SIGNAL_OTHER_CHUNK, 8);
    data_spans.emplace_back(header, std::span<uint64_t>(reinterpret_cast<uint64_t*>(&yaml_size), 1));
    payload_chunk_header data_header(0, payload_chunk_header::SIGNAL_OTHER_CHUNK, yaml_size);
    data_spans.emplace_back(data_header, std::span<const char>(yaml_string));
    return data_spans;
}

void toYAMLCallback(Backend& to, Backend& from, YAML::Node yaml, PropertySpecifier const& specifier, atomic<bool>& setProcessing, atomic<bool>& isProcessing, 
        const string& label_rule, const maybe<TreeNode>& m_from) {
    // To YAML callback will do these steps:
    // 1. Update the YAML representation of the node by updating the value of the YAML according to the label rule.
    // 2. Update the YAML property at the PropertySpecifier.
    if (isProcessing.load()) {
        return; // Ignore if already processing
    }
    setProcessing.store(true);
    updateYAMLFrom(yaml, label_rule, m_from, from, true);
    
    vector<shared_span<>> yaml_data = createSharedSpanFromYAML(yaml);
    string node_label = specifier.getNodeLabel();
    auto m_to = to.getNode(node_label);
    if (m_to.is_nothing()) {
        shared_span<> concatted(yaml_data.begin(), yaml_data.end());
        TreeNode yamlNode(node_label, 
            node_label + " YAML Container", 
            { {specifier.getPropertyType(), specifier.getPropertyName()} },
            TreeNodeVersion{0, 256, "default", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>()}, // Default version for YAML nodes
            {},
            move(concatted), 
            maybe<string>(), // No query how-to for YAML nodes
            maybe<string>() // No QA sequence for YAML nodes
        );
        to.upsertNode({yamlNode});
        setProcessing.store(false);
        return;
    } else {
        auto toNode = m_to.unsafe_get_just();
        auto property_infos = toNode.getPropertyInfo();
        TreeNode::PropertyInfo property_info(specifier.getPropertyType(), specifier.getPropertyName());
        auto findit = find(property_infos.begin(), property_infos.end(), property_info);
        shared_span<> value_span = yaml_data[1];
        if (findit != property_infos.end()) {
            toNode.setPropertyValueSpan(specifier.getPropertyName(), move(value_span));
        } else {
            toNode.insertPropertySpan(property_infos.size(), specifier.getPropertyName(), specifier.getPropertyType(), move(value_span));
            property_infos.push_back(property_info);
            toNode.setPropertyInfo(property_infos);
        }
        to.upsertNode({toNode});
    }
    setProcessing.store(false);
}

string getYAMLString(const TreeNode& node, const PropertySpecifier& specifier) {
    auto property_data = node.getPropertyValueSpan(specifier.getPropertyName());
    auto yaml_span = get<2>(property_data);
    string yaml(yaml_span.begin<char>(), yaml_span.end<char>());
    return yaml;
}

bool isNodeModified(const TreeNode& node, const YAML::Node yaml) {
    // First, check if the node label is in yaml
    auto label_rule = node.getLabelRule();
    vector<string> parts = split(label_rule, '/');
    YAML::Node curNode = yaml;
    bool found = false;
    for (const auto& part : parts) {
        if (curNode.IsMap() && curNode[part]) {
            curNode = curNode[part];
        } else {            
            return true; // If any part of the label rule is not found in YAML, the node is deleted and therefore modified
        }
        if (part == parts.back()) {
            found = true; // We found the node in YAML
        }
    }
    if (!found) {
        return true; // If we didn't find the node in YAML, it is considered modified (deleted)
    }
    // Now check each of the properties in the node
    YAML::Node compare_yaml = node.asYAMLNode(global_null_backend, false);
    set<string> all_children;
    for (const auto& item : compare_yaml["childnames"]) {
        all_children.insert(item.first.as<string>());
    }
    for (const auto& item : curNode["child_names"]) {
        all_children.insert(item.first.as<string>());
    }
    set<string> all_names;
    for (const auto& item : curNode) {
        all_names.insert(item.first.as<string>());
    }
    for (const auto& item : compare_yaml) {
        all_names.insert(item.first.as<string>());
    }
    for (const auto& name : all_names) {
        if (all_children.find(name) != all_children.end()) {
            // Check that the name can also be found in curNode
            if (!curNode[name] || !compare_yaml[name]) {
                return true;
            }
            continue;
        }
        if (curNode[name] != compare_yaml[name]) {
            return true; // If any property is different, the node is modified
        }
    }
    return false;
}

set<string> walk_names(string prefix, const YAML::Node node) {
    set<string> names;
    if (node.IsMap()) {
        for (const auto& it : node) {
            string name = prefix.empty() ? it.first.as<string>() : prefix + "/" + it.first.as<string>();
            names.insert(name);
            auto child_names = walk_names(name, it.second);
            names.insert(child_names.begin(), child_names.end());
        }
    } else if (node.IsSequence()) {
        for (size_t i = 0; i < node.size(); ++i) {
            string name = prefix + "[" + to_string(i) + "]";
            names.insert(name);
            auto child_names = walk_names(name, node[i]);
            names.insert(child_names.begin(), child_names.end());
        }
    }
    return names;
}

maybe<TreeNode> getNodeFromYAML(const string& label_rule, const YAML::Node yaml) {
    vector<string> parts = split(label_rule, '/');
    YAML::Node curNode = yaml;
    for (const auto& part : parts) {
        if (curNode.IsMap() && curNode[part]) {
            curNode = curNode[part];
        } else {            
            return nothing<TreeNode>(); // If any part of the label rule is not found in YAML, return nothing
        }
    }
    auto label_prefix = parts.size() > 1 ? label_rule.substr(0, label_rule.rfind('/')) : "";
    return just(fromYAMLNode(curNode, label_prefix, parts.back(), false)[0]);
}

void fromYAMLCallback(Backend& to, YAML::Node yaml, PropertySpecifier const& specifier, atomic<bool>& setProcessing, atomic<bool>& isProcessing, 
        const string& label_rule, const maybe<TreeNode>& m_from) {
    // From YAML callback will do these steps:
    // 1. Reload the YAML representation of the node by using the YAML::Load function to parse the YAML string.
    // 2. Update the to backend based on any changes noticed between the new YAML representation and the old one.
    // 3. Replace the old YAML representation with the new one.
    if (isProcessing.load()) {
        return; // Ignore if already processing
    }
    setProcessing.store(true);
    if (m_from.is_nothing()) {
        throw runtime_error("Cannot update YAML from a non-existing node: " + label_rule);
    }
    TreeNode fromNode = m_from.unsafe_get_just();
    vector<TreeNode> nodes = to.getFullTree();
    // Create a set of node_labels for detecting created nodes
    yaml = YAML::Load(getYAMLString(fromNode, specifier));
    auto walked_names = walk_names("", yaml);
    set<string> created_nodes(walked_names.begin(), walked_names.end());
    vector<TreeNode> modifiedNodes;
    vector<TreeNode> deletedNodes;
    for (auto node : nodes) {
        if (isNodeModified(node, yaml)) {
            auto label_rule = node.getLabelRule();
            auto m_node = getNodeFromYAML(label_rule, yaml);
            if (m_node.is_nothing()) {
                deletedNodes.push_back(node);
            } else {
                modifiedNodes.push_back(m_node.unsafe_get_just());
            }
        }
        created_nodes.erase(node.getLabelRule());
    }
    // For each created node, create a new TreeNode and add it to the modifiedNodes
    for (const auto& created_node : created_nodes) {
        auto m_node = getNodeFromYAML(created_node, yaml);
        if (m_node.is_nothing()) {
            continue; // If the node is not found in YAML, skip it
        }
        modifiedNodes.push_back(m_node.unsafe_get_just());
    }
    if (!modifiedNodes.empty()) {
        to.upsertNode(modifiedNodes);
    }
    for (const auto& deleted_node : deletedNodes) {
        to.deleteNode(deleted_node.getLabelRule());
    }
    setProcessing.store(false);
}

YAMLMediator::YAMLMediator(Backend& tree, Backend& yamlTree, const PropertySpecifier& specifier, bool initialize_tree)
    : backendTree_(tree), backendYAMLTree_(yamlTree), specifier_(specifier)
{
    yamlRepresentation_ = YAML::Node();
    if (initialize_tree) {
        // Initialize the backend tree with the YAML data source
        auto m_node = backendTree_.getNode(specifier.getNodeLabel());
        if (m_node.is_nothing()) {
            throw runtime_error("Node with label " + specifier.getNodeLabel() +
                                     " does not exist in the backend tree.");
        }
        fromYAMLCallback(backendYAMLTree_, yamlRepresentation_, specifier_, processingTree_, processingYAMLTree_,
            specifier.getNodeLabel(), m_node);
    } else {
        // Initialize the YAML data source with the information from the backend tree
        auto fullTree = backendTree_.getFullTree();
        maybe<TreeNode> m_node;
        for (const auto& node : fullTree) {
            if (m_node.is_nothing()) {
                m_node = just(node);
                continue;
            } 
            updateYAMLFrom(yamlRepresentation_, node.getLabelRule(), just(node), global_null_backend, false);
            cerr << "YAMLMediator: Updating YAML representation for node: " << node.getLabelRule() << endl;
            cerr << yamlRepresentation_ << endl;
        }
        auto label_rule = m_node.lift_def("", [](const TreeNode& node) { return node.getLabelRule(); });
        toYAMLCallback(backendTree_, backendYAMLTree_, yamlRepresentation_, specifier_, processingYAMLTree_, processingTree_,
            label_rule, m_node); // Use the first node as a reference for the YAML representation
    }
    processingTree_.store(false);
    processingYAMLTree_.store(false);
    // Create a callback for the tree backend to listen for changes
    backendTree_.registerNodeListener("YAMLMediatorTree", specifier.getNodeLabel(), false, [this](Backend&, const string& label_rule, const maybe<TreeNode>& m_node) {
        fromYAMLCallback(backendYAMLTree_, yamlRepresentation_, specifier_, processingTree_, processingYAMLTree_, label_rule, m_node);
    });

    string root_node_label_rule = ""; 
    // Create a callback for the YAML tree backend to listen for changes
    backendYAMLTree_.registerNodeListener("YAMLMediatorYAMLTree", root_node_label_rule, true, [this](Backend&, const string& label_rule, const maybe<TreeNode>& m_node) {
        toYAMLCallback(backendTree_, backendYAMLTree_, yamlRepresentation_, specifier_, processingYAMLTree_, processingTree_, label_rule, m_node);
    });
}

YAMLMediator::~YAMLMediator() {
    // Unregister the callbacks
    backendTree_.deregisterNodeListener("YAMLMediatorTree", specifier_.getNodeLabel());
    backendYAMLTree_.deregisterNodeListener("YAMLMediatorYAMLTree", "");
}