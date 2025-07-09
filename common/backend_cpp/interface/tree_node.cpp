#include "tree_node.h"

using namespace std;

TreeNode::TreeNode()
    : label_rule(""), description(""), property_infos({}), 
      version(), child_names({}), property_data(global_no_chunk_header, false),
      query_how_to(fplus::maybe<string>()), qa_sequence(fplus::maybe<string>())
    {}

TreeNode::TreeNode(const std::string& label_rule, const std::string& description, 
        const std::vector<TreeNode::PropertyInfo>& property_infos,
        const TreeNodeVersion& version,
        const std::vector<std::string>& child_names,
        shared_span<>&& property_data, 
        const fplus::maybe<std::string>& query_how_to,
        const fplus::maybe<std::string>& qa_sequence)
    : label_rule(label_rule), description(description), property_infos(property_infos),
      version(version), child_names(child_names), property_data(move(property_data)), 
      query_how_to(query_how_to), qa_sequence(qa_sequence)
    {
        // label_rule cannot have whitespace
        if (label_rule.find_first_of(" \t\n\r") != string::npos) {
            throw invalid_argument("Label rule cannot contain whitespace: " + label_rule);
        }
        for (auto child_name: child_names) {
            // Child names cannot have whitespace
            if (child_name.find_first_of(" \t\n\r") != string::npos) {
                throw invalid_argument("Child names cannot contain whitespace: " + child_name);
            }
        }
        if (query_how_to == fplus::maybe<string>("")) {
            this->query_how_to = fplus::maybe<string>();
        }
        if (qa_sequence == fplus::maybe<string>("")) {
            this->qa_sequence = fplus::maybe<string>();
        }
    }

// The assignment operators are for the most part typical, except for the handling of the property_data.
// When doing move semantics, the property_data need to use move semantics.
// When doing copy semantics, the property_data need to use the shared_span restrict method to get a reference counted view of the data.
TreeNode& TreeNode::operator=(const TreeNode& other) {
    if (this != &other) {
        label_rule = other.label_rule;
        description = other.description;
        property_infos = other.property_infos;
        version = other.version;
        child_names = other.child_names;
        auto content_total_range = make_pair(0, other.property_data.size());
        property_data = move(other.property_data.restrict(content_total_range));
        query_how_to = other.query_how_to;
        qa_sequence = other.qa_sequence;
    }
    return *this;
}

TreeNode& TreeNode::operator=(TreeNode&& other) noexcept {
    if (this != &other) {
        label_rule = std::move(other.label_rule);
        description = std::move(other.description);
        property_infos = std::move(other.property_infos);
        version = std::move(other.version);
        child_names = std::move(other.child_names);
        property_data = std::move(other.property_data);
        version = std::move(other.version);
        query_how_to = std::move(other.query_how_to);
        qa_sequence = std::move(other.qa_sequence);
    }
    return *this;
}

TreeNode::TreeNode(TreeNode&& other) noexcept
    : label_rule(std::move(other.label_rule)), description(std::move(other.description)),
      property_infos(std::move(other.property_infos)), version(std::move(other.version)),
      child_names(std::move(other.child_names)), property_data(std::move(other.property_data)),
      query_how_to(std::move(other.query_how_to)), qa_sequence(std::move(other.qa_sequence))
      {}

TreeNode::TreeNode(const TreeNode& other)
    : label_rule(other.label_rule), description(other.description),
      property_infos(other.property_infos), version(other.version), 
      child_names(other.child_names), property_data(other.property_data),
      query_how_to(other.query_how_to), qa_sequence(other.qa_sequence)
      {}

const std::string& TreeNode::getLabelRule() const {
    return label_rule;
}

void TreeNode::setLabelRule(const std::string& label_rule) {
    this->label_rule = label_rule;
}

const std::string& TreeNode::getDescription() const {
    return description;
}

void TreeNode::setDescription(const std::string& description) {
    this->description = description;
}

