#include "script_runner.h"

#include "logger.h"

#include <algorithm>
#include <sstream>

namespace dbs {

ScriptRunner::ScriptRunner(EngineConfig config, PadLibrary pads, CodeTrackConfig track)
    : core_(std::move(config), std::move(pads)), trackCfg_(track) {}

void ScriptRunner::feed(std::int64_t frames, std::int64_t chunkFrames) {
    std::int64_t sent = 0;
    std::vector<float> chunk(static_cast<std::size_t>(chunkFrames));
    while (sent < frames) {
        const std::int64_t n = std::min(chunkFrames, frames - sent);
        for (std::int64_t i = 0; i < n; ++i) {
            chunk[static_cast<std::size_t>(i)] =
                expectedSampleAt(core_.writePos() + i, trackCfg_);
        }
        const std::int64_t accepted = core_.pushInput(chunk.data(), n);
        if (accepted < n) {
            // The bounded ring is full. The deterministic runner never
            // drops input (which would misalign absolute positions); callers
            // must drain output between feeds to create room.
            std::ostringstream m;
            m << "script feed: ring full with " << (n - accepted)
              << " frames unwritten at writePos=" << core_.writePos();
            DBS_LOG_ERROR("script", m.str());
            throw std::runtime_error("bounded ring full: drain before feeding more");
        }
        sent += accepted;
    }
}

SubmitResult ScriptRunner::edit(std::int64_t begin, std::int64_t end,
                                const std::string& pad) {
    auto r = core_.submitEdit(begin, end, pad);
    results_.push_back(r);
    return r;
}

SubmitResult ScriptRunner::undo(std::uint64_t id) {
    auto r = core_.undoEdit(id);
    results_.push_back(r);
    return r;
}

std::int64_t ScriptRunner::drain(std::int64_t frames) {
    pullScratch_.resize(static_cast<std::size_t>(
        std::max(frames, static_cast<std::int64_t>(1))));
    const std::int64_t n = core_.pullOutput(pullScratch_.data(), frames);
    output_.insert(output_.end(), pullScratch_.begin(),
                   pullScratch_.begin() + n);
    return n;
}

void ScriptRunner::endInput() {
    core_.signalEndOfInput();
}

std::int64_t ScriptRunner::drainToCompletion(std::int64_t maxFrames) {
    std::int64_t total = 0;
    pullScratch_.resize(257);  // deliberately prime-sized, unaligned to edits
    while (total < maxFrames) {
        const std::int64_t want = std::min<std::int64_t>(257, maxFrames - total);
        const std::int64_t n = core_.pullOutput(pullScratch_.data(), want);
        if (n > 0) {
            output_.insert(output_.end(), pullScratch_.begin(),
                           pullScratch_.begin() + n);
            total += n;
        }
        if (n < want) break;  // stop-output policy or natural halt
    }
    return total;
}

ScriptReport ScriptRunner::report() const {
    ScriptReport r;
    r.totalOutput = static_cast<std::int64_t>(output_.size());
    r.silenceFilled = core_.snapshot().silenceFilled;
    r.underrunEvents = core_.snapshot().underrunEvents;
    r.backPressure = core_.snapshot().backPressureFrames;
    r.results = results_;
    r.output = output_;
    return r;
}

}  // namespace dbs
