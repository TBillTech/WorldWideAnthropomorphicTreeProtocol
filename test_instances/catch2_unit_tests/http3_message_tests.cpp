#include <catch2/catch_all.hpp>
#include "http3_tree_message_helpers.h"

// Example test for encode_label/decode_label
TEST_CASE("encode_label and decode_label roundtrip", "[http3_tree_message_helpers]") {
    uint16_t request_id = 42;
    uint8_t signal = 7;
    std::string label = "test_label";

    chunkList encoded = encode_label(request_id, signal, label);
    auto decoded = decode_label(encoded);

    REQUIRE(decoded.second == label);
}

// Add more tests for other helpers as needed