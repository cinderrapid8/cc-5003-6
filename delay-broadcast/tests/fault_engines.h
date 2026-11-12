#pragma once

#include "../src/audio_types.h"
#include "../src/delay_engine.h"
#include "../src/pads.h"

#include <cstdint>
#include <vector>

namespace dbs {
namespace tst {

// Fault-injected engines. Each wraps an unmodified EngineCore and corrupts
// exactly one property of the delayed broadcast, while keeping control-plane
// semantics (submit/undo/push/heads) intact. A valid acceptance suite must
// reject every one of these via receiver-level and ring-level assertions.

// Fault 1: on an underrun the gap is filled by repeating the last aired frame
// instead of silence. Directly violates "stale material is never replayed".
class StaleReplayEngine {
public:
    StaleReplayEngine(EngineConfig cfg, PadLibrary pads)
        : core_(std::move(cfg), std::move(pads)), last_(0.0f) {}

    std::int64_t pushInput(const float* p, std::int64_t n) {
        return core_.pushInput(p, n);
    }
    void signalEndOfInput() { core_.signalEndOfInput(); }
    SubmitResult submitEdit(std::int64_t b, std::int64_t e,
                            const std::string& pad) {
        return core_.submitEdit(b, e, pad);
    }
    SubmitResult undoEdit(std::uint64_t id) { return core_.undoEdit(id); }
    bool resume() { return core_.resume(); }
    std::int64_t writePos() const { return core_.writePos(); }
    std::int64_t outputPos() const { return core_.outputPos(); }

    std::int64_t pullOutput(float* dst, std::int64_t count) {
        const std::int64_t n = core_.pullOutput(dst, count);
        for (std::int64_t i = 0; i < n; ++i) {
            if (dst[i] == 0.0f) dst[i] = last_;  // silence gap -> stale repeat
            else last_ = dst[i];
        }
        return n;
    }

private:
    EngineCore core_;
    float last_;
};

// Fault 2: every output frame is shifted by one absolute position, breaking
// sample alignment between declared replacement ranges and recorded audio.
class ShiftedFrameEngine {
public:
    ShiftedFrameEngine(EngineConfig cfg, PadLibrary pads)
        : core_(std::move(cfg), std::move(pads)) {}

    std::int64_t pushInput(const float* p, std::int64_t n) {
        return core_.pushInput(p, n);
    }
    void signalEndOfInput() { core_.signalEndOfInput(); }
    SubmitResult submitEdit(std::int64_t b, std::int64_t e,
                            const std::string& pad) {
        return core_.submitEdit(b, e, pad);
    }
    SubmitResult undoEdit(std::uint64_t id) { return core_.undoEdit(id); }
    bool resume() { return core_.resume(); }
    std::int64_t writePos() const { return core_.writePos(); }
    std::int64_t outputPos() const { return core_.outputPos(); }

    std::int64_t pullOutput(float* dst, std::int64_t count) {
        stage_.resize(static_cast<std::size_t>(count));
        const std::int64_t n = core_.pullOutput(stage_.data(), count);
        if (n >= 2) {
            // Frame recorded at absolute slot k carries the good value for k+1.
            for (std::int64_t i = 0; i + 1 < n; ++i) {
                dst[i] = stage_[static_cast<std::size_t>(i + 1)];
            }
            dst[n - 1] = stage_[static_cast<std::size_t>(n - 1)];
        } else if (n == 1) {
            dst[0] = stage_[0];
        }
        return n;
    }

private:
    EngineCore core_;
    std::vector<float> stage_;
};

// Fault 3: the seam fade bleeds one frame past the edit's end boundary, so a
// sample declared outside the replacement range is no longer the original.
class BoundaryBleedEngine {
public:
    BoundaryBleedEngine(EngineConfig cfg, PadLibrary pads)
        : core_(std::move(cfg), std::move(pads)) {}

    std::int64_t pushInput(const float* p, std::int64_t n) {
        return core_.pushInput(p, n);
    }
    void signalEndOfInput() { core_.signalEndOfInput(); }
    SubmitResult submitEdit(std::int64_t b, std::int64_t e,
                            const std::string& pad) {
        return core_.submitEdit(b, e, pad);
    }
    SubmitResult undoEdit(std::uint64_t id) { return core_.undoEdit(id); }
    bool resume() { return core_.resume(); }
    std::int64_t writePos() const { return core_.writePos(); }
    std::int64_t outputPos() const { return core_.outputPos(); }

    std::int64_t pullOutput(float* dst, std::int64_t count) {
        const std::int64_t start = core_.outputPos();
        // Capture edit end boundaries before the output head moves past them.
        std::vector<std::int64_t> boundaries;
        for (const EditRecord& e : core_.snapshot().edits)
            boundaries.push_back(e.end);

        const std::int64_t n = core_.pullOutput(dst, count);
        for (std::int64_t boundary : boundaries) {
            const std::int64_t local = boundary - start;
            // Corrupt the first frame strictly outside the declared range if
            // it lands in this pulled block.
            if (local >= 0 && local < n) {
                dst[static_cast<std::size_t>(local)] =
                    0.5f * dst[static_cast<std::size_t>(local)] + 0.5f;
            }
        }
        return n;
    }

private:
    EngineCore core_;
};

// Fault 4: one frame inside each pulled block is dropped (replaced by the
// following frame), desynchronising the receiver from its declared ranges.
class DropFrameEngine {
public:
    DropFrameEngine(EngineConfig cfg, PadLibrary pads)
        : core_(std::move(cfg), std::move(pads)) {}

    std::int64_t pushInput(const float* p, std::int64_t n) {
        return core_.pushInput(p, n);
    }
    void signalEndOfInput() { core_.signalEndOfInput(); }
    SubmitResult submitEdit(std::int64_t b, std::int64_t e,
                            const std::string& pad) {
        return core_.submitEdit(b, e, pad);
    }
    SubmitResult undoEdit(std::uint64_t id) { return core_.undoEdit(id); }
    bool resume() { return core_.resume(); }
    std::int64_t writePos() const { return core_.writePos(); }
    std::int64_t outputPos() const { return core_.outputPos(); }

    std::int64_t pullOutput(float* dst, std::int64_t count) {
        const std::int64_t n = core_.pullOutput(dst, count);
        if (n >= 2) {
            const std::int64_t victim = n / 2;
            dst[static_cast<std::size_t>(victim)] =
                dst[static_cast<std::size_t>(victim + 1)];
        }
        return n;
    }

private:
    EngineCore core_;
};

}  // namespace tst
}  // namespace dbs
