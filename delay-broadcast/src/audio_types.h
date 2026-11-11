#pragma once

#include <cstdint>
#include <cstddef>

namespace dbs {

enum class GapPolicy { InsertSilence, StopOutput };
enum class UnderrunPolicy { InsertSilence, StopOutput };
enum class EndOfInputPolicy { InsertSilence, StopOutput };

struct EngineConfig {
    int sampleRate = 48000;
    std::size_t capacityFrames = 384000;
    std::int64_t delayFrames = 144000;
    std::int64_t fadeFrames = 240;
    std::int32_t underrunGapThreshold = 1;
    UnderrunPolicy underrun = UnderrunPolicy::InsertSilence;
    EndOfInputPolicy endOfInput = EndOfInputPolicy::InsertSilence;
};

inline const char* gapPolicyName(GapPolicy p) {
    return p == GapPolicy::InsertSilence ? "insert-silence" : "stop-output";
}
inline const char* underrunPolicyName(UnderrunPolicy p) {
    return p == UnderrunPolicy::InsertSilence ? "insert-silence" : "stop-output";
}
inline const char* eoiPolicyName(EndOfInputPolicy p) {
    return p == EndOfInputPolicy::InsertSilence ? "insert-silence" : "stop-output";
}

}  // namespace dbs
