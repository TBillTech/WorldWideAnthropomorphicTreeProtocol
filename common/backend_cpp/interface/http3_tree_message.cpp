#include <vector>

#include "http3_tree_message.h"
#include "shared_chunk.h"
#include "tree_node.h"

static const std::vector<string> SIGNAL_STRINGS = {
    "SIGNAL_OTHER_CHUNK", "SIGNAL_CLOSE_STREAM", "SIGNAL_HEARTBEAT", "SIGNAL_WWATP_REQUEST_CONTINUE", "SIGNAL_WWATP_RESPONSE_CONTINUE", "SIGNAL_WWATP_REQUEST_FINAL", "SIGNAL_WWATP_RESPONSE_FINAL", "SIGNAL_WWATP_GET_NODE_REQUEST", "SIGNAL_WWATP_GET_NODE_RESPONSE", "SIGNAL_WWATP_UPSERT_NODE_REQUEST", "SIGNAL_WWATP_UPSERT_NODE_RESPONSE", "SIGNAL_WWATP_DELETE_NODE_REQUEST", "SIGNAL_WWATP_DELETE_NODE_RESPONSE", "SIGNAL_WWATP_GET_PAGE_TREE_REQUEST", "SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE", "SIGNAL_WWATP_QUERY_NODES_REQUEST", "SIGNAL_WWATP_QUERY_NODES_RESPONSE", "SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST", "SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE", "SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST", "SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE", "SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST", "SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE", "SIGNAL_WWATP_GET_FULL_TREE_REQUEST", "SIGNAL_WWATP_GET_FULL_TREE_RESPONSE", "SIGNAL_WWATP_REGISTER_LISTENER_REQUEST", "SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE", "SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST", "SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE", "SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST", "SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE", "SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST", "SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE", "SIGNAL_WWATP_GET_JOURNAL_REQUEST", "SIGNAL_WWATP_GET_JOURNAL_RESPONSE"
};

static const string UNKNOWN_SIGNAL_STRING = "UNKNOWN_SIGNAL";

constexpr const string& get_signal_string(uint8_t signal) {
    if (signal < SIGNAL_STRINGS.size()) {
        return SIGNAL_STRINGS[signal];
    }
    return UNKNOWN_SIGNAL_STRING;
}

chunkList encode_label(uint16_t request_id, uint8_t signal, const string& str) {
    chunkList encoded;
    if (str.size() > shared_span<>::chunk_size - sizeof(payload_chunk_header)) {
        throw invalid_argument("String size exceeds chunk size");
    }
    auto header = payload_chunk_header(request_id, signal, str.size());
    encoded.emplace_back(header, span<const char>(str.c_str(), str.size()));
    return encoded;
}

pair<size_t, string> decode_label(chunkList encoded) {
    auto& chunk = encoded.front();
    string str(chunk.begin<const char>(), chunk.end<const char>());
    return {1, str};
}

fplus::maybe<size_t> can_decode_label(size_t start_chunk, chunkList encoded) {
    if (encoded.size() <= start_chunk) {
        return fplus::nothing<size_t>();
    }
    auto& chunk = *std::next(encoded.begin(), start_chunk);
    if (chunk.size() < sizeof(payload_chunk_header)) {
        throw invalid_argument("Chunk size is less than header size");
    }
    auto header = chunk.get_signal<payload_chunk_header>();
    size_t data_size = header.data_length;
    if (chunk.size() < data_size) {
        throw invalid_argument("Chunk size is less than data size");
    }
    return fplus::maybe<size_t>(start_chunk + 1);
}

chunkList encode_long_string(uint16_t request_id, uint8_t signal, const string& str) {
    chunkList encoded;
    auto str_start = str.c_str();
    auto cur_str_position = 0;
    size_t max_chunk_payload_size = shared_span<>::chunk_size - sizeof(payload_chunk_header);
    size_t first_chunk_payload_size = min(max_chunk_payload_size, str.size());
    auto header = payload_chunk_header(request_id, signal, first_chunk_payload_size);
    auto chunk = shared_span<>(header, true);
    auto position = chunk.copy_type(str.size());
    auto first_chunk_str_payload = first_chunk_payload_size - sizeof(size_t);
    auto upto = chunk.copy_span(span<const char>(str_start, first_chunk_str_payload), {true, position});
    encoded.emplace_back(chunk.restrict_upto(upto));
    cur_str_position += first_chunk_str_payload;
    while(cur_str_position < str.size()) {
        auto remaining_str_size = str.size() - cur_str_position;
        auto chunk_payload_size = min(max_chunk_payload_size, remaining_str_size);
        auto header = payload_chunk_header(request_id, signal, chunk_payload_size);
        encoded.emplace_back(header, span<const char>(str_start + cur_str_position, chunk_payload_size));
        cur_str_position += chunk_payload_size;
    }
    return encoded;
}