const fplus::maybe<std::string>& TreeNode::getQueryHowTo() const {    
    return query_how_to;
}

void TreeNode::setQueryHowTo(const fplus::maybe<std::string>& query_how_to) {
    if (query_how_to == fplus::maybe<string>("")) {
        this->query_how_to = fplus::maybe<string>();
    }else {
        this->query_how_to = query_how_to;
    }
}

const fplus::maybe<std::string>& TreeNode::getQaSequence() const {
    return qa_sequence;
}

void TreeNode::setQaSequence(const fplus::maybe<std::string>& qa_sequence) {
    if (qa_sequence == fplus::maybe<string>("")) {
        this->qa_sequence = fplus::maybe<string>();
    } else {
        this->qa_sequence = qa_sequence;
    }
}

const std::vector<TreeNode::PropertyInfo>& TreeNode::getPropertyInfo() const {
    return property_infos;
}

void TreeNode::setPropertyInfo(const std::vector<TreeNode::PropertyInfo>& property_infos) {
    this->property_infos = property_infos;
}

const string& TreeNode::getPolicy() const {
    return version.policy;
}

void TreeNode::setPolicy(const string& policy) {
    this->version.policy = policy;
}

const std::vector<std::string>& TreeNode::getChildNames() const {
    return child_names;
}

std::vector<std::string> TreeNode::getAbsoluteChildNames() const {
    std::vector<std::string> absolute_child_names;
    for (const auto& child_name : child_names) {
        absolute_child_names.push_back(label_rule + "/" + child_name);
    }
    return absolute_child_names;
}

void TreeNode::setChildNames(const std::vector<std::string>& child_names) {
    this->child_names = child_names;
}

const shared_span<>& TreeNode::getPropertyData() const {
    return property_data;
}

void TreeNode::setPropertyData(shared_span<>&& property_data) {
    this->property_data = move(property_data);
}

const TreeNodeVersion& TreeNode::getVersion() const {
    return version;
}

void TreeNode::setVersion(const TreeNodeVersion& version) {
    this->version = version;
}

bool TreeNode::operator==(const TreeNode& other) const {
    bool properties_equal = true;
    properties_equal = label_rule == other.label_rule && properties_equal;
    properties_equal = description == other.description && properties_equal;
    properties_equal = property_infos == other.property_infos && properties_equal;
    properties_equal = version == other.version && properties_equal;
    properties_equal = child_names == other.child_names && properties_equal;
    auto query_how_to_value = query_how_to.get_with_default("");
    auto other_query_how_to_value = other.query_how_to.get_with_default("");
    properties_equal = query_how_to_value == other_query_how_to_value && properties_equal;
    properties_equal = qa_sequence == other.qa_sequence && properties_equal;
    bool contents_equal = property_data.size() == other.property_data.size();
    if (contents_equal) {
        auto this_it = property_data.begin<uint8_t>();
        auto other_it = other.property_data.begin<uint8_t>();
        for (size_t i = 0; i < property_data.size(); ++i) {
            if (*this_it != *other_it) {
                contents_equal = false;
                break;
            }
            ++this_it;
            ++other_it;
        }
    }
    return properties_equal && contents_equal;
}

// We need a string-length-value function in order to implement stream operators for long strings that contain arbitrary data.
// In particular, the concern is that sometimes description might contain statements about property_infos or TreeNodes and so on.
// So, this function accepts a label for the string, and writes out the length of the string, followed by the string itself.
void write_length_string(std::ostream& os, const std::string& label, const std::string& str) {
    os << label << " " << str.size() << " :\n";
    os << str;
    os << "\n";
}
pair<string,string> read_length_string(std::istream& is) {
    size_t length;
    std::string label;
    is >> label;
    is >> length;
    std::string delimiter;
    is >> delimiter; // Read the delimiter (":")
    is.get(); // Consume the newline after the delimiter
    string out_str;
    out_str.resize(length);
    is.read(&out_str[0], length);
    is.get(); // Consume the newline after the string
    return {label, out_str};
}

// Custom flag management
long& hide_contents_flag(std::ios_base& ios) {
    static int index = std::ios_base::xalloc();
    return ios.iword(index);
}

// Manipulator to set the hide_contents flag
std::ostream& hide_contents(std::ostream& os) {
    hide_contents_flag(os) = 1;
    return os;
}

// Manipulator to clear the hide_contents flag
std::ostream& show_contents(std::ostream& os) {
    hide_contents_flag(os) = 0;
    return os;
}

std::ostream& operator<<(std::ostream& os, const TreeNode& node)
{
    os << "TreeNode(\n";
    os << "label_rule: " << node.getLabelRule() << "\n";
    write_length_string(os, "description", node.getDescription());
    os << "property_infos: [ ";
    for (const auto& info : node.getPropertyInfo()) {
        auto type = info.first; // Type of the property
        auto name = info.second; // Name of the property
        os << type << " (" << name << "), ";
    }
    os << "], ";
    os << "version: " << node.getVersion() << ", ";

    os << "child_names: [ ";
    for (const auto& child_name : node.getChildNames()) {
        os << child_name << ", ";
    }
    os << "], ";

    write_length_string(os, "query_how_to", node.getQueryHowTo().get_with_default(""));
    write_length_string(os, "qa_sequence", node.getQaSequence().get_with_default(""));

    if (hide_contents_flag(os) == 0) {
        os << node.getPropertyData();
    } else {
        shared_span<> empty(global_no_chunk_header, false);
        os << empty;
    }

    os << ")\n";
    return os;
}

std::istream& operator>>(std::istream& is, TreeNode& node)
{
    std::string label;
    is >> label; // Consume "TreeNode("

    std::string label_rule;
    is >> label >> label_rule; // Read "label_rule:" and the value
    node.setLabelRule(label_rule);

    auto description = read_length_string(is);
    node.setDescription(description.second);

    std::vector<TreeNode::PropertyInfo> property_infos;
    is >> label; // Consume "property_infos:"
    is >> label; // Consume " ["
    is.get(); // Consume the space after "["
    while (is.peek() != ']') {
        std::string type;
        std::string name;
        is >> type;
        is >> name; // Read the property name
        if (name.back() == ',') {
            name.pop_back(); // Remove the trailing comma
        }
        if (name.front() == '(' && name.back() == ')') {
            name = name.substr(1, name.size() - 2); // Remove parentheses
        }
        if (type.back() == ',') {
            type.pop_back(); // Remove the trailing comma
        }
        if (is.peek() == ' ') {
            is.get(); // Consume the space after the comma
        }
        property_infos.push_back({type, name});
    }
    is >> label; // Consume "]"
    node.setPropertyInfo(property_infos);

    TreeNodeVersion version;
    is >> label >> version; // Read "version:" and the value
    node.setVersion(version);
    is >> label; // Consume "," after version

    std::vector<std::string> child_names;
    is >> label; // Consume "child_names:"
    is >> label; // Consume " ["
    is.get(); // Consume the space after "["
    while (is.peek() != ']') {
        std::string child_name;
        is >> child_name;
        if (child_name.back() == ',') {
            child_name.pop_back(); // Remove the trailing comma
        }
        child_names.push_back(child_name);
        if (is.peek() == ' ') {
            is.get(); // Consume the space after the comma
        }
    }
    is >> label; // Consume "]"
    node.setChildNames(child_names);

    auto query_how_to_str = read_length_string(is);
    if (query_how_to_str.first == "query_how_to") {
        if (query_how_to_str.second.empty()) {
            node.setQueryHowTo(fplus::nothing<std::string>());
        } else {
            node.setQueryHowTo(fplus::just(query_how_to_str.second));
        }
    } else {
        throw std::runtime_error("Expected 'query_how_to' label, got: " + query_how_to_str.first);
    }

    auto qa_sequence_str = read_length_string(is);
    if (qa_sequence_str.first == "qa_sequence") {
        if (qa_sequence_str.second.empty()) {
            node.setQaSequence(fplus::nothing<std::string>());
        } else {
            node.setQaSequence(fplus::just(qa_sequence_str.second));
        }
    } else {
        throw std::runtime_error("Expected 'qa_sequence' label, got: " + qa_sequence_str.first);
    }

    is >> node.property_data; // Read the property_data

    is >> label; // Consume ")"
    is.get(); // Consume the newline after the closing parenthesis

    return is;
}

void TreeNode::prefixLabels(const std::string& prefix)
{
    if (prefix.empty()) {
        return;
    }
    if (prefix.back() != '/') {
        throw std::runtime_error("Prefix must end with a '/'");
    }
    if (label_rule.find(prefix) != 0) {
        label_rule = prefix + label_rule;
    }
    auto new_child_names = std::vector<std::string>(child_names.size());
    for (size_t i = 0; i < child_names.size(); ++i) {
        if (child_names[i].find(prefix) != 0) {
            new_child_names[i] = prefix + child_names[i];
        } else {
            new_child_names[i] = child_names[i];
        }
    }
    child_names = std::move(new_child_names);
}

void TreeNode::shortenLabels(const std::string& prefix)
{
    if (prefix.empty()) {
        return;
    }
    if (label_rule.find(prefix) == 0) {
        label_rule = label_rule.substr(prefix.size());
    }
    for (auto& child_name : child_names) {
        if (child_name.find(prefix) == 0) {
            child_name = child_name.substr(prefix.size());
        }
    }
}


void prefixTransactionLabels(const std::string& prefix, Transaction& transaction)
{
    if (prefix.empty()) {
        return;
    }
    for (auto& sub_transaction : transaction) {
        prefixSubTransactionLabels(prefix, sub_transaction);
    }
}

void shortenTransactionLabels(const std::string& prefix, Transaction& transaction)
{
    if (prefix.empty()) {
        return;
    }
    for (auto& sub_transaction : transaction) {
        shortenSubTransactionLabels(prefix, sub_transaction);
    }
}

void prefixSubTransactionLabels(const std::string& prefix, SubTransaction& sub_transaction)
{
    if (prefix.empty()) {
        return;
    }
    prefixNewNodeVersionLabels(prefix, sub_transaction.first);
    for (auto& new_node_version : sub_transaction.second) {
        prefixNewNodeVersionLabels(prefix, new_node_version);
    }
}

void shortenSubTransactionLabels(const std::string& prefix, SubTransaction& sub_transaction)
{
    if (prefix.empty()) {
        return;
    }
    shortenNewNodeVersionLabels(prefix, sub_transaction.first);
    for (auto& new_node_version : sub_transaction.second) {
        shortenNewNodeVersionLabels(prefix, new_node_version);
    }
}

void prefixNewNodeVersionLabels(const std::string& prefix, NewNodeVersion& new_node_version)
{
    if (prefix.empty()) {
        return;
    }
    if (prefix.back() != '/') {
        throw std::runtime_error("Prefix must end with a '/'");
    }
    if (new_node_version.second.second.is_just()) {
        new_node_version.second.second.unsafe_get_just().prefixLabels(prefix);
    }
    if (new_node_version.second.first.find(prefix) != 0) {
        new_node_version.second.first = prefix + new_node_version.second.first;
    }
}

void shortenNewNodeVersionLabels(const std::string& prefix, NewNodeVersion& new_node_version)
{
    if (prefix.empty()) {
        return;
    }
    if (new_node_version.second.first.find(prefix) == 0) {
        new_node_version.second.first = new_node_version.second.first.substr(prefix.size());
    }
    if (new_node_version.second.second.is_just()) {
        new_node_version.second.second.unsafe_get_just().shortenLabels(prefix);
    }
}