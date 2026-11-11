#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace dbs {

// A "pad" (垫片) is replacement material that is looped/trimmed to exactly
// fill a requested edit range. Pads are local demo assets only.
struct Pad {
    std::string name;
    std::vector<float> samples;
    int sampleRate = 0;

    float at(std::int64_t index) const {
        if (samples.empty()) return 0.0f;
        return samples[static_cast<std::size_t>(index % static_cast<std::int64_t>(samples.size()))];
    }
};

class PadLibrary {
public:
    void add(Pad pad);
    const Pad* find(const std::string& name) const;
    std::vector<std::string> names() const;

    // Deterministic synthetic pads so the simulator is self-contained.
    static PadLibrary makeBuiltin(int sampleRate);

private:
    std::unordered_map<std::string, Pad> pads_;
};

// Continuous "spoken password"-style numbered announcement track.
// Each password block carries a unique, position-locked numeric code encoded
// both as an audible tone sequence and as an analytically checkable waveform,
// so tests can prove samples outside replaced ranges were never touched.
struct CodeTrackConfig {
    int sampleRate = 48000;
    std::int64_t blockFrames = 4800;   // 100 ms per numbered block
    int codeEveryBlocks = 1;
    float amplitude = 0.6f;
};

std::vector<float> generateCodeTrack(std::int64_t totalFrames, const CodeTrackConfig& cfg);

// Recover the code expected at an absolute frame from the deterministic
// generator; used by the test oracle.
std::uint32_t expectedCodeAt(std::int64_t frame, const CodeTrackConfig& cfg);

// Reconstruct the exact float sample the generator produced at frame.
float expectedSampleAt(std::int64_t frame, const CodeTrackConfig& cfg);

}  // namespace dbs
