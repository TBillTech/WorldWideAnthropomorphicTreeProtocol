#include <catch2/catch_all.hpp>
#include "http3_tree_message_helpers.h"
#include "backend_testbed.h"

void test_encode_decode_label(uint16_t request_id, uint8_t signal, const std::string& label) {
    chunkList empty_chunk_list;
    REQUIRE(can_decode_label(0, empty_chunk_list).is_nothing());

    chunkList encoded = encode_label(request_id, signal, label);
    REQUIRE(encoded.size() == 1);
    REQUIRE(can_decode_label(0, encoded).is_just());
    REQUIRE(can_decode_label(0, encoded).unsafe_get_just() == encoded.size());
    auto decoded = decode_label(encoded);
    REQUIRE(decoded.first == 1);
    REQUIRE(decoded.second == label);
}

TEST_CASE("encode_label and decode_label roundtrip", "[http3_tree_message_helpers]") {
    // Test several labels with different lengths and characters
    test_encode_decode_label(42, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, "");
    test_encode_decode_label(1, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, "test_label");
    test_encode_decode_label(2, payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_REQUEST, "another_test_label");
    test_encode_decode_label(3, payload_chunk_header::SIGNAL_WWATP_GET_PAGE_TREE_REQUEST, 
        "a_very_long_label_that_exceeds_normal_length");
    test_encode_decode_label(4, payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_REQUEST, 
        "label_with_special_characters_!@#$%^&*()_+");
    // url like label:
    test_encode_decode_label(5, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, 
        "https://example.com/path/to/resource?query=param#fragment");
}

template <typename encoded_type>
void test_encode_decode_type(uint16_t request_id, uint8_t signal, const encoded_type& obj,
    std::function<chunkList(uint16_t, uint8_t, const encoded_type&)> encode_func,
    std::function<std::pair<size_t, encoded_type>(chunkList)> decode_func,
    std::function<fplus::maybe<size_t>(size_t, chunkList)> can_decode_func) {
    chunkList empty_chunk_list;
    REQUIRE(can_decode_func(0, empty_chunk_list).is_nothing());

    chunkList encoded = encode_func(request_id, signal, obj);
    string multi_chunk_string(5000, 'D');
    chunkList prior_chunks = encode_long_string(request_id, signal, multi_chunk_string);
    auto prior_count = prior_chunks.size();
    // create a chunk list from the prior_chunks concatenated with the encoded:
    prior_chunks.insert(prior_chunks.end(), encoded.begin(), encoded.end());
    // make sure that any sub list returns false from can decode
    for (size_t i = 0; i < encoded.size(); ++i) {
        // Get a list with all elements up to the ith element
        chunkList sub_encoded(encoded.begin(), std::next(encoded.begin(), i));
        REQUIRE(can_decode_func(i, sub_encoded).is_nothing());
    }
    REQUIRE(can_decode_func(prior_count, prior_chunks).is_just());
    REQUIRE(can_decode_func(prior_count, prior_chunks).unsafe_get_just() == prior_chunks.size());
    auto decoded = decode_func(encoded);
    REQUIRE(decoded.first == encoded.size());
    REQUIRE(decoded.second == obj);
}

TEST_CASE("encode_long_string and decode_long_string roundtrip", "[http3_tree_message_helpers]") {
    using encoded_type = std::string;
    auto test_encode_decode = [](uint16_t request_id, uint8_t signal, const encoded_type& str) {
        return test_encode_decode_type<encoded_type>(request_id, signal, str,
            encode_long_string, decode_long_string, can_decode_long_string);
    };
    // Test several strings with different lengths and characters
    test_encode_decode(42, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, "");
    test_encode_decode(1, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, "short string");
    test_encode_decode(2, payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_REQUEST, 
        "a slightly longer string that should still fit in one chunk");
    test_encode_decode(4, payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_REQUEST, 
        "string_with_special_characters_!@#$%^&*()_+");
    // url like string:
    test_encode_decode(5, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, 
        "https://example.com/path/to/resource?query=param#fragment");
    // But we also need to test some strings that are longer than the chunk size
    std::string long_string(2000, 'a'); // 2000 characters of 'a'
    test_encode_decode(6, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, long_string);
    std::string very_long_string(5000, 'b'); // 5000 characters of 'b'
    test_encode_decode(7, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, very_long_string);
}

TEST_CASE("encode_MaybeTreeNode and decode_MaybeTreeNode roundtrip", "[http3_tree_message_helpers]") {
    using encoded_type = fplus::maybe<TreeNode>;
    auto test_encode_decode = [](uint16_t request_id, uint8_t signal, const encoded_type& maybe_tree_node) {
        return test_encode_decode_type<encoded_type>(request_id, signal, maybe_tree_node,
            encode_MaybeTreeNode, decode_MaybeTreeNode, can_decode_MaybeTreeNode);
    };
    // Test several TreeNodes with different properties
    fplus::maybe<TreeNode> nada;
    test_encode_decode(42, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, nada);

    auto simpleAnimal = createAnimalNode("Sponge", "Bottom Feeder", {}, 
        {1, 1}, {}, {}, "", "");
    fplus::maybe<TreeNode> just_simple_animal(simpleAnimal);
    test_encode_decode(2, payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_REQUEST, just_simple_animal);

    auto anAnimal = createAnimalNode("Seal", "A marine mammal", {"Mammal"}, 
        {1, 1}, {"pup_1", "pup_2"}, {{1, "pup 1 dossier"}, {2, "pup 2 dossier"}}, 
        "How to query seal", "Seal QA sequence");
    fplus::maybe<TreeNode> just_animal(anAnimal);
    test_encode_decode(3, payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_REQUEST, just_animal);
    
}


TEST_CASE("encode_SequentialNotification and decode_SequentialNotification roundtrip", "[http3_tree_message_helpers]") {
    using encoded_type = SequentialNotification;
    auto test_encode_decode = [](uint16_t request_id, uint8_t signal, const encoded_type& notification) {
        return test_encode_decode_type<encoded_type>(request_id, signal, notification,
            encode_SequentialNotification, decode_SequentialNotification, can_decode_SequentialNotification);
    };
    // Test several SequentialNotifications with different properties
    SequentialNotification empty_notification(0, Notification("", fplus::maybe<TreeNode>()));
    test_encode_decode(42, payload_chunk_header::SIGNAL_WWATP_GET_NODE_REQUEST, empty_notification);

    SequentialNotification simple_notification(10, Notification("/lion/pup_1", fplus::maybe<TreeNode>()));
    test_encode_decode(1, payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_REQUEST, simple_notification);

    // More complex notification
    TreeNode complex_node = createAnimalNode("https://example.com/path/to/lion?query=param#fragment", "A complex animal", {"Mammal"}, 
        {2, 2}, {"pup_1", "pup_2"}, {{1, "pup 1 dossier"}, {2, "pup 2 dossier"}}, 
        "How to query complex animal", "Complex Animal QA sequence");
    SequentialNotification complex_notification(13, Notification(complex_node.getLabelRule(), fplus::maybe<TreeNode>(complex_node)));
    test_encode_decode(2, payload_chunk_header::SIGNAL_WWATP_GET_JOURNAL_RESPONSE, complex_notification);
}