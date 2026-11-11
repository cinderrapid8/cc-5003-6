#include "delay_engine.h"

#include "logger.h"

#include <algorithm>
#include <sstream>
#include <thread>

namespace dbs {

const char* toString(SubmitStatus status) {
    switch (status) {
        case SubmitStatus::Accepted: return "accepted";
        case SubmitStatus::RejectedBadRange: return "rejected:bad-range";
        case SubmitStatus::RejectedAlreadyAired: return "rejected:already-aired";
        case SubmitStatus::RejectedNotBuffered: return "rejected:range-not-buffered";
        case SubmitStatus::RejectedUnknownPad: return "rejected:unknown-pad";
    }
    return "rejected:unknown";
}

EngineCore::EngineCore(EngineConfig config, PadLibrary pads)
    : config_(config), pads_(std::move(pads)), ring_(config.capacityFrames) {
    if (config_.delayFrames < 0 ||
        config_.delayFrames >= static_cast<std::int64_t>(ring_.capacity())) {
        throw std::invalid_argument("delayFrames must be within [0, capacity)");
    }
    std::ostringstream msg;
    msg << "engine created: rate=" << config_.sampleRate
        << " capacity=" << ring_.capacity()
        << " delay=" << config_.delayFrames
        << " gap=" << underrunPolicyName(config_.underrun)
        << " eoi=" << eoiPolicyName(config_.endOfInput)
        << " fade=" << config_.fadeFrames;
    DBS_LOG_INFO("engine", msg.str());
}

std::int64_t EngineCore::pushInput(const float* frames, std::int64_t count) {
    if (count <= 0) return 0;
    std::int64_t accepted = ring_.write(frames, writePos_, outputPos_, count);
    if (accepted < count) {
        const std::int64_t refused = count - accepted;
        backPressureFrames_ += refused;
        std::ostringstream msg;
        msg << "ring pressure: " << refused
            << " samples back-pressured/evicted (old material never replayed)";
        DBS_LOG_WARN("engine", msg.str());
    }
    writePos_ += accepted;
    return accepted;
}

void EngineCore::signalEndOfInput() {
    if (!inputEnded_) {
        inputEnded_ = true;
        DBS_LOG_INFO("io", "input end-of-stream signaled");
    }
}

SubmitResult EngineCore::submitEdit(std::int64_t begin, std::int64_t end,
                                    const std::string& padName) {
    SubmitResult result;
    result.id = nextId_;
    result.seq = nextSeq_;

    if (begin < 0 || end <= begin) {
        result.status = SubmitStatus::RejectedBadRange;
        result.detail = "begin must be >= 0 and end > begin";
        return result;
    }
    if (begin < outputPos_) {
        result.status = SubmitStatus::RejectedAlreadyAired;
        std::ostringstream d;
        d << "begin=" << begin << " already aired (outputPos=" << outputPos_ << ")";
        result.detail = d.str();
        return result;
    }
    if (end > writePos_) {
        result.status = SubmitStatus::RejectedNotBuffered;
        std::ostringstream d;
        d << "end=" << end << " not buffered yet (writePos=" << writePos_ << ")";
        result.detail = d.str();
        return result;
    }
    if (!pads_.find(padName)) {
        result.status = SubmitStatus::RejectedUnknownPad;
        result.detail = "no pad named '" + padName + "'";
        return result;
    }

    Edit edit;
    edit.id = nextId_++;
    edit.seq = nextSeq_++;
    edit.begin = begin;
    edit.end = end;
    edit.padName = padName;
    edit.active = true;

    // Overlap handling is fully deterministic: the newest event wins.
    // Earlier edits overlapping the new range are split (outside pieces kept)
    // or removed (fully covered). The final per-sample owner is always the
    // active edit with the highest sequence number.
    std::deque<Edit> rebuilt;
    rebuilt.push_back(edit);
    for (const Edit& old : edits_) {
        if (!old.active || old.end <= begin || old.begin >= end) {
            rebuilt.push_back(old);
            continue;
        }
        if (old.begin >= begin && old.end <= end) {
            std::ostringstream m;
            m << "edit id=" << old.id << " seq=" << old.seq
              << " fully covered by new edit id=" << edit.id
              << " seq=" << edit.seq << " -> removed from active ownership";
            DBS_LOG_INFO("control", m.str());
            continue;
        }
        if (old.begin < begin) {
            Edit left = old;
            left.end = begin;
            rebuilt.push_back(left);
            std::ostringstream m;
            m << "edit id=" << old.id << " split: left ["
              << left.begin << "," << left.end
              << ") survives, right part ceded to edit id=" << edit.id;
            DBS_LOG_INFO("control", m.str());
        }
        if (old.end > end) {
            Edit right = old;
            right.begin = end;
            rebuilt.push_back(right);
            std::ostringstream m;
            m << "edit id=" << old.id << " split: right ["
              << right.begin << "," << right.end
              << ") survives, left part ceded to edit id=" << edit.id;
            DBS_LOG_INFO("control", m.str());
        }
    }
    edits_.swap(rebuilt);

    result.status = SubmitStatus::Accepted;
    std::ostringstream m;
    m << "edit accepted id=" << edit.id << " seq=" << edit.seq
      << " range=[" << begin << "," << end << ") pad=" << padName
      << " length=" << (end - begin);
    DBS_LOG_INFO("control", m.str());
    return result;
}

SubmitResult EngineCore::undoEdit(std::uint64_t id) {
    SubmitResult result;
    result.id = id;
    result.seq = nextSeq_;

    for (auto it = edits_.begin(); it != edits_.end(); ++it) {
        if (it->id == id && it->active) {
            if (it->begin < outputPos_) {
                result.status = SubmitStatus::RejectedAlreadyAired;
                std::ostringstream d;
                d << "edit id=" << id << " already on air from " << it->begin
                  << " (outputPos=" << outputPos_ << ")";
                result.detail = d.str();
                return result;
            }
            result.status = SubmitStatus::Accepted;
            edits_.erase(it);
            ++nextSeq_;
            std::ostringstream m;
            m << "undo accepted id=" << id << " seq=" << result.seq
              << " original material restored (no sample count change)";
            DBS_LOG_INFO("control", m.str());
            return result;
        }
    }
    if (std::find(prunedEditIds_.begin(), prunedEditIds_.end(), id) !=
        prunedEditIds_.end()) {
        result.status = SubmitStatus::RejectedAlreadyAired;
        result.detail = "edit id " + std::to_string(id) +
                        " has already been fully aired and cannot be undone";
        return result;
    }
    result.status = SubmitStatus::RejectedBadRange;
    result.detail = "no active edit with id " + std::to_string(id);
    DBS_LOG_WARN("control", "undo rejected: " + result.detail);
    return result;
}

const EngineCore::Edit* EngineCore::coveringEdit(std::int64_t frame) const {
    const Edit* winner = nullptr;
    for (const Edit& e : edits_) {
        if (e.active && frame >= e.begin && frame < e.end) {
            if (!winner || e.seq > winner->seq) winner = &e;
        }
    }
    return winner;
}

void EngineCore::pruneEdits() {
    // Edits are stored in submission order, not positional order, so every
    // entry must be inspected rather than only the deque front.
    for (auto it = edits_.begin(); it != edits_.end();) {
        if (it->end <= outputPos_) {
            prunedEditIds_.push_back(it->id);
            it = edits_.erase(it);
        } else {
            ++it;
        }
    }
    // Ids are monotonic; drop only the oldest tombstones to bound memory.
    while (prunedEditIds_.size() > 4096) prunedEditIds_.pop_front();
}

float EngineCore::sourceSample(const Edit* edit, std::int64_t position) const {
    if (!edit) {
        return ring_.read(position);
    }
    // A neighboring edit only owns frames inside its own range; asking for a
    // seam sample just outside that boundary (a crossfade) falls back to the
    // original ring material rather than a negative pad index.
    if (position < edit->begin || position >= edit->end) {
        return ring_.read(position);
    }
    const Pad* pad = pads_.find(edit->padName);
    if (!pad) return ring_.read(position);
    return pad->at(position - edit->begin);
}

const EngineCore::Edit* EngineCore::editEndingAt(std::int64_t boundary) const {
    const Edit* found = nullptr;
    for (const Edit& e : edits_) {
        if (e.end == boundary && (!found || e.seq > found->seq)) found = &e;
    }
    return found;
}

const EngineCore::Edit* EngineCore::editStartingAt(std::int64_t boundary) const {
    const Edit* found = nullptr;
    for (const Edit& e : edits_) {
        if (e.begin == boundary && (!found || e.seq > found->seq)) found = &e;
    }
    return found;
}

std::int64_t EngineCore::fadeLengthFor(const Edit* region, std::int64_t begin,
                                       std::int64_t end) const {
    std::int64_t regionLen = std::int64_t(1) << 40;
    if (region) regionLen = region->end - region->begin;
    (void)begin;
    (void)end;
    // Half the owning region so entry and exit fades can never overlap even
    // on very short edits.
    return std::max<std::int64_t>(
        1, std::min({config_.fadeFrames, (regionLen + 1) / 2, std::int64_t(65536)}));
}

float EngineCore::renderFrame(std::int64_t position) {
    const Edit* here = coveringEdit(position);
    const float primary = sourceSample(here, position);
    if (config_.fadeFrames <= 0 || !here) return primary;

    // All blending stays strictly inside the edit range, so samples outside a
    // replaced region are provably never touched. The entry fade uses the
    // first f frames of the edit; the exit fade uses its last f frames. Both
    // are clamped to half the region length so they can never overlap, and
    // the frame count is unchanged regardless.
    const std::int64_t f = fadeLengthFor(here, here->begin, here->end);

    const std::int64_t fromStart = position - here->begin;
    if (fromStart >= 0 && fromStart < f) {
        const Edit* before = editEndingAt(here->begin);
        const float gain =
            static_cast<float>(fromStart + 1) / static_cast<float>(f);
        return sourceSample(before, position) * (1.0f - gain) + primary * gain;
    }

    const std::int64_t fromEnd = here->end - 1 - position;
    if (fromEnd >= 0 && fromEnd < f) {
        const Edit* after = editStartingAt(here->end);
        const float gain =
            static_cast<float>(fromEnd + 1) / static_cast<float>(f);
        return primary * gain + sourceSample(after, position) * (1.0f - gain);
    }
    return primary;
}

std::int64_t EngineCore::pullOutput(float* dst, std::int64_t count) {
    std::int64_t produced = 0;
    while (produced < count) {
        if (paused_) break;

        const std::int64_t position = outputPos_;
        // A frame exists in the bounded window only when the write head has
        // passed it; the fixed broadcast delay additionally requires D more
        // input frames before it may air. Both checks are strict so the
        // position==writePos boundary is treated as "not available yet".
        const bool frameBuffered = position < writePos_;
        const bool delayReady =
            writePos_ >= position + config_.delayFrames + 1;
        const bool prebuffering = writePos_ < config_.delayFrames + 1;
        // Once input has ended, no future frames can satisfy the delay gate,
        // so the remaining buffered frames are flushed in order instead of
        // being stranded behind a delay that can never complete.
        const bool available =
            frameBuffered && (inputEnded_ || delayReady);

        if (!available) {
            // Pre-buffering: the output head has not gone on air yet because
            // the fixed delay window is not full. Startup fill always waits
            // for enough real input regardless of the gap policy; inserting
            // silence here would silently consume the configured broadcast
            // delay. No head moves, so positions stay exactly sample-aligned.
            if (prebuffering && !inputEnded_) {
                break;
            }
            if (inputEnded_ &&
                config_.endOfInput == EndOfInputPolicy::StopOutput) {
                DBS_LOG_INFO("engine",
                             "end-of-input stop policy: output halting at " +
                             std::to_string(position));
                break;
            }
            if (!inputEnded_ &&
                config_.underrun == UnderrunPolicy::StopOutput) {
                paused_ = true;
                ++underrunEvents_;
                std::ostringstream m;
                m << "underrun at " << position
                  << ": stop-output policy engaged (no old program repeated)";
                DBS_LOG_WARN("engine", m.str());
                break;
            }
            // InsertSilence (gap or end-of-input): the zero frame occupies
            // its own absolute frame slot in the ring, so the write head
            // advances with the output head. Resumed real input then lands at
            // its correct absolute position, and stale frames are never
            // replayed to fill a gap.
            const float zero = 0.0f;
            const std::int64_t stored =
                ring_.write(&zero, writePos_, outputPos_, 1);
            if (stored == 1) ++writePos_;
            dst[produced++] = 0.0f;
            ++outputPos_;
            ++silenceFilled_;
            if (!inputEnded_) ++underrunEvents_;
            pruneEdits();
            continue;
        }

        dst[produced++] = renderFrame(position);
        ++outputPos_;
        pruneEdits();
    }
    return produced;
}

bool EngineCore::resume() {
    if (!paused_) return false;
    if (writePos_ <= outputPos_) return false;
    paused_ = false;
    DBS_LOG_INFO("engine",
                 "input recovered at " + std::to_string(writePos_) +
                 ": resuming output from " + std::to_string(outputPos_));
    return true;
}

bool EngineCore::finished() const {
    if (!inputEnded_) return false;
    if (config_.endOfInput == EndOfInputPolicy::StopOutput) {
        return outputPos_ >= writePos_;
    }
    // Silence policy runs until the consumer is stopped externally; the
    // threaded renderer stops when stop() is called after EOS drain.
    return false;
}

EngineSnapshot EngineCore::snapshot() const {
    EngineSnapshot s;
    s.writePos = writePos_;
    s.outputPos = outputPos_;
    s.controlledBegin = outputPos_;
    s.controlledEnd = writePos_;
    s.bufferedFrames = writePos_ - outputPos_;
    s.capacityFrames = static_cast<std::int64_t>(ring_.capacity());
    s.delayFrames = config_.delayFrames;
    s.paused = paused_;
    s.inputEnded = inputEnded_;
    s.silenceFilled = silenceFilled_;
    s.underrunEvents = underrunEvents_;
    s.backPressureFrames = backPressureFrames_;
    s.edits.reserve(edits_.size());
    for (const Edit& e : edits_) {
        EditRecord r;
        r.id = e.id;
        r.seq = e.seq;
        r.begin = e.begin;
        r.end = e.end;
        r.padName = e.padName;
        r.active = e.active;
        s.edits.push_back(r);
    }
    return s;
}

// --- threaded wrapper -----------------------------------------------------

DelayEngine::DelayEngine(EngineConfig config, PadLibrary pads,
                         OutputCallback callback, std::int64_t blockFrames)
    : core_(std::move(config), std::move(pads)),
      callback_(std::move(callback)),
      blockFrames_(blockFrames),
      blockBuf_(static_cast<std::size_t>(blockFrames), 0.0f) {}

DelayEngine::~DelayEngine() {
    stop();
}

void DelayEngine::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&DelayEngine::renderLoop, this);
}

void DelayEngine::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        Command stop;
        stop.kind = Command::Kind::Stop;
        commands_.push_back(std::move(stop));
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::future<SubmitResult> DelayEngine::submitEdit(std::int64_t begin,
                                                  std::int64_t end,
                                                  const std::string& padName) {
    Command cmd;
    cmd.kind = Command::Kind::Edit;
    cmd.begin = begin;
    cmd.end = end;
    cmd.padName = padName;
    auto fut = cmd.promise.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(std::move(cmd));
    }
    cv_.notify_one();
    return fut;
}

std::future<SubmitResult> DelayEngine::undoEdit(std::uint64_t id) {
    Command cmd;
    cmd.kind = Command::Kind::Undo;
    cmd.id = id;
    auto fut = cmd.promise.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(std::move(cmd));
    }
    cv_.notify_one();
    return fut;
}

std::int64_t DelayEngine::pushInput(const float* frames, std::int64_t count) {
    std::int64_t accepted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepted = core_.pushInput(frames, count);
    }
    if (accepted > 0) cv_.notify_one();
    return accepted;
}

void DelayEngine::signalEndOfInput() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        core_.signalEndOfInput();
    }
    cv_.notify_one();
}

EngineSnapshot DelayEngine::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.snapshot();
}

void DelayEngine::renderLoop() {
    while (true) {
        bool stopRequested = false;
        float* out = nullptr;
        std::int64_t produced = 0;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Short bounded wait: input data must wake the loop on its own
            // (pushInput notifies), while the timeout also lets a
            // stop-output pause poll for recovery without a command.
            cv_.wait_for(lock, std::chrono::milliseconds(2),
                         [this] { return !commands_.empty() || !running_; });

            std::deque<Command> local;
            local.swap(commands_);
            for (Command& cmd : local) {
                SubmitResult result;
                switch (cmd.kind) {
                    case Command::Kind::Edit:
                        result = core_.submitEdit(cmd.begin, cmd.end,
                                                  cmd.padName);
                        cmd.promise.set_value(result);
                        break;
                    case Command::Kind::Undo:
                        result = core_.undoEdit(cmd.id);
                        cmd.promise.set_value(result);
                        break;
                    case Command::Kind::Stop:
                        stopRequested = true;
                        break;
                }
            }
            if (stopRequested) break;

            // Every EngineCore mutation and render happens under the same
            // lock, so deque pointers used inside renderFrame can never be
            // invalidated by a concurrent submitEdit/pushInput.
            core_.resume();
            produced = core_.pullOutput(blockBuf_.data(), blockFrames_);
            out = blockBuf_.data();
        }

        if (produced > 0) {
            // The user callback runs outside the engine lock so a slow sink
            // cannot block control commands or input.
            callback_(out, produced);
        } else {
            // No data and not stopped: wait for either new input or a command
            // instead of spinning. Old program is never used to fill the gap.
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(2),
                         [this] { return !commands_.empty() || !running_; });
        }
    }
}

}  // namespace dbs
