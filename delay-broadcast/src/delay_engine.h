#pragma once

#include "audio_types.h"
#include "ring_buffer.h"
#include "pads.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <string>
#include <vector>

namespace dbs {

enum class SubmitStatus {
    Accepted,
    RejectedBadRange,
    RejectedAlreadyAired,
    RejectedNotBuffered,
    RejectedUnknownPad,
};

const char* toString(SubmitStatus status);

struct EditRecord {
    std::uint64_t id = 0;
    std::uint64_t seq = 0;
    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::string padName;
    bool active = true;
};

struct EngineSnapshot {
    std::int64_t writePos = 0;
    std::int64_t outputPos = 0;
    std::int64_t controlledBegin = 0;
    std::int64_t controlledEnd = 0;
    std::int64_t bufferedFrames = 0;
    std::int64_t capacityFrames = 0;
    std::int64_t delayFrames = 0;
    bool paused = false;
    bool inputEnded = false;
    std::int64_t silenceFilled = 0;
    std::int64_t underrunEvents = 0;
    std::int64_t backPressureFrames = 0;
    std::vector<EditRecord> edits;
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Accepted;
    std::uint64_t id = 0;
    std::uint64_t seq = 0;
    std::string detail;
};

// Pure, single-threaded engine. All timing is driven by sample counts;
// wall-clock code never influences output content. The threaded DelayEngine
// below is a thin wrapper used by the interactive simulator and GUI.
class EngineCore {
public:
    EngineCore(EngineConfig config, PadLibrary pads);

    const EngineConfig& config() const { return config_; }
    const PadLibrary& pads() const { return pads_; }

    // Producer side -------------------------------------------------------

    // Feed frames at the current write head. Frames that would overflow the
    // bounded window are refused (counted as back-pressure); already-buffered
    // material is never overwritten and never replayed.
    std::int64_t pushInput(const float* frames, std::int64_t count);
    void signalEndOfInput();

    // Control events (deterministic sequence order) ----------------------
    SubmitResult submitEdit(std::int64_t begin, std::int64_t end,
                            const std::string& padName);
    SubmitResult undoEdit(std::uint64_t id);

    // Consumer side -------------------------------------------------------

    // Produce exactly count frames, splitting internally at every edit
    // boundary and underrun edge regardless of the caller's block size.
    // Returns the number of frames written to dst (less than count only when
    // StopOutput pauses or when input has fully ended under that policy).
    std::int64_t pullOutput(float* dst, std::int64_t count);

    // Snapshot for UI rendering (copies edit metadata under no lock here;
    // the threaded wrapper takes its own mutex).
    EngineSnapshot snapshot() const;

    std::int64_t writePos() const { return writePos_; }
    std::int64_t outputPos() const { return outputPos_; }
    bool paused() const { return paused_; }
    bool finished() const;

    // Clear a stop-output underrun pause once fresh input has arrived.
    // Never repositions the output head: positions stay monotonic.
    bool resume();

private:
    struct Edit {
        std::uint64_t id = 0;
        std::uint64_t seq = 0;
        std::int64_t begin = 0;
        std::int64_t end = 0;
        std::string padName;
        bool active = true;
    };

    const Edit* coveringEdit(std::int64_t frame) const;
    void pruneEdits();
    float renderFrame(std::int64_t position);
    float sourceSample(const Edit* edit, std::int64_t position) const;

    // Seam helpers: a boundary at B between region A and region B is faded
    // using the f frames starting at B (all owned by the "after" region).
    // Exactly one region owns each frame, so a frame is blended at most once.
    const Edit* editEndingAt(std::int64_t boundary) const;
    const Edit* editStartingAt(std::int64_t boundary) const;
    std::int64_t fadeLengthFor(const Edit* region, std::int64_t begin,
                               std::int64_t end) const;

    EngineConfig config_;
    PadLibrary pads_;
    RingBuffer ring_;

    std::int64_t writePos_ = 0;
    std::int64_t outputPos_ = 0;
    bool inputEnded_ = false;
    bool paused_ = false;
    std::uint64_t nextId_ = 1;
    std::uint64_t nextSeq_ = 1;
    std::int64_t silenceFilled_ = 0;
    std::int64_t underrunEvents_ = 0;
    std::int64_t backPressureFrames_ = 0;
    std::deque<Edit> edits_;
    // Ids of edits pruned after fully airing: an undo against one of them is
    // an already-aired rejection, not a generic unknown-id rejection.
    std::deque<std::uint64_t> prunedEditIds_;
};

// Threading wrapper: one render thread pulls blocks and hands them to an
// output callback; control events are queued and applied in a deterministic
// global sequence. The render thread never blocks on UI confirmation.
class DelayEngine {
public:
    using OutputCallback = std::function<void(const float*, std::int64_t)>;

    DelayEngine(EngineConfig config, PadLibrary pads, OutputCallback callback,
                std::int64_t blockFrames = 480);
    ~DelayEngine();

    void start();
    void stop();

    std::future<SubmitResult> submitEdit(std::int64_t begin, std::int64_t end,
                                         const std::string& padName);
    std::future<SubmitResult> undoEdit(std::uint64_t id);

    std::int64_t pushInput(const float* frames, std::int64_t count);
    void signalEndOfInput();

    EngineSnapshot snapshot();

    EngineCore& core() { return core_; }

private:
    struct Command {
        enum class Kind { Edit, Undo, Stop };
        Kind kind;
        std::int64_t begin = 0;
        std::int64_t end = 0;
        std::uint64_t id = 0;
        std::string padName;
        std::promise<SubmitResult> promise;
    };

    void renderLoop();

    EngineCore core_;
    OutputCallback callback_;
    std::int64_t blockFrames_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    std::atomic<bool> running_{false};
    std::vector<float> blockBuf_;
};

}  // namespace dbs