pair<size_t, string> decode_long_string(chunkList encoded) {
    auto chunk_count = can_decode_long_string(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode long string");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    shared_span<> concatenated(encoded.begin(), std::next(encoded.begin(), chunk_count_value));
    auto just_payload = concatenated.restrict({sizeof(size_t), concatenated.size()});
    string decoded_str(concatenated.begin<const char>(), concatenated.end<const char>());
    return {chunk_count_value, decoded_str};
}

fplus::maybe<size_t> can_decode_long_string(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto& chunk = *std::next(encoded.begin(), start_chunk);
    if (chunk.size() < sizeof(payload_chunk_header)) {
        throw invalid_argument("Chunk size is less than header size");
    }
    auto header = chunk.get_signal<payload_chunk_header>();
    size_t data_size = header.data_length;
    size_t long_string_length = 0;
    chunk.at<size_t>(long_string_length);
    auto expected_chunk = start_chunk+1;
    long_string_length -= shared_span<>::chunk_size - sizeof(payload_chunk_header) - sizeof(size_t);
    while(long_string_length > 0){
        expected_chunk++;
        long_string_length -= shared_span<>::chunk_size - sizeof(payload_chunk_header);
    }
    if (expected_chunk > encoded.size()) {
        return fplus::nothing<size_t>();
    }
    return fplus::maybe<size_t>(expected_chunk);
}

chunkList encode_MaybeTreeNode(uint16_t request_id, uint8_t signal, const fplus::maybe<TreeNode>& mnode) {
    if (mnode.is_nothing()) {
        return encode_long_string(request_id, signal, "Nothing");
    }
    TreeNode node = mnode.unsafe_get_just();
    stringstream oss;
    oss << "Just TreeNode(\n";
    oss << "label_rule: " << node.getLabelRule() << "\n";
    write_length_string(oss, "description", node.getDescription());
    oss << "literal_types: [ ";
    for (const auto& type : node.getLiteralTypes()) {
        oss << type << ", ";
    }
    oss << "], ";
    oss << "version: " << node.getVersion() << ", ";

    oss << "child_names: [ ";
    for (const auto& child_name : node.getChildNames()) {
        oss << child_name << ", ";
    }
    oss << "], ";

    write_length_string(oss, "query_how_to", node.getQueryHowTo().get_with_default(""));
    write_length_string(oss, "qa_sequence", node.getQaSequence().get_with_default(""));

    shared_span<> empty_span(global_no_chunk_header, false);
    oss << empty_span;

    oss << ")\n";
    // Output node.getContents().size() as hex, padded to fixed width
    // However, worrying about a 32 bit system, use sizeof(size_t) == 8
    oss << std::setw(8 * 2) << std::setfill('0') << std::hex << node.getContents().size();
    oss << std::dec; // Reset to decimal for any further output

    chunkList encoded = encode_long_string(request_id, signal, oss.str());
    payload_chunk_header header(request_id, signal, 0);
    auto contents_vec = node.getContents().flatten_with_signal(header);
    chunkList contents(std::make_move_iterator(contents_vec.begin()), std::make_move_iterator(contents_vec.end()));
    encoded.insert(encoded.end(), std::make_move_iterator(contents.begin()), std::make_move_iterator(contents.end()));
    return encoded;
}

pair<size_t, fplus::maybe<TreeNode>> decode_MaybeTreeNode(chunkList encoded) {
    auto chunk_count = can_decode_long_string(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode MaybeTreeNode");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    shared_span<> concatenated(encoded.begin(), std::next(encoded.begin(), chunk_count_value));
    auto just_payload = concatenated.restrict({sizeof(size_t), concatenated.size()});
    string decoded_str(concatenated.begin<const char>(), concatenated.end<const char>());
    if (decoded_str == "Nothing") {
        return {chunk_count_value, fplus::nothing<TreeNode>()};
    }
    TreeNode node;
    stringstream iss(decoded_str);
    std::string label;
    iss >> label; // Consume "Just" to get to the TreeNode
    iss >> node;
    size_t contents_count;
    iss >> std::hex >> contents_count;
    shared_span<> contents(std::next(encoded.begin(), chunk_count_value), std::next(encoded.begin(), chunk_count_value + contents_count));
    node.setContents(move(contents));
    return {chunk_count_value+contents_count, fplus::maybe<TreeNode>(node)};
}

fplus::maybe<size_t> can_decode_MaybeTreeNode(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto& chunk = *std::next(encoded.begin(), start_chunk);
    if (chunk.size() < sizeof(payload_chunk_header)) {
        throw invalid_argument("Chunk size is less than header size");
    }
    auto can_decode_structure = can_decode_long_string(start_chunk, encoded);
    if (can_decode_structure.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    if (can_decode_structure.unsafe_get_just() >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count = can_decode_structure.unsafe_get_just();
    // now grab the _last_ 8 * 2 characters of the _last_ chunk that can be decoded:
    auto& last_chunk = *std::next(encoded.begin(), chunk_count + start_chunk - 1);
    auto last_chunk_size = last_chunk.size();
    if (last_chunk_size < 8 * 2) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count_start = sizeof(payload_chunk_header) + last_chunk_size - 8 * 2;
    struct sixteen_bytes_t {
        char contents[8*2];
    };
    sixteen_bytes_t contents_size_str = {0};
    last_chunk.at<sixteen_bytes_t>(contents_size_str, 
        {true, {chunk_count_start, 0}});
    // Use string_view to avoid null-termination issues
    std::string_view contents_view(contents_size_str.contents, sizeof(contents_size_str.contents));
    std::stringstream iss;
    iss.write(contents_view.data(), contents_view.size());
    size_t contents_size;
    iss >> std::hex >> contents_size;
    // Finally, check if chunk_count + contents_size + start_chunk is within the bounds of the encoded chunkList
    if (chunk_count + contents_size + start_chunk > encoded.size()) {
        return fplus::nothing<size_t>();
    }
    return fplus::maybe<size_t>(chunk_count + contents_size + start_chunk);
}

chunkList encode_SequentialNotification(uint16_t request_id, uint8_t signal, const SequentialNotification& notifications) {
    // The sequential nofitication is a pair of a uint64_t and a Notification,
    // and the Notification is a pair of a string and a maybe<TreeNode>.
    // We want to leverage the encode and decode methods for the string and maybe<TreeNode> types,
    // So Pack up the uint64_t and the string into a chunk, and then contatenate 
    // the chunkList from delegating to encode_MaybeTreeNode.
 
    // We can leverage encode_label by prefixing the nofication label with the sequence number
    // and then encoding the label as a string.
    stringstream oss;
    oss << notifications.first << " " << notifications.second.first;
    chunkList encoded = encode_label(request_id, signal, oss.str());
    chunkList maybe_tree = encode_MaybeTreeNode(request_id, signal, notifications.second.second);
    encoded.insert(encoded.end(), std::make_move_iterator(maybe_tree.begin()), std::make_move_iterator(maybe_tree.end()));
    return encoded;
}

pair<size_t, SequentialNotification> decode_SequentialNotification(chunkList encoded) {
    auto chunk_count = can_decode_long_string(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode SequentialNotification");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    auto decoded = decode_label(encoded);
    // unpack the uint64_t and the string
    stringstream iss(decoded.second);
    uint64_t sequence_number;
    iss >> sequence_number;
    // use the current offset of the iss to get the start of the notification label
    auto notification_label_start = iss.tellg();
    string notification_label = decoded.second.substr(notification_label_start);
    // Then, decode the maybe TreeNode beginning at encoded[1]
    auto maybe_tree = decode_MaybeTreeNode(chunkList(std::next(encoded.begin(), decoded.first), encoded.end()));
    if (maybe_tree.first == 0) {
        throw invalid_argument("Cannot decode MaybeTreeNode");
    }
    auto maybe_tree_value = maybe_tree.second;
    Notification notification(notification_label, maybe_tree_value);
    SequentialNotification seq_notification(sequence_number, notification);
    return {chunk_count_value + maybe_tree.first, seq_notification};
}

fplus::maybe<size_t> can_decode_SequentialNotification(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto& chunk = *std::next(encoded.begin(), start_chunk);
    if (chunk.size() < sizeof(payload_chunk_header)) {
        throw invalid_argument("Chunk size is less than header size");
    }
    auto can_decode_structure = can_decode_long_string(start_chunk, encoded);
    if (can_decode_structure.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    if (can_decode_structure.unsafe_get_just() >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    // Then simply delegate to can_decode_MaybeTreeNode
    return can_decode_MaybeTreeNode(can_decode_structure.unsafe_get_just(), encoded);
}

chunkList encode_VectorSequentialNotification(uint16_t request_id, uint8_t signal, const vector<SequentialNotification>& notifications) {
    chunkList encoded = encode_label(request_id, signal, to_string(notifications.size()));
    for (const auto& notification : notifications) {
        auto notification_chunk = encode_SequentialNotification(request_id, signal, notification);
        encoded.insert(encoded.end(), std::make_move_iterator(notification_chunk.begin()), std::make_move_iterator(notification_chunk.end()));
    }
    return encoded;
}

pair<size_t, vector<SequentialNotification>> decode_VectorSequentialNotification(chunkList encoded) {
    auto chunk_count = can_decode_label(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode VectorSequentialNotification");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    auto decoded = decode_label(encoded);
    stringstream iss(decoded.second);
    size_t vector_size;
    iss >> vector_size;
    vector<SequentialNotification> notifications;
    for (size_t i = 0; i < vector_size; ++i) {
        auto notification = decode_SequentialNotification(chunkList(std::next(encoded.begin(), decoded.first), encoded.end()));
        notifications.push_back(notification.second);
        decoded.first += notification.first;
    }
    return {chunk_count_value + decoded.first, notifications};
}

fplus::maybe<size_t> can_decode_VectorSequentialNotification(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto& chunk = *std::next(encoded.begin(), start_chunk);
    if (chunk.size() < sizeof(payload_chunk_header)) {
        throw invalid_argument("Chunk size is less than header size");
    }
    auto can_decode_structure = can_decode_label(start_chunk, encoded);
    if (can_decode_structure.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    auto vector_size_decode = decode_label(chunkList(std::next(encoded.begin(), can_decode_structure.unsafe_get_just()), encoded.end()));
    string vector_size_str = vector_size_decode.second;
    stringstream iss(vector_size_str);
    size_t vector_size;
    iss >> vector_size;
    size_t current_offset = 1 + start_chunk;
    for (size_t i = 0; i < vector_size; ++i) {
        auto notification = can_decode_SequentialNotification(current_offset, encoded);
        if (notification.is_nothing()) {
            return fplus::nothing<size_t>();
        }
        current_offset += notification.unsafe_get_just();
    }
    return fplus::maybe<size_t>(current_offset);
}

chunkList encode_NewNodeVersion(uint16_t request_id, uint8_t signal, const NewNodeVersion& new_node_version) {
    // The new node version is a decorated Notificaton similiar to the SequentialNotification.
    // It is a maybe uint16_t with a nofication label and a maybe<TreeNode>.
    // We want to leverage the encode and decode methods for the string and maybe<TreeNode> types,
    // So Pack up the string into a chunk, and then contatenate 
    // the chunkList from delegating to encode_MaybeTreeNode.
    stringstream oss;
    if (new_node_version.first.is_nothing()) {
        oss << "Nothing";
    } else {
        oss << "Just " << new_node_version.first.unsafe_get_just();
    }
    oss << " " << new_node_version.second.first;
    chunkList encoded = encode_label(request_id, signal, oss.str());
    chunkList maybe_tree = encode_MaybeTreeNode(request_id, signal, new_node_version.second.second);
    encoded.insert(encoded.end(), std::make_move_iterator(maybe_tree.begin()), std::make_move_iterator(maybe_tree.end()));
    return encoded;    
}

pair<size_t, NewNodeVersion> decode_NewNodeVersion(chunkList encoded) {
    auto chunk_count = can_decode_long_string(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode NewNodeVersion");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    auto decoded = decode_label(encoded);
    // unpack the uint16_t and the string
    stringstream iss(decoded.second);
    fplus::maybe<uint16_t> version;
    string version_str;
    iss >> version_str;
    if (version_str == "Nothing") {
        version = fplus::nothing<uint16_t>();
    } else {
        uint16_t version_value;
        iss >> version_value;
        version = fplus::maybe<uint16_t>(version_value);
    }
    // use the current offset of the iss to get the start of the notification label
    auto notification_label_start = iss.tellg();
    string notification_label = decoded.second.substr(notification_label_start);
    // Then, decode the maybe TreeNode beginning at encoded[1]
    auto maybe_tree = decode_MaybeTreeNode(chunkList(std::next(encoded.begin(), decoded.first), encoded.end()));
    if (maybe_tree.first == 0) {
        throw invalid_argument("Cannot decode MaybeTreeNode");
    }
    auto maybe_tree_value = maybe_tree.second;
    Notification notification(notification_label, maybe_tree_value);
    NewNodeVersion new_node_version(version, notification);
    return {chunk_count_value + maybe_tree.first, new_node_version};
}

fplus::maybe<size_t> can_decode_NewNodeVersion(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto& chunk = *std::next(encoded.begin(), start_chunk);
    if (chunk.size() < sizeof(payload_chunk_header)) {
        throw invalid_argument("Chunk size is less than header size");
    }
    auto can_decode_structure = can_decode_label(start_chunk, encoded);
    if (can_decode_structure.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    // Then simply delegate to can_decode_MaybeTreeNode
    return can_decode_MaybeTreeNode(can_decode_structure.unsafe_get_just(), encoded);
}

chunkList encode_SubTransaction(uint16_t request_id, uint8_t signal, const SubTransaction& sub_transaction) {
    // The sub transaction is one NewNodeVerison plus a vector of one or more NewNodeVersions.
    // So the only thing to track beyond simply delegating to N NewNodeVersion encoders is 
    // the count of the vector.  The count can leverage the encode_label method.
    chunkList encoded = encode_label(request_id, signal, to_string(sub_transaction.second.size()));
    auto first_new_node_version = encode_NewNodeVersion(request_id, signal, sub_transaction.first);
    encoded.insert(encoded.end(), std::make_move_iterator(first_new_node_version.begin()), std::make_move_iterator(first_new_node_version.end()));
    for (const auto& new_node_version : sub_transaction.second) {
        auto new_node_version_chunk = encode_NewNodeVersion(request_id, signal, new_node_version);
        encoded.insert(encoded.end(), std::make_move_iterator(new_node_version_chunk.begin()), std::make_move_iterator(new_node_version_chunk.end()));
    }
    return encoded;    
}

pair<size_t, SubTransaction> decode_SubTransaction(chunkList encoded) {
    auto chunk_count = can_decode_label(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode SubTransaction");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    auto decoded = decode_label(encoded);
    // unpack the uint16_t and the string
    stringstream iss(decoded.second);
    size_t vector_size;
    iss >> vector_size;
    // Then, decode the NewNodeVersion beginning at encoded[1]
    auto base_newnode = decode_NewNodeVersion(chunkList(std::next(encoded.begin(), decoded.first), encoded.end()));
    if (base_newnode.first == 0) {
        throw invalid_argument("Cannot decode NewNodeVersion");
    }
    auto base_newnode_value = base_newnode.second;
    size_t newnode_version_offset = base_newnode.first;
    vector<NewNodeVersion> new_node_versions;
    for (size_t i = 0; i < vector_size; ++i) {
        auto new_node_version = decode_NewNodeVersion(chunkList(std::next(encoded.begin(), newnode_version_offset), encoded.end()));
        if (new_node_version.first == 0) {
            throw invalid_argument("Cannot decode NewNodeVersion");
        }
        new_node_versions.push_back(new_node_version.second);
        newnode_version_offset = new_node_version.first;
    }
    SubTransaction sub_transaction(base_newnode_value, move(new_node_versions));
    return {newnode_version_offset, sub_transaction};
}

fplus::maybe<size_t> can_decode_SubTransaction(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count = can_decode_label(start_chunk, encoded);
    if (chunk_count.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    chunkList just_count_chunk;
    just_count_chunk.push_back(*std::next(encoded.begin(), start_chunk));
    auto decoded = decode_label(just_count_chunk);
    // read the length of the vector
    stringstream iss(decoded.second);
    size_t vector_size;
    iss >> vector_size;
    // Check the base NewNodeVersion can be decoded
    size_t current_chunk = start_chunk + 1;
    auto base_newnode = can_decode_NewNodeVersion(current_chunk, encoded);
    if (base_newnode.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    current_chunk = base_newnode.unsafe_get_just();
    for (size_t i = 0; i < vector_size; ++i) {
        auto new_node_version = can_decode_NewNodeVersion(current_chunk, encoded);
        if (new_node_version.is_nothing()) {
            return fplus::nothing<size_t>();
        }
        current_chunk = new_node_version.unsafe_get_just();
    }
    // Then simply delegate to can_decode_NewNodeVersion
    return current_chunk;
}

chunkList encode_Transaction(uint16_t request_id, uint8_t signal, const Transaction& transaction) {
    // The transaction is a vector of SubTransactions.
    // So the only thing to track beyond simply delegating to N SubTransaction encoders is 
    // the count of the vector.  The count can leverage the encode_label method.
    chunkList encoded = encode_label(request_id, signal, to_string(transaction.size()));
    for (const auto& sub_transaction : transaction) {
        auto sub_transaction_chunk = encode_SubTransaction(request_id, signal, sub_transaction);
        encoded.insert(encoded.end(), std::make_move_iterator(sub_transaction_chunk.begin()), std::make_move_iterator(sub_transaction_chunk.end()));
    }
    return encoded;    
}

pair<size_t, Transaction> decode_Transaction(chunkList encoded) {
    auto chunk_count = can_decode_label(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode Transaction");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    auto decoded = decode_label(encoded);
    // read the length of the vector
    stringstream iss(decoded.second);
    size_t vector_size;
    iss >> vector_size;
    // Then, decode the SubTransaction beginning at encoded[1]
    size_t sub_transaction_offset = decoded.first;
    Transaction transaction;
    for (size_t i = 0; i < vector_size; ++i) {
        auto sub_transaction = decode_SubTransaction(chunkList(std::next(encoded.begin(), sub_transaction_offset), encoded.end()));
        if (sub_transaction.first == 0) {
            throw invalid_argument("Cannot decode SubTransaction");
        }
        transaction.push_back(sub_transaction.second);
        sub_transaction_offset = sub_transaction.first;
    }
    return {sub_transaction_offset, transaction};
}

fplus::maybe<size_t> can_decode_Transaction(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count = can_decode_label(start_chunk, encoded);
    if (chunk_count.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    chunkList just_count_chunk;
    just_count_chunk.push_back(*std::next(encoded.begin(), start_chunk));
    auto decoded = decode_label(just_count_chunk);
    // read the length of the vector
    stringstream iss(decoded.second);
    size_t vector_size;
    iss >> vector_size;
    size_t current_chunk = start_chunk + 1;
    for (size_t i = 0; i < vector_size; ++i) {
        auto sub_transaction = can_decode_SubTransaction(current_chunk, encoded);
        if (sub_transaction.is_nothing()) {
            return fplus::nothing<size_t>();
        }
        current_chunk = sub_transaction.unsafe_get_just();
    }
    return current_chunk;
}

chunkList encode_VectorTreeNode(uint16_t request_id, uint8_t signal, const std::vector<TreeNode>& nodes) {
    // The vector of tree nodes is a vector of TreeNodes.
    // So the only thing to track beyond simply delegating to N TreeNode encoders is 
    // the count of the vector.  The count can leverage the encode_label method.
    chunkList encoded = encode_label(request_id, signal, to_string(nodes.size()));
    for (const auto& node : nodes) {
        auto node_chunk = encode_MaybeTreeNode(request_id, signal, fplus::maybe<TreeNode>(node));
        encoded.insert(encoded.end(), std::make_move_iterator(node_chunk.begin()), std::make_move_iterator(node_chunk.end()));
    }
    return encoded;    
}

pair<size_t, std::vector<TreeNode>> decode_VectorTreeNode(chunkList encoded) {
    auto chunk_count = can_decode_label(0, encoded);
    if (chunk_count.is_nothing()) {
        throw invalid_argument("Cannot decode VectorTreeNode");
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    auto decoded = decode_label(encoded);
    // read the length of the vector
    stringstream iss(decoded.second);
    size_t vector_size;
    iss >> vector_size;
    // Then, decode the TreeNode beginning at encoded[1]
    size_t tree_node_offset = decoded.first;
    std::vector<TreeNode> nodes;
    for (size_t i = 0; i < vector_size; ++i) {
        auto node = decode_MaybeTreeNode(chunkList(std::next(encoded.begin(), tree_node_offset), encoded.end()));
        if (node.first == 0) {
            throw invalid_argument("Cannot decode TreeNode");
        }
        if (node.second.is_nothing()) {
            throw invalid_argument("TreeNode is nothing");
        }
        nodes.push_back(node.second.unsafe_get_just());
        tree_node_offset = node.first;
    }
    return {tree_node_offset, nodes};
}

fplus::maybe<size_t> can_decode_VectorTreeNode(size_t start_chunk, chunkList encoded) {
    if (start_chunk >= encoded.size()) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count = can_decode_label(start_chunk, encoded);
    if (chunk_count.is_nothing()) {
        return fplus::nothing<size_t>();
    }
    auto chunk_count_value = chunk_count.unsafe_get_just();
    // if chunk_count_values is not 1, then something went wrong
    if (chunk_count_value != 1) {
        throw invalid_argument("Chunk count is not 1");
    }
    chunkList just_count_chunk;
    just_count_chunk.push_back(*std::next(encoded.begin(), start_chunk));
    auto decoded = decode_label(just_count_chunk);
    // read the length of the vector
    stringstream iss(decoded.second);
    size_t vector_size;
    iss >> vector_size;
    size_t current_chunk = start_chunk + 1;
    for (size_t i = 0; i < vector_size; ++i) {
        auto node = can_decode_MaybeTreeNode(current_chunk, encoded);
        if (node.is_nothing()) {
            return fplus::nothing<size_t>();
        }
        current_chunk = node.unsafe_get_just();
    }
    return current_chunk;
}

void HTTP3TreeMessage::encode_getNodeRequest(const std::string& label_rule) {
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, label_rule);
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

std::string HTTP3TreeMessage::decode_getNodeRequest(){
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_getNodeResponse(const fplus::maybe<TreeNode>& node) {
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_NODE_RESPONSE;
    chunkList encoded = encode_MaybeTreeNode(request_id_, signal_, node);
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

fplus::maybe<TreeNode> HTTP3TreeMessage::decode_getNodeResponse() {
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_MaybeTreeNode(responseChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_upsertNodeRequest(const std::vector<TreeNode>& nodes)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_UPSERT_NODE_REQUEST;
    chunkList encoded = encode_VectorTreeNode(request_id_, signal_, nodes);
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

std::vector<TreeNode> HTTP3TreeMessage::decode_upsertNodeRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_VectorTreeNode(requestChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_upsertNodeResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_UPSERT_NODE_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_upsertNodeResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_deleteNodeRequest(const std::string& label_rule)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, label_rule);
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

std::string HTTP3TreeMessage::decode_deleteNodeRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_deleteNodeResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_deleteNodeResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_getPageTreeRequest(const std::string& page_node_label_rule)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_PAGE_TREE_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, page_node_label_rule);
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

std::string HTTP3TreeMessage::decode_getPageTreeRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_getPageTreeResponse(const std::vector<TreeNode>& nodes)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE;
    chunkList encoded = encode_VectorTreeNode(request_id_, signal_, nodes);
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

std::vector<TreeNode> HTTP3TreeMessage::decode_getPageTreeResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_VectorTreeNode(responseChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_getQueryNodesRequest(const std::string& label_rule)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, label_rule);
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

std::string HTTP3TreeMessage::decode_getQueryNodesRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_getQueryNodesResponse(const std::vector<TreeNode>& nodes)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_RESPONSE;
    chunkList encoded = encode_VectorTreeNode(request_id_, signal_, nodes);
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

std::vector<TreeNode> HTTP3TreeMessage::decode_getQueryNodesResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_VectorTreeNode(responseChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_openTransactionLayerRequest(const TreeNode& node)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST;
    chunkList encoded = encode_MaybeTreeNode(request_id_, signal_, fplus::maybe<TreeNode>(node));
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

TreeNode HTTP3TreeMessage::decode_openTransactionLayerRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_MaybeTreeNode(requestChunks);
    if (decoded.second.is_nothing()) {
        throw invalid_argument("TreeNode is nothing");
    }
    return decoded.second.unsafe_get_just();
}

void HTTP3TreeMessage::encode_openTransactionLayerResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_openTransactionLayerResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_closeTransactionLayersRequest()
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, "");
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

void HTTP3TreeMessage::encode_closeTransactionLayersResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_closeTransactionLayersResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_applyTransactionRequest(const Transaction& transaction)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST;
    chunkList encoded = encode_Transaction(request_id_, signal_, transaction);
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

Transaction HTTP3TreeMessage::decode_applyTransactionRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_Transaction(requestChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_applyTransactionResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_applyTransactionResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_getFullTreeRequest()
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_FULL_TREE_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, "");
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

void HTTP3TreeMessage::encode_getFullTreeResponse(const std::vector<TreeNode>& nodes)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_FULL_TREE_RESPONSE;
    chunkList encoded = encode_VectorTreeNode(request_id_, signal_, nodes);
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

std::vector<TreeNode> HTTP3TreeMessage::decode_getFullTreeResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_VectorTreeNode(responseChunks);
    return decoded.second;
}

void HTTP3TreeMessage::encode_registerNodeListenerRequest(const std::string& listener_name, const std::string& label_rule, bool child_notify)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_REGISTER_LISTENER_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, listener_name + " " + label_rule + " " + to_string(child_notify));
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

tuple<std::string, std::string, bool> HTTP3TreeMessage::decode_registerNodeListenerRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    stringstream iss(decoded.second);
    std::string listener_name;
    std::string label_rule;
    bool child_notify;
    iss >> listener_name >> label_rule >> child_notify;
    return {listener_name, label_rule, child_notify};
}

void HTTP3TreeMessage::encode_registerNodeListenerResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_registerNodeListenerResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_deregisterNodeListenerRequest(const std::string& listener_name, const std::string& label_rule)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, listener_name + " " + label_rule);
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

pair<std::string, std::string> HTTP3TreeMessage::decode_deregisterNodeListenerRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    stringstream iss(decoded.second);
    std::string listener_name;
    std::string label_rule;
    iss >> listener_name >> label_rule;
    return {listener_name, label_rule};
}

void HTTP3TreeMessage::encode_deregisterNodeListenerResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_deregisterNodeListenerResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_notifyListenersRequest(const std::string& label_rule, const fplus::maybe<TreeNode>& node)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, label_rule);
    auto node_chunk = encode_MaybeTreeNode(request_id_, signal_, node);
    encoded.insert(encoded.end(), std::make_move_iterator(node_chunk.begin()), std::make_move_iterator(node_chunk.end()));
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

pair<std::string, fplus::maybe<TreeNode> > HTTP3TreeMessage::decode_notifyListenersRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    stringstream iss(decoded.second);
    std::string label_rule;
    iss >> label_rule;
    auto node = decode_MaybeTreeNode(chunkList(std::next(requestChunks.begin(), decoded.first), requestChunks.end()));
    if (node.first == 0) {
        throw invalid_argument("Cannot decode TreeNode");
    }
    return {label_rule, node.second};
}

void HTTP3TreeMessage::encode_notifyListenersResponse(bool success)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, to_string(success));
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

bool HTTP3TreeMessage::decode_notifyListenersResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_label(responseChunks);
    stringstream iss(decoded.second);
    bool success;
    iss >> success;
    return success;
}

void HTTP3TreeMessage::encode_processNotificationRequest()
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST;
    chunkList encoded = encode_label(request_id_, signal_, "");
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

void HTTP3TreeMessage::encode_processNotificationResponse()
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE;
    chunkList encoded = encode_label(request_id_, signal_, "");
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

void HTTP3TreeMessage::encode_getJournalRequest(SequentialNotification const& last_notification)
{
    if (isInitialized_) {
        throw invalid_argument("HTTP3TreeMessage is already initialized");
    }
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_JOURNAL_REQUEST;
    // Just send the uint64_t sequence number in the request
    chunkList encoded = encode_SequentialNotification(request_id_, signal_, 
        {last_notification.first, {"", fplus::maybe<TreeNode>()}});
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.insert(requestChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    isInitialized_ = true;
}

SequentialNotification HTTP3TreeMessage::decode_getJournalRequest()
{
    if (!requestComplete) {
        throw invalid_argument("HTTP3TreeMessage has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    auto decoded = decode_label(requestChunks);
    auto notification = decode_SequentialNotification(chunkList(std::next(requestChunks.begin(), decoded.first), requestChunks.end()));
    if (notification.first == 0) {
        throw invalid_argument("Cannot decode SequentialNotification");
    }
    return notification.second;
}

void HTTP3TreeMessage::encode_getJournalResponse(const std::vector<SequentialNotification>& notifications)
{
    uint8_t signal_ = payload_chunk_header::SIGNAL_WWATP_GET_JOURNAL_RESPONSE;
    chunkList encoded = encode_VectorSequentialNotification(request_id_, signal_, notifications);
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.insert(responseChunks.end(), std::make_move_iterator(encoded.begin()), std::make_move_iterator(encoded.end()));
    responseComplete = true;
}

std::vector<SequentialNotification> HTTP3TreeMessage::decode_getJournalResponse()
{
    if (!responseComplete) {
        throw invalid_argument("HTTP3TreeMessage response has not been fully recieved");
    }
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    auto decoded = decode_VectorSequentialNotification(responseChunks);
    return decoded.second;
}


void HTTP3TreeMessage::setRequestId(uint16_t request_id) {
    request_id_ = request_id;
    for (auto& chunk : requestChunks) {
        chunk.set_request_id(request_id);
    }
    requestComplete = true;
}

fplus::maybe<shared_span<> > HTTP3TreeMessage::popRequestChunk() {
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    if (requestChunks.empty()) {
        return fplus::nothing<shared_span<>>();
    }
    auto chunk = fplus::maybe(requestChunks.front());
    requestChunks.pop_front();
    return chunk;
}

void HTTP3TreeMessage::pushRequestChunk(shared_span<> chunk) {
    uint8_t signal = chunk.get_signal_signal();
    std::lock_guard<std::mutex> lock(requestChunksMutex);
    requestChunks.push_back(chunk);
    if (requestChunks.size() == 1) {
        request_id_ = chunk.get_request_id();
        signal_ = signal;
        isInitialized_ = true;
    }
    auto can_decode = fplus::maybe<size_t>();
    switch (signal) {
        case payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_UPSERT_NODE_REQUEST:
            can_decode = can_decode_VectorTreeNode(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_PAGE_TREE_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST:
            can_decode = can_decode_MaybeTreeNode(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST:
            can_decode = can_decode_Transaction(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_FULL_TREE_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_REGISTER_LISTENER_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            // If decoding the label fails, then MaybeTreeNode will fail too, so attempting 
            // decode at index 0 is OK here.
            can_decode = can_decode_MaybeTreeNode(can_decode.get_with_default(0), requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST:
            can_decode = can_decode_label(0, requestChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_JOURNAL_REQUEST:
            can_decode = can_decode_VectorSequentialNotification(0, requestChunks);
            break;
        default:
            throw invalid_argument("Unknown signal");
    }
    if (can_decode.is_just()) {
        requestComplete = true;
    }
}

fplus::maybe<shared_span<> > HTTP3TreeMessage::popResponseChunk() {
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    if (responseChunks.empty()) {
        if (responseComplete) {
            processingFinished = true;
        }
        return fplus::nothing<shared_span<>>();
    }
    auto chunk = fplus::maybe(responseChunks.front());
    responseChunks.pop_front();
    return chunk;
}

void HTTP3TreeMessage::pushResponseChunk(shared_span<> chunk) {
    uint8_t signal = chunk.get_signal_signal();
    std::lock_guard<std::mutex> lock(responseChunksMutex);
    responseChunks.push_back(chunk);
    auto can_decode = fplus::maybe<size_t>();
    switch (signal) {
        case payload_chunk_header::SIGNAL_WWATP_GET_NODE_RESPONSE:
            can_decode = can_decode_MaybeTreeNode(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_UPSERT_NODE_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE:
            can_decode = can_decode_VectorTreeNode(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_RESPONSE:
            can_decode = can_decode_VectorTreeNode(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_FULL_TREE_RESPONSE:
            can_decode = can_decode_VectorTreeNode(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE:
            can_decode = can_decode_label(0, responseChunks);
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_JOURNAL_RESPONSE:
            can_decode = can_decode_VectorSequentialNotification(0, responseChunks);
            break;
        default:
            throw invalid_argument("Unknown signal");
    }
    if (can_decode.is_just()) {
        responseComplete = true;
    }
}

void HTTP3TreeMessage::reset() {
    request_id_ = 0;
    isInitialized_ = false;
    requestComplete = false;
    responseComplete = false;
    requestChunks.clear();
    responseChunks.clear();
}