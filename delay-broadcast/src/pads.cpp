#include "pads.h"

#include <cmath>
#include <numbers>

namespace dbs {

namespace {
constexpr float kPi = static_cast<float>(std::numbers::pi_v<double>);

// Deterministic, frame-locked hash so every absolute frame has a unique,
// analytically recomputable value (no random state to drift).
float hashFloat(std::uint64_t x) {
    std::uint64_t z = x + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z ^= z >> 31;
    return static_cast<float>(static_cast<std::int64_t>(z & 0xFFFFFFFFULL) -
                              static_cast<std::int64_t>(0x80000000ULL)) /
           static_cast<float>(0x80000000ULL);
}
}  // namespace

std::uint32_t expectedCodeAt(std::int64_t frame, const CodeTrackConfig& cfg) {
    const std::int64_t block = frame / cfg.blockFrames;
    return static_cast<std::uint32_t>(block / cfg.codeEveryBlocks + 1);
}

float expectedSampleAt(std::int64_t frame, const CodeTrackConfig& cfg) {
    const std::int64_t inside = frame % cfg.blockFrames;
    const std::uint32_t code = expectedCodeAt(frame, cfg);

    // Audible carrier plus a code-dependent frequency gives each numbered
    // block a distinct, hearable "announcement".
    const float baseFreq = 440.0f;
    const float codeFreq = 300.0f + 8.0f * static_cast<float>(code % 80);
    const double phase1 = 2.0 * std::numbers::pi * baseFreq *
                          static_cast<double>(inside) / cfg.sampleRate;
    const double phase2 = 2.0 * std::numbers::pi * codeFreq *
                          static_cast<double>(inside) / cfg.sampleRate;
    float v = 0.7f * static_cast<float>(std::sin(phase1)) +
              0.3f * static_cast<float>(std::sin(phase2));

    // Frame-locked texture guarantees a unique expected value per position;
    // tests compare against this exact function rather than a stored copy.
    v = 0.85f * v + 0.15f * hashFloat(static_cast<std::uint64_t>(frame) * 7919ULL);

    // Short block-edge shaping avoids clicks while remaining deterministic.
    const std::int64_t edge = std::min(inside, cfg.blockFrames - 1 - inside);
    if (edge < 32) {
        const float gain = (edge + 1) / 33.0f;
        v *= gain;
    }
    return cfg.amplitude * v;
}

std::vector<float> generateCodeTrack(std::int64_t totalFrames, const CodeTrackConfig& cfg) {
    std::vector<float> out(static_cast<std::size_t>(totalFrames));
    for (std::int64_t i = 0; i < totalFrames; ++i) {
        out[static_cast<std::size_t>(i)] = expectedSampleAt(i, cfg);
    }
    return out;
}

namespace {

Pad makeTonePad(const std::string& name, int sampleRate, float freq,
                 std::int64_t frames, float amplitude = 0.5f) {
    Pad pad;
    pad.name = name;
    pad.sampleRate = sampleRate;
    pad.samples.resize(static_cast<std::size_t>(frames));
    for (std::int64_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * std::numbers::pi * freq * i / sampleRate;
        pad.samples[static_cast<std::size_t>(i)] =
            amplitude * static_cast<float>(std::sin(phase));
    }
    return pad;
}

}  // namespace

void PadLibrary::add(Pad pad) {
    pads_.insert_or_assign(pad.name, std::move(pad));
}

const Pad* PadLibrary::find(const std::string& name) const {
    auto it = pads_.find(name);
    return it == pads_.end() ? nullptr : &it->second;
}

std::vector<std::string> PadLibrary::names() const {
    std::vector<std::string> out;
    out.reserve(pads_.size());
    for (const auto& [name, pad] : pads_) out.push_back(name);
    return out;
}

PadLibrary PadLibrary::makeBuiltin(int sampleRate) {
    PadLibrary lib;
    lib.add(makeTonePad("tone-440", sampleRate, 440.0f, sampleRate));
    lib.add(makeTonePad("tone-1k", sampleRate, 1000.0f, sampleRate, 0.4f));
    lib.add(makeTonePad("tone-320", sampleRate, 320.0f, sampleRate, 0.45f));

    // A gentle two-tone chime pad.
    Pad chime;
    chime.name = "chime";
    chime.sampleRate = sampleRate;
    chime.samples.resize(static_cast<std::size_t>(sampleRate / 2));
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(chime.samples.size()); ++i) {
        const double p1 = 2.0 * std::numbers::pi * 660.0 * i / sampleRate;
        const double p2 = 2.0 * std::numbers::pi * 990.0 * i / sampleRate;
        const float env = 0.5f * (1.0f + static_cast<float>(std::sin(
            2.0 * std::numbers::pi * 2.0 * i / sampleRate - kPi / 2.0f)));
        chime.samples[static_cast<std::size_t>(i)] =
            0.4f * env * (static_cast<float>(std::sin(p1)) +
                          0.6f * static_cast<float>(std::sin(p2)));
    }
    lib.add(std::move(chime));
    return lib;
}

}  // namespace dbs
