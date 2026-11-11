#pragma once

#include "delay_engine.h"
#include "pads.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dbs {

enum class ScriptOpKind { Feed, Edit, Undo, Drain, Wait };

struct ScriptOp {
    ScriptOpKind kind;
    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::int64_t frames = 0;
    std::uint64_t editId = 0;
    std::string padName;
};

struct ScriptReport {
    std::int64_t totalInput = 0;
    std::int64_t totalOutput = 0;
    std::int64_t silenceFilled = 0;
    std::int64_t underrunEvents = 0;
    std::int64_t backPressure = 0;
    std::vector<SubmitResult> results;
    std::vector<float> output;
};

// Deterministic, single-threaded runner: every Feed pushes exactly the given
// number of frames from the code track in deliberately awkward chunk sizes,
// and every Drain pulls an exact frame count, so request/callback
// misalignment is exercised at sample granularity.
class ScriptRunner {
public:
    ScriptRunner(EngineConfig config, PadLibrary pads, CodeTrackConfig track);

    void feed(std::int64_t frames, std::int64_t chunkFrames);
    SubmitResult edit(std::int64_t begin, std::int64_t end, const std::string& pad);
    SubmitResult undo(std::uint64_t id);
    std::int64_t drain(std::int64_t frames);
    void endInput();

    // Pull until no more output can be produced (handles EOI policies).
    std::int64_t drainToCompletion(std::int64_t maxFrames);

    const EngineCore& core() const { return core_; }
    EngineCore& core() { return core_; }
    const std::vector<float>& output() const { return output_; }
    const CodeTrackConfig& trackConfig() const { return trackCfg_; }
    ScriptReport report() const;

private:
    EngineCore core_;
    CodeTrackConfig trackCfg_;
    std::vector<float> output_;
    std::vector<SubmitResult> results_;
    std::vector<float> pullScratch_;
};

}  // namespace dbs
