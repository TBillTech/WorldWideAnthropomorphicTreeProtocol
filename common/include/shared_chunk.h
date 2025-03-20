#pragma once

#include "memory_pool.h"
#include <memory>
#include <span>
#include <vector>
#include <stdexcept>

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

template<class ChunkType = UDPChunk>
class shared_span {
public:
    typedef ChunkType chunk_type;
    static constexpr size_t chunk_size = ChunkType::chunk_size;

    shared_span(shared_span &other) : chunks(other.chunks) {
    }
    // the default constructor uses the memory pool to allocate a new chunk (the memory pool is parameterized by the chunk type)
    shared_span() {
        auto chunk = memory_pool.allocate<ChunkType>();
        chunks.emplace_back(shared_ptr<ChunkType>(chunk, [](ChunkType* ptr) { memory_pool.deallocate(ptr); }), make_pair(0, ChunkType::chunk_size));
    }
    // Move constructor
    shared_span(shared_span &&other) : chunks(move(other.chunks)) {
    }
    // Constructor that takes a span of bytes and copies them into the shared_span
    shared_span(std::span<const uint8_t> data) {
        size_t data_offset = 0;
        while (data_offset < data.size()) {
            if (chunks.empty() || chunks.back().second.second == chunks.back().first->chunk_size) {
                auto chunk = memory_pool.allocate<ChunkType>();
                chunks.emplace_back(shared_ptr<ChunkType>(chunk, [](ChunkType* ptr) { memory_pool.deallocate(ptr); }), make_pair(0, 0));
            }
            auto& last_chunk = chunks.back();
            size_t chunk_space = last_chunk.first->chunk_size - last_chunk.second.second;
            size_t copy_size = min(chunk_space, data.size() - data_offset);
            memcpy(last_chunk.first->data + last_chunk.second.second, data.data() + data_offset, copy_size);
            last_chunk.second.second += copy_size;
            data_offset += copy_size;
        }
    }

    shared_span restrict(pair<size_t, size_t> range) {
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
    size_t copy_from_span(std::span<const uint8_t> data) {
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
    ChunkType* use_chunk() {
        if (chunks.size() != 1) {
            throw invalid_argument("use_chunk can only be called on a shared_span with one chunk");
        }
        return chunks[0].first.get();
    }

    shared_span append(shared_span &other) {
        shared_span new_span(*this);
        new_span.chunks.insert(new_span.chunks.end(), other.chunks.begin(), other.chunks.end());
        return move(new_span);
    }

    // Begin returns an iterator class that knows how to increment from the end of the span of one chunk to the start of the span of the next chunk.
    class iterator {
    public:
        iterator(vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> &chunks) : chunks(chunks), chunk_index(0), span_index(0) {
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
        iterator(vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> &chunks, size_t chunk_index, size_t span_index) : chunks(chunks), chunk_index(chunk_index), span_index(span_index) {
        }
        iterator(iterator &other) : chunks(other.chunks), chunk_index(other.chunk_index), span_index(other.span_index) {
        }
        iterator& operator++() {
            if (chunk_index == chunks.size()) {
                span_index = 0;
                return *this;
            }
            // If the span_index + 1 >= chunk start position + chunk offset length, then move to the next chunk
            if (span_index + 1 >= chunks[chunk_index].second.first + chunks[chunk_index].second.second) {
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
            } else {
                span_index++;
            }
            return *this;
        }
        bool operator!=(iterator const &other) const {
            return &chunks != &other.chunks || chunk_index != other.chunk_index || span_index != other.span_index;
        }
        uint8_t& operator*() {
            return chunks[chunk_index].first->data[span_index];
        }
    private:
        vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> &chunks;
        size_t chunk_index;
        size_t span_index;
    };
    iterator begin() {
        return iterator(chunks);
    }
    iterator end() {
        return iterator(chunks, chunks.size(), 0);
    }
    class const_iterator {
    public:
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
        const_iterator(const_iterator const &other) : chunks(other.chunks), chunk_index(other.chunk_index), span_index(other.span_index) {
        }
        const_iterator& operator++() {
            if (chunk_index == chunks.size()) {
                span_index = 0;
                return *this;
            }
            // If the span_index + 1 >= chunk start position + chunk offset length, then move to the next chunk
            if (span_index + 1 >= chunks[chunk_index].second.first + chunks[chunk_index].second.second) {
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
            } else {
                span_index++;
            }
            return *this;
        }
        bool operator!=(const_iterator const &other) const {
            return &chunks != &other.chunks || chunk_index != other.chunk_index || span_index != other.span_index;
        }
        uint8_t const & operator*() {
            return chunks[chunk_index].first->data[span_index];
        }
    private:
        vector<pair<shared_ptr<ChunkType>, pair<size_t, size_t>>> const &chunks;
        size_t chunk_index;
        size_t span_index;
    };
    // The const version of the begin function uses a const iterator
    const_iterator begin() const {
        return const_iterator(chunks);
    }
    const_iterator end() const {
        return const_iterator(chunks, chunks.size(), 0);
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
