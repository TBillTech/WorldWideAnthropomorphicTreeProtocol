#pragma once

#include <memory>
#include <span>
#include <vector>
#include <stdexcept>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/core/demangle.hpp>
#include <type_traits>

#include "memory_pool.h"

// This file defines the shared_span class, which keeps shared_ptrs to contiguous memory chunks in order, and the start and end of the span.
// The underlying chunks are usually 1200, but can technically be any size if an alternative chunk type is provided.
// Since the shared span class contains shared_ptrs to chunks, when the last shared_span goes out of scope, any referenced chunks will be deallocated.
// There is also here defined a shared_chunk class, which is a utility class helping to allocate the chunk, and technically defines a span for the whole chunk.

using namespace std;

template<const int size>
class SizedChunk {
public:
    static constexpr size_t chunk_size = static_cast<size_t>(size);
    uint8_t data[chunk_size];
};

using UDPChunk = SizedChunk<1200>;

template <typename ChunkType, typename PODType>
PODType pod_from_chunk(typename std::remove_const<PODType>::type &result, vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks
        , pair<bool, pair<size_t, size_t>> start = {false, {0, 0}}) {
    size_t byte_offset = chunks.begin()->second.first;
    size_t current_chunk = 0;
    if (start.first) {
        byte_offset = start.second.first;
        current_chunk = start.second.second;
    }
    for (size_t pod_offset = 0; pod_offset < sizeof(PODType);) {
        if (current_chunk >= chunks.size()) {
            throw invalid_argument("Not enough chunks to copy the data type " + boost::core::demangle(typeid(PODType).name()));
        }
        auto& chunk = chunks.at(current_chunk);
        size_t chunk_space = chunk.second.second;
        size_t to_copy = min(sizeof(PODType) - pod_offset, chunk_space + chunk.second.first - byte_offset);
        memcpy(reinterpret_cast<uint8_t*>(&result) + pod_offset, chunk.first->data + byte_offset, to_copy);
        pod_offset += to_copy;
        if (pod_offset == sizeof(PODType)) {
            if (byte_offset + to_copy < chunk_space) {
                return result;
            }
            current_chunk++;
            if (current_chunk <= chunks.size()) {
                byte_offset = chunks.at(current_chunk).second.first;
            }
            return result;
        }
        current_chunk++;
        if (current_chunk <= chunks.size()) {
            byte_offset = chunks.at(current_chunk).second.first;
        }
    }
    return result;
}

template<class ChunkType = UDPChunk>
class shared_span {
public:
    typedef ChunkType chunk_type;
    static constexpr size_t chunk_size = ChunkType::chunk_size;

    shared_span(const shared_span &other) : chunks(other.chunks) {
    }
    // the default constructor uses the memory pool to allocate a new chunk (the memory pool is parameterized by the chunk type)
    shared_span(bool allocate = false) {
        if (allocate) {
            auto chunk = memory_pool.allocate<ChunkType>();
            chunks.emplace_back(shared_ptr<ChunkType>(chunk, [](ChunkType* ptr) { memory_pool.deallocate(ptr); }), make_pair(0, ChunkType::chunk_size));
        }
    }
    // Move constructor
    shared_span(shared_span &&other) : chunks(move(other.chunks)) {
    }
    // Constructor that takes a span of bytes and copies them into the shared_span
    template <typename PODType>
    shared_span(const std::span<PODType> data) {
        size_t byte_offset = 0;
        while (byte_offset < data.size()*sizeof(PODType)) {
            if (chunks.empty() || chunks.back().second.second == chunks.back().first->chunk_size) {
                auto chunk = memory_pool.allocate<ChunkType>();
                chunks.emplace_back(shared_ptr<ChunkType>(chunk, [](ChunkType* ptr) { memory_pool.deallocate(ptr); }), make_pair(0, 0));
            }
            auto& last_chunk = chunks.back();
            size_t chunk_space = last_chunk.first->chunk_size - last_chunk.second.second;
            size_t copy_size = min(chunk_space, (data.size() * sizeof(PODType) - byte_offset));
            memcpy(last_chunk.first->data + last_chunk.second.second, reinterpret_cast<const uint8_t*>(data.data()) + byte_offset, copy_size);
            last_chunk.second.second += copy_size;
            byte_offset += copy_size;
        }
    }

