// This program is a test program for the shared_span class, which is a class that keeps shared_ptrs to contiguous memory chunks in order,
// and the start and end of the span. 

// This program creates various shared_span objects, copies some "locatable" data into them, and then
// verifies that operations on the shared_span objects work as expected.

#include <span>
#include <iostream>

#include "shared_chunk.h"

const size_t memoryBlockSize = 2400;
uint8_t memoryBlock0[memoryBlockSize];
uint8_t memoryBlockA[memoryBlockSize];

void init_sizes() {
    // Also wish to test strings
    memory_pool.setPoolSize<shared_span<>>(1000);
}

void init_MemoryBlock() {
    for (size_t i = 0; i < memoryBlockSize; i++) {
        memoryBlock0[i] = i % 256;
    }
    for (size_t i = 0; i < memoryBlockSize; i++) {
        memoryBlockA[i] = (i + 'A') % 256;
    }
}

bool check_span_data(shared_span<> const& span, std::span<const uint8_t> data) {
    using const_span_iterator_type = shared_span<>::const_iterator;
    const_span_iterator_type iterator = span.begin();
    for (size_t i = 0; i < data.size(); i++) {
        if (*iterator != data[i]) {
            return false;
        }
        ++iterator;
    }
    return true;
}

bool copy_data_into_span(shared_span<>& span, std::span<const uint8_t> data) {
    span.copy_from_span(data);
    return check_span_data(span, data);
}

bool test_shared_span_constructor() {
    shared_span<> span;
    const size_t UDPChunkSize = shared_span<>::chunk_size;
    return copy_data_into_span(span, std::span<const uint8_t>(memoryBlock0, UDPChunkSize));
}

bool test_update_shared_span_segment() {
    shared_span<> span;
    const size_t UDPChunkSize = shared_span<>::chunk_size;
    copy_data_into_span(span, std::span<const uint8_t>(memoryBlock0, UDPChunkSize));
    auto restricted_span = span.restrict(make_pair(7, 5));
    auto remaining_span = span.restrict(make_pair(7+5, UDPChunkSize-5-7));
    if (!check_span_data(restricted_span, std::span<const uint8_t>(memoryBlock0 + 7, 5))) {
        return false;
    }
    if (!check_span_data(remaining_span, std::span<const uint8_t>(memoryBlock0 + 7 + 5, UDPChunkSize-5-7))) {
        return false;
    }
    copy_data_into_span(restricted_span, std::span<const uint8_t>(memoryBlockA+7, 5));
    auto restricted_span2 = span.restrict(make_pair(7, 5));
    if (!check_span_data(restricted_span2, std::span<const uint8_t>(memoryBlockA + 7, 5))) {
        return false;
    }
    if (!check_span_data(remaining_span, std::span<const uint8_t>(memoryBlock0 + 7 + 5, UDPChunkSize-5-7))) {
        return false;
    }
    copy_data_into_span(restricted_span2, std::span<const uint8_t>(memoryBlock0+7, 5));
    return check_span_data(span, std::span<const uint8_t>(memoryBlock0, UDPChunkSize));
}

bool test_shared_span_restrict() {
    shared_span<> span;
    const size_t UDPChunkSize = shared_span<>::chunk_size;
    copy_data_into_span(span, std::span<const uint8_t>(memoryBlock0, shared_span<>::chunk_size));
    auto restricted_span = span.restrict(make_pair(7, 5));
    auto remaining_span = span.restrict(make_pair(7+5, UDPChunkSize-5-7));
    if (!check_span_data(restricted_span, std::span<const uint8_t>(memoryBlock0 + 7, 5))) {
        return false;
    }
    if (!check_span_data(remaining_span, std::span<const uint8_t>(memoryBlock0 + 7 + 5, UDPChunkSize-5-7))) {
        return false;
    }
    return true;
}

bool test_shared_span_copy_from_span() {
    shared_span<> span;
    return copy_data_into_span(span, std::span<const uint8_t>(memoryBlock0, shared_span<>::chunk_size));
}

bool test_shared_span_use_chunk() {
    shared_span<> span;
    span.copy_from_span(std::span<const uint8_t>((uint8_t*)"Hello, World!", 13));
    auto chunk = span.use_chunk();
    if (memcmp(chunk->data, "Hello, World!", 13) != 0) {
        return false;
    }
    memcpy(chunk->data, "Goodbye, World!", 14);
    return check_span_data(span, std::span<const uint8_t>((uint8_t*)"Goodbye, World!", 14));
}

// Also need to test that appending spans together creates the right byte sequences
bool test_shared_span_append() {
    shared_span<> span;
    shared_span<> span2;
    copy_data_into_span(span, std::span<const uint8_t>(memoryBlock0, shared_span<>::chunk_size));
    copy_data_into_span(span2, std::span<const uint8_t>(memoryBlockA, shared_span<>::chunk_size));
    auto appended_span = span.append(span2);
    // Now create two subspans from the appended_span and check that they are the same as the original spans
    auto restricted_span = appended_span.restrict(make_pair(200, shared_span<>::chunk_size-200));
    auto remaining_span = appended_span.restrict(make_pair(shared_span<>::chunk_size, shared_span<>::chunk_size-200));
    if (!check_span_data(restricted_span, std::span<const uint8_t>(memoryBlock0+200, shared_span<>::chunk_size-200))) {
        return false;
    }
    return check_span_data(remaining_span, std::span<const uint8_t>(memoryBlockA, shared_span<>::chunk_size-200));
}

// The following is the same as the above test, except use the span constructor with the full 2400 length memoryBlock0
bool test_shared_span_copy_more() {
    shared_span<> span(std::span<const uint8_t>(memoryBlock0, 2400));
    // Now create two subspans from the appended_span and check that they are the same as the original data
    auto restricted_span = span.restrict(make_pair(0, 1200));
    auto remaining_span = span.restrict(make_pair(1200, 1200));
    if (!check_span_data(restricted_span, std::span<const uint8_t>(memoryBlock0, 1200))) {
        return false;
    }
    if (!check_span_data(remaining_span, std::span<const uint8_t>(memoryBlock0+1200, 1200))) {
        return false;
    }
    auto more_restricted_span = restricted_span.restrict(make_pair(600, 600));
    if (!check_span_data(more_restricted_span, std::span<const uint8_t>(memoryBlock0+600, 600))) {
        return false;
    }
    auto more_remaining_span = remaining_span.restrict(make_pair(0, 600));
    if (!check_span_data(more_remaining_span, std::span<const uint8_t>(memoryBlock0+1200, 600))) {
        return false;
    }
    auto combined_span = more_restricted_span.append(more_remaining_span);
    if (!check_span_data(combined_span, std::span<const uint8_t>(memoryBlock0+600, 1200))) {
        return false;
    }
    auto middle_span = combined_span.restrict(make_pair(300, 600));
    if (!check_span_data(middle_span, std::span<const uint8_t>(memoryBlock0+900, 600))) {
        return false;
    }
    auto trailing_span = middle_span.restrict(make_pair(301, 100));
    if (!check_span_data(trailing_span, std::span<const uint8_t>(memoryBlock0+1201, 100))) {
        return false;
    }
    return true;
}

int main() {
    init_sizes();
    cout << "Sizes initialized" << endl << flush;
    init_MemoryBlock();
    cout << "Memory block initialized" << endl << flush;
    if (!test_shared_span_constructor()) {
        return 1;
    }
    cout << "Shared span constructor test passed" << endl << flush;
    if (!test_shared_span_copy_from_span()) {
        return 1;
    }
    cout << "Shared span copy from span test passed" << endl << flush;
    if (!test_shared_span_restrict()) {
        return 1;
    }
    cout << "Shared span restrict test passed" << endl << flush;
    if (!test_update_shared_span_segment()) {
        return 1;
    }
    cout << "Shared span update segment test passed" << endl << flush;
    if (!test_shared_span_use_chunk()) {
        return 1;
    }
    cout << "Shared span use chunk test passed" << endl << flush;
    if (!test_shared_span_copy_more()) {
        return 1;
    }
    cout << "All tests passed!" << endl << flush;
    return 0;
}