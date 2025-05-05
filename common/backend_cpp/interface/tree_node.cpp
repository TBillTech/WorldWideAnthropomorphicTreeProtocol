#include "tree_node.h"

using namespace std;

TreeNode::TreeNode() 
    : label_rule(""), description(""),
      literal_types(), child_names(), contents(global_no_chunk_header, false), 
      version({0, 0, ""}) {}

TreeNode::TreeNode(const std::string& label_rule, const std::string& description, 
        const std::vector<std::string>& literal_types,
        const TreeNodeVersion& version,
        const std::vector<std::string>& child_names,
        shared_span<>&& contents, 
        const fplus::maybe<std::string>& query_how_to,
        const fplus::maybe<std::string>& qa_sequence)
    : label_rule(label_rule), description(description), literal_types(literal_types),
      version(version), child_names(child_names), contents(move(contents)), 
      query_how_to(query_how_to), qa_sequence(qa_sequence)
    {}

// The assignment operators are for the most part typical, except for the handling of the contents.
// When doing move semantics, the contents need to use move semantics.
// When doing copy semantics, the contents need to use the shared_span restrict method to get a reference counted view of the data.
TreeNode& TreeNode::operator=(const TreeNode& other) {
    if (this != &other) {
        label_rule = other.label_rule;
        description = other.description;
        literal_types = other.literal_types;
        child_names = other.child_names;
        auto content_total_range = make_pair(0, other.contents.size());
        contents = move(other.contents.restrict(content_total_range));
        version = other.version;
        query_how_to = other.query_how_to;
        qa_sequence = other.qa_sequence;
    }
    return *this;
}

TreeNode& TreeNode::operator=(TreeNode&& other) noexcept {
    if (this != &other) {
        label_rule = std::move(other.label_rule);
        description = std::move(other.description);
        literal_types = std::move(other.literal_types);
        child_names = std::move(other.child_names);
        contents = std::move(other.contents);
        version = std::move(other.version);
        query_how_to = std::move(other.query_how_to);
        qa_sequence = std::move(other.qa_sequence);
    }
    return *this;
}

TreeNode::TreeNode(TreeNode&& other) noexcept
    : label_rule(std::move(other.label_rule)), description(std::move(other.description)),
      literal_types(std::move(other.literal_types)),
      child_names(std::move(other.child_names)), contents(std::move(other.contents)),
      version(std::move(other.version)),
      query_how_to(std::move(other.query_how_to)), qa_sequence(std::move(other.qa_sequence))
      {}

TreeNode::TreeNode(const TreeNode& other)
    : label_rule(other.label_rule), description(other.description),
      literal_types(other.literal_types),
      child_names(other.child_names), contents(other.contents),
      version(other.version), 
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

const std::vector<std::string>& TreeNode::getLiteralTypes() const {
    return literal_types;
}

void TreeNode::setLiteralTypes(const std::vector<std::string>& literal_types) {
    this->literal_types = literal_types;
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
    return move(absolute_child_names);
}

void TreeNode::setChildNames(const std::vector<std::string>& child_names) {
    this->child_names = child_names;
}

const shared_span<>& TreeNode::getContents() const {
    return contents;
}

void TreeNode::setContents(shared_span<>&& contents) {
    this->contents = move(contents);
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
    properties_equal =  literal_types == other.literal_types && properties_equal;
    properties_equal =  version == other.version && properties_equal;
    properties_equal =  child_names == other.child_names && properties_equal;
    auto query_how_to_value = query_how_to.get_with_default("");
    auto other_query_how_to_value = other.query_how_to.get_with_default("");
    properties_equal = query_how_to_value == other_query_how_to_value && properties_equal;
    properties_equal =  qa_sequence == other.qa_sequence && properties_equal;
    properties_equal = label_rule == other.label_rule && properties_equal;
    bool contents_equal = contents.size() == other.contents.size();
    if (contents_equal) {
        auto this_it = contents.begin<uint8_t>();
        auto other_it = other.contents.begin<uint8_t>();
        for (size_t i = 0; i < contents.size(); ++i) {
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
// In particular, the concern is that sometimes description might contain statements about literal_types or TreeNodes and so on.
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

std::ostream& operator<<(std::ostream& os, const TreeNode& node)
{
    os << "TreeNode(\n";
    os << "label_rule: " << node.getLabelRule() << "\n";
    write_length_string(os, "description", node.getDescription());
    os << "literal_types: [ ";
    for (const auto& type : node.getLiteralTypes()) {
        os << type << ", ";
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

    os << node.getContents();

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

    std::vector<std::string> literal_types;
    is >> label; // Consume "literal_types:"
    is >> label; // Consume " ["
    is.get(); // Consume the space after "["
    while (is.peek() != ']') {
        std::string type;
        is >> type;
        if (type.back() == ',') {
            type.pop_back(); // Remove the trailing comma
        }
        if (is.peek() == ' ') {
            is.get(); // Consume the space after the comma
        }
        literal_types.push_back(type);
    }
    is >> label; // Consume "]"
    node.setLiteralTypes(literal_types);

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

    is >> node.contents; // Read the contents

    is >> label; // Consume ")"
    is.get(); // Consume the newline after the closing parenthesis

    return is;
}