    shared_span restrict(pair<size_t, size_t> range) const {
        shared_span new_span(*this);
        size_t restricted_start = range.first;
        size_t restricted_length = range.second;
        size_t current_chunk = 0;
        while(current_chunk < new_span.chunks.size()) {
            auto& chunk = new_span.chunks.at(current_chunk);
            if (restricted_start + restricted_length <= chunk.second.second) {
                // The whole restricted range is in the current chunk
                chunk.second.first = chunk.second.first + restricted_start;
                chunk.second.second = restricted_length;
                new_span.chunks.resize(current_chunk + 1);
                return move(new_span);
            }
            if (restricted_start >= chunk.second.second) {
                // The restricted range is not in this chunk
                new_span.chunks.erase(new_span.chunks.begin());
                restricted_start -= chunk.second.second;
                continue;
            }
            // The restricted range starts in this chunk, but extends into the next chunk
            chunk.second.first = chunk.second.first + restricted_start;
            chunk.second.second = chunk.second.second - restricted_start;
            restricted_length -= chunk.second.second;
            restricted_start = 0;
            current_chunk++;
        }
        // The loop ran out of chunks, and so the resulting span will be shorter than the requested range
        return move(new_span);
    }

    // Method to copy a span of bytes into the shared_span.  Will overwrite data, but NEVER exceed the bounds of the span.
    // Will return the number of bytes written to the span.
    size_t copy_from_span(std::span<const uint8_t> data) const {
        size_t data_offset = 0;
        size_t current_chunk = 0;
        while (data_offset < data.size()) {
            if (current_chunk >= chunks.size()) {
                return data_offset;
            }
            auto& chunk = chunks.at(current_chunk);
            size_t chunk_space = chunk.second.second;
            size_t copy_size = min(chunk_space, data.size() - data_offset);
            memcpy(chunk.first->data + chunk.second.first, data.data() + data_offset, copy_size);
            data_offset += copy_size;
        }
        return data_offset;
    }

    // Warning, this should ONLY be used if the shared_span is kept alive, as the underlying chunk will be deallocated when the last shared_span goes out of scope.
    // This is useful for passing to compatible libraries.
    ChunkType* use_chunk() const {
        if (chunks.size() != 1) {
            throw invalid_argument("use_chunk can only be called on a shared_span with one chunk");
        }
        return chunks[0].first.get();
    }

    shared_span append(shared_span &other) const {
        shared_span new_span(*this);
        new_span.chunks.insert(new_span.chunks.end(), other.chunks.begin(), other.chunks.end());
        return move(new_span);
    }

    template <typename PODType>
    pair<size_t, size_t> copy_type(PODType data, pair<bool, pair<size_t, size_t>> start = {false, {0, 0}}) const {
        size_t byte_offset = chunks.begin()->second.first;
        size_t current_chunk = 0;
        if (start.first) {
            byte_offset = start.second.first;
            current_chunk = start.second.second;
        }
        for (size_t pod_offset = 0; pod_offset < sizeof(PODType);) {
            if (current_chunk >= chunks.size()) {
                throw invalid_argument("Not enough chunks to copy the data type " + boost::core::demangle(typeid(PODType).name()));
            }
            auto& chunk = chunks.at(current_chunk);
            size_t chunk_space = chunk.second.second;
            size_t to_copy = min(sizeof(PODType) - pod_offset, chunk_space + chunk.second.first - byte_offset);
            memcpy(chunk.first->data + byte_offset, reinterpret_cast<uint8_t*>(&data) + pod_offset, to_copy);
            pod_offset += to_copy;
            if (pod_offset == sizeof(PODType)) {
                if (byte_offset + to_copy < chunk_space) {
                    return {byte_offset + to_copy, current_chunk};
                }
                current_chunk++;
                if (current_chunk <= chunks.size()) {
                    byte_offset = chunks.at(current_chunk).second.first;
                }
                return {byte_offset + to_copy, current_chunk};
            }
            current_chunk++;
            if (current_chunk <= chunks.size()) {
                byte_offset = chunks.at(current_chunk).second.first;
            }
        }
        return {byte_offset, current_chunk};
    }

    template <typename PODType>
    void copy_span(span<PODType> data) const {
        pair<bool, pair<size_t, size_t>> start = {false, {0, 0}};
        for (size_t i = 0; i < data.size(); i++) {
            auto next_start = copy_type(data[i], start);
            start = {true, next_start};
        }
    }

    template <typename PODType>
    inline PODType& at(typename std::remove_const<PODType>::type& result, pair<bool, pair<size_t, size_t>> start = {false, {0, 0}}) const {
        pod_from_chunk<ChunkType, typename std::remove_const<PODType>::type>(result, chunks, start);
        return result;
    }

    template <typename PODType>
    class const_iterator : public boost::iterator_facade<
        const_iterator<const PODType>, const PODType, boost::random_access_traversal_tag, const PODType&> {
    public:
        template <typename PODType2>
        friend class const_iterator;
        using ModifyablePODType = typename std::remove_const<PODType>::type;

        const_iterator(vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks) : chunks(chunks), chunk_index(0), span_index(0) {
            if (chunks.size() > 0) {
                while (chunks[chunk_index].second.second == 0) {
                    chunk_index++;
                    if (chunk_index == chunks.size()) {
                        return;
                    }
                }
                span_index = chunks[0].second.first;
            }
        }
        const_iterator(vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks, size_t chunk_index, size_t span_index) : chunks(chunks), chunk_index(chunk_index), span_index(span_index) {
        }
        template <typename PODType2>
        const_iterator(const_iterator<const PODType2> const &other) : chunks(other.chunks), chunk_index(other.chunk_index), span_index(other.span_index) {
        }
        const_iterator& operator++() {
            if (chunk_index == chunks.size()) {
                span_index = 0;
                return *this;
            }
            size_t bytes_to_skip = sizeof(PODType);
            while (bytes_to_skip > 0) {
                size_t chunk_space = chunks[chunk_index].second.first + chunks[chunk_index].second.second - span_index;
                if (bytes_to_skip <= chunk_space) {
                    span_index += bytes_to_skip;
                    if (span_index == chunks[chunk_index].second.first + chunks[chunk_index].second.second) {
                        chunk_index++;
                        if (chunk_index == chunks.size()) {
                            span_index = 0;
                            return *this;
                        }
                        while(chunks[chunk_index].second.second == 0) {
                            chunk_index++;
                            if (chunk_index == chunks.size()) {
                                span_index = 0;
                                return *this;
                            }
                        }
                        span_index = chunks[chunk_index].second.first;
                    }
                    return *this;
                }
                bytes_to_skip -= chunk_space;
                chunk_index++;
                if (chunk_index == chunks.size()) {
                    // Partial PODType at the end of span should throw exception
                    throw invalid_argument("PODType extends past the end of the span, but not enough bytes to complete the PODType");
                }
                while(chunks[chunk_index].second.second == 0) {
                    chunk_index++;
                    if (chunk_index == chunks.size()) {
                        // Partial PODType at the end of span should throw exception
                        throw invalid_argument("PODType extends past the end of the span, but not enough bytes to complete the PODType");
                    }
                }
                span_index = chunks[chunk_index].second.first;
            }
            return *this;
        }
        bool operator!=(const_iterator const &other) const {
            return &chunks != &other.chunks || chunk_index != other.chunk_index || span_index != other.span_index;
        }
        const PODType& operator*() {
            // If the length of the PODType extends past the end of the chunk, then throw an exception
            if (span_index + sizeof(PODType) > chunks[chunk_index].second.first + chunks[chunk_index].second.second) {
                if (chunk_index + 1 > chunk_stitches.size()) {
                    // Then need to create a PODType which correctly stiches the two parts from the consequitive chunks together, and
                    // allows to be returned from this operator
                    if (chunk_index > chunk_stitches.size()){
                        throw invalid_argument("When dereferencing chunks, must dereference spanning chunks in order.");
                    }
                    chunk_stitches.resize(chunk_stitches.size() + 1);
                    pair<bool, pair<size_t, size_t>> start_loc = {true, {span_index, chunk_index}};
                    vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks_ref = chunks;
                    pod_from_chunk<ChunkType, ModifyablePODType>(chunk_stitches[chunk_index], chunks_ref, start_loc);
                }
                return chunk_stitches[chunk_index];
            // Otherwise, it is safe to return the reference to the PODType
            } else {
                return *reinterpret_cast<const PODType*>(chunks[chunk_index].first->data + span_index);
            }
        }
        shared_span<ChunkType> to_span() const {
            // Create a new span, starting with the chunk_index and update the first chunk with via the span_index
            shared_span new_span;
            new_span.chunks.insert(new_span.chunks.end(), chunks.begin() + chunk_index, chunks.end());
            // Then restrict it to the span_index
            shared_span restricted_span = new_span.restrict(make_pair(span_index, new_span.size()));
            return move(restricted_span);
        }
        std::ptrdiff_t distance_to(const_iterator const &other) const {
            if (&chunks != &other.chunks) {
                throw invalid_argument("Cannot compare iterators from different shared_spans");
            }
            // If the other iterator is before this one, then return a negative distance
            if ((chunk_index < other.chunk_index) || ((chunk_index == other.chunk_index) && (span_index < other.span_index))) {
                return -1 * other.distance_to(*this);
            }
            if (chunk_index == other.chunk_index) {
                return (other.span_index - span_index);
            }
            std::ptrdiff_t distance = 0;
            // Distance within this last chunk
            if (chunk_index < chunks.size()) {
                distance -= span_index - chunks[chunk_index].second.first;
            }
            // Distance for the full chunks in between
            for (size_t i = other.chunk_index + 1; i < chunk_index; ++i) {
                distance -= chunks[i].second.second;
            }
            // Distance within the first chunk
            auto& other_chunk = chunks[other.chunk_index];
            distance -= (other_chunk.second.first + other_chunk.second.second - other.span_index);
            return distance / sizeof(PODType);
        }
    private:
        vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks;
        size_t chunk_index;
        size_t span_index;
        vector<ModifyablePODType> chunk_stitches;
    };
    // The const version of the begin function uses a const iterator
    template <typename PODType>
    const_iterator<const PODType> cbegin() const {
        return const_iterator<const PODType>(chunks);
    }
    template <typename PODType>
    const_iterator<const PODType> cend() const {
        return const_iterator<const PODType>(chunks, chunks.size(), 0);
    }
    template <typename PODType>
    auto crange() const {
        struct const_range {
            shared_span const &span;
            const_range(shared_span const &span) : span(span) {}
            decltype(auto) begin() { return span.begin<PODType>(); }
            decltype(auto) end() { return span.end<PODType>(); }
        };
        return const_range(*this);
    }

    // Begin returns an iterator class that knows how to increment from the end of the span of one chunk to the start of the span of the next chunk.
    template <typename PODType>
    class iterator : public boost::iterator_facade<
        iterator<PODType>, PODType, boost::random_access_traversal_tag, PODType&>{
    public:
        template <typename PODType2>
        friend class const_iterator;
        template <typename PODType2>
        friend class iterator;
        iterator(vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks) : chunks(chunks), chunk_index(0), span_index(0) {
            if (chunks.size() > 0) {
                while (chunks[chunk_index].second.second == 0) {
                    chunk_index++;
                    if (chunk_index == chunks.size()) {
                        return;
                    }
                }
                span_index = chunks[0].second.first;
            }
        }
        iterator(vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks, size_t chunk_index, size_t span_index) : chunks(chunks), chunk_index(chunk_index), span_index(span_index) {
        }
        template <typename PODType2>
        iterator(iterator<PODType2> const &other) : chunks(other.chunks), chunk_index(other.chunk_index), span_index(other.span_index) {
        }
        iterator& operator++() {
            if (chunk_index == chunks.size()) {
                span_index = 0;
                return *this;
            }
            size_t bytes_to_skip = sizeof(PODType);
            while (bytes_to_skip > 0) {
                size_t chunk_space = chunks[chunk_index].second.first + chunks[chunk_index].second.second - span_index;
                if (bytes_to_skip <= chunk_space) {
                    span_index += bytes_to_skip;
                    if (span_index == chunks[chunk_index].second.first + chunks[chunk_index].second.second) {
                        chunk_index++;
                        if (chunk_index == chunks.size()) {
                            span_index = 0;
                            return *this;
                        }
                        while(chunks[chunk_index].second.second == 0) {
                            chunk_index++;
                            if (chunk_index == chunks.size()) {
                                span_index = 0;
                                return *this;
                            }
                        }
                        span_index = chunks[chunk_index].second.first;
                    }
                    return *this;
                }
                bytes_to_skip -= chunk_space;
                chunk_index++;
                if (chunk_index == chunks.size()) {
                    // Partial PODType at the end of span should throw exception
                    throw invalid_argument("PODType extends past the end of the span, but not enough bytes to complete the PODType");
                }
                while(chunks[chunk_index].second.second == 0) {
                    chunk_index++;
                    if (chunk_index == chunks.size()) {
                        // Partial PODType at the end of span should throw exception
                        throw invalid_argument("PODType extends past the end of the span, but not enough bytes to complete the PODType");
                    }
                }
                span_index = chunks[chunk_index].second.first;
            }
            return *this;
        }
        bool operator!=(iterator const &other) const {
            return &chunks != &other.chunks || chunk_index != other.chunk_index || span_index != other.span_index;
        }
        PODType& operator*() const {
            // If the length of the PODType extends past the end of the chunk, then throw an exception
            if (span_index + sizeof(PODType) > chunks[chunk_index].second.first + chunks[chunk_index].second.second) {
                // non const iterator dereferencing of PODTypes that cross between chunks is not allowed, 
                // because it would allow writing past the end of the chunk into other memory 
                throw invalid_argument("PODType extends past the end of the span");
            // Otherwise, it is safe to return the reference to the PODType
            } else {
                return *reinterpret_cast<PODType*>(chunks[chunk_index].first->data + span_index);
            }
        }
        shared_span<ChunkType> to_span() const {
            // Create a new span, starting with the chunk_index and update the first chunk with via the span_index
            shared_span new_span;
            new_span.chunks.insert(new_span.chunks.end(), chunks.begin() + chunk_index, chunks.end());
            // Then restrict it to the span_index
            shared_span restricted_span = new_span.restrict(make_pair(span_index, new_span.size()));
            return move(restricted_span);
        }
        // Conversion operator to const_iterator
        template <typename PODType2>
        operator const_iterator<PODType2>() const {
            return const_iterator(chunks, chunk_index, span_index);
        }
        std::ptrdiff_t distance_to(iterator const &other) const {
            if (&chunks != &other.chunks) {
                throw invalid_argument("Cannot compare iterators from different shared_spans");
            }
            // If the other iterator is before this one, then return a negative distance
            if ((chunk_index < other.chunk_index) || ((chunk_index == other.chunk_index) && (span_index < other.span_index))) {
                return -1 * other.distance_to(*this);
            }
            if (chunk_index == other.chunk_index) {
                return (other.span_index - span_index);
            }
            std::ptrdiff_t distance = 0;
            // Distance within the current chunk
            distance += (chunks[chunk_index].second.first + chunks[chunk_index].second.second - span_index);
            // Distance for the full chunks in between
            for (size_t i = chunk_index + 1; i < other.chunk_index; ++i) {
                distance += chunks[i].second.second;
            }
            // Distance within the last chunk
            distance += other.span_index;
            return distance / sizeof(PODType);
        }
    private:
        vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks;
        size_t chunk_index;
        size_t span_index;
    };

    template <typename PODType>
    auto begin() const {
        if constexpr (std::is_const_v<PODType>) {
            return cbegin<PODType>();
        } else {
            return iterator<PODType>(chunks);
        }
    }

    template <typename PODType>
    auto end() const {
        if constexpr (std::is_const_v<PODType>) {
            return cend<PODType>();
        } else {
            return iterator<PODType>(chunks, chunks.size(), 0);
        }
    }

    template <typename PODType>
    auto range() const {
        struct _range {
            shared_span const &span;
            _range(shared_span const &span) : span(span) {}
            decltype(auto) begin() { return span.begin<PODType>(); }
            decltype(auto) end() { return span.end<PODType>(); }
        };
        return _range(*this);        
    }

    size_t size() const {
        size_t total = 0;
        for (auto& chunk : chunks) {
            total += chunk.second.second;
        }
        return total;
    }
private:
    vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> chunks;
};
