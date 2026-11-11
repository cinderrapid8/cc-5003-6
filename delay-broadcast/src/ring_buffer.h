#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace dbs {

// Bounded single-producer/single-consumer ring of mono float frames.
// All positions are absolute int64 frame counters; physical storage wraps
// modulo the (power-of-two) capacity. Content that has already been output
// is overwritten as the input head advances, so stale material can never be
// replayed to fill an underrun.
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity)
        : mask_(roundPow2(capacity) - 1), storage_(mask_ + 1, 0.0f) {
        if (capacity < 2) throw std::invalid_argument("ring capacity must be >= 2");
    }

    std::size_t capacity() const { return storage_.size(); }

    // Frames currently held between outputHead and writeHead.
    std::int64_t buffered(std::int64_t writeHead, std::int64_t outputHead) const {
        return writeHead - outputHead;
    }

    // Write incoming frames starting at absolute position writeHead.
    // Returns number of frames actually stored; frames beyond the bounded
    // capacity are rejected (caller applies back-pressure), never overwriting
    // not-yet-output material.
    std::int64_t write(const float* src, std::int64_t writeHead, std::int64_t outputHead,
                       std::int64_t maxFrames) {
        const std::int64_t room = static_cast<std::int64_t>(storage_.size()) -
                                  (writeHead - outputHead);
        const std::int64_t n = std::min(std::min(maxFrames, room),
                                        static_cast<std::int64_t>(storage_.size()));
        for (std::int64_t i = 0; i < n; ++i) {
            storage_[static_cast<std::uint64_t>(writeHead + i) & mask_] = src[i];
        }
        return n;
    }

    // Read one frame at an absolute position; the position must still be
    // inside the bounded window [writeHead-capacity, writeHead).
    float read(std::int64_t position) const {
        return storage_[static_cast<std::uint64_t>(position) & mask_];
    }

    bool contains(std::int64_t position, std::int64_t writeHead,
                  std::int64_t outputHead) const {
        return position >= outputHead && position < writeHead;
    }

private:
    static std::size_t roundPow2(std::size_t value) {
        std::size_t p = 1;
        while (p < value) p <<= 1;
        return p;
    }

    std::size_t mask_;
    std::vector<float> storage_;
};

}  // namespace dbs
