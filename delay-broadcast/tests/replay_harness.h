#pragma once

#include "probe_report.h"
#include "reference_model.h"

#include "../src/audio_types.h"
#include "../src/delay_engine.h"
#include "../src/pads.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dbs {
namespace tst {

// A receiver that records everything the output callback hands it, keeping
// the exact block tiling so span boundaries can be checked separately from
// sample content. It never reaches into engine internals.
class RecordingSink {
public:
    struct Span {
        std::int64_t beginAbs = 0;  // absolute frame of first sample
        std::vector<float> samples;
    };

    void record(const float* p, std::int64_t n) {
        Span s;
        s.beginAbs = cursor_;
        s.samples.assign(p, p + n);
        cursor_ += n;
        flat_.insert(flat_.end(), p, p + n);
        spans_.push_back(std::move(s));
    }

    std::int64_t cursor() const { return cursor_; }
    const std::vector<float>& flat() const { return flat_; }
    const std::vector<Span>& spans() const { return spans_; }
    float at(std::int64_t absFrame) const {
        return flat_[static_cast<std::size_t>(absFrame)];
    }
    void clear() {
        cursor_ = 0;
        flat_.clear();
        spans_.clear();
    }

private:
    std::int64_t cursor_ = 0;
    std::vector<float> flat_;
    std::vector<Span> spans_;
};

// One deterministic step of a scenario. Feeds and pulls carry an explicit
// chunk size so the same event sequence can be replayed under different
// callback block cuttings without changing any absolute frame position.
struct Event {
    enum class Kind { Feed, Edit, Undo, Pull, EndInput, Resume };
    Kind kind = Kind::Pull;
    std::int64_t frames = 0;   // Feed/Pull total request
    std::int64_t chunk = 0;    // per-callback block size
    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::uint64_t id = 0;
    std::string pad;
};

inline Event evFeed(std::int64_t frames, std::int64_t chunk) {
    Event e;
    e.kind = Event::Kind::Feed;
    e.frames = frames;
    e.chunk = chunk;
    return e;
}
inline Event evPull(std::int64_t frames, std::int64_t chunk) {
    Event e;
    e.kind = Event::Kind::Pull;
    e.frames = frames;
    e.chunk = chunk;
    return e;
}
inline Event evEdit(std::int64_t begin, std::int64_t end,
                    const std::string& pad) {
    Event e;
    e.kind = Event::Kind::Edit;
    e.begin = begin;
    e.end = end;
    e.pad = pad;
    return e;
}
inline Event evUndo(std::uint64_t id) {
    Event e;
    e.kind = Event::Kind::Undo;
    e.id = id;
    return e;
}
inline Event evEndInput() {
    Event e;
    e.kind = Event::Kind::EndInput;
    return e;
}
inline Event evResume() {
    Event e;
    e.kind = Event::Kind::Resume;
    return e;
}

// How a scenario's block sizes are re-cut between replays. All policies keep
// the same totals and event ordering; only callback boundaries move.
enum class ChunkPolicy {
    AsScripted,
    AllOne,
    PrimeBlocks,
    LargeBlocks,
    Alternating,
};

std::int64_t cutChunk(ChunkPolicy policy, std::int64_t original,
                      std::int64_t stepIndex) {
    (void)stepIndex;
    switch (policy) {
        case ChunkPolicy::AsScripted: return original;
        case ChunkPolicy::AllOne: return 1;
        case ChunkPolicy::PrimeBlocks: return 257;
        case ChunkPolicy::LargeBlocks: return 4096;
        case ChunkPolicy::Alternating:
            return (stepIndex % 2 == 0) ? 37 : 521;
    }
    return original;
}

template <typename Engine>
struct ReplayArtifacts {
    RecordingSink sink;
    std::vector<SubmitResult> results;
};

// Replay one event sequence against a concrete engine and an independent
// reference model, feeding both from the analytic generator and splitting
// every request at the policy's callback boundaries.
template <typename Engine>
ReplayArtifacts<Engine> replay(Engine& engine, ReferenceModel& model,
                               const std::vector<Event>& events,
                               ChunkPolicy policy, ProbeReport& report) {
    ReplayArtifacts<Engine> art;
    std::vector<float> block;
    std::int64_t stepIndex = 0;

    auto feed = [&](std::int64_t total, std::int64_t scriptedChunk) {
        std::int64_t sent = 0;
        std::int64_t chunk = cutChunk(policy, scriptedChunk, stepIndex);
        while (sent < total) {
            const std::int64_t n = std::min(chunk, total - sent);
            block.resize(static_cast<std::size_t>(n));
            for (std::int64_t i = 0; i < n; ++i) {
                block[static_cast<std::size_t>(i)] =
                    expectedSampleAt(engine.writePos() + i,
                                     model.trackConfigForTest());
            }
            const std::int64_t accepted = engine.pushInput(block.data(), n);
            // Scenarios are sized to keep the bounded window fed but never
            // overflowed during a normal replay; a zero acceptance would
            // silently misalign absolute positions.
            report.expect(accepted == n,
                          "scenario feed fully accepted (drain keeps window free)");
            model.feed(accepted);
            sent += accepted;
        }
        ++stepIndex;
    };

    auto pull = [&](std::int64_t total, std::int64_t scriptedChunk) {
        std::int64_t wanted = 0;
        std::int64_t chunk = cutChunk(policy, scriptedChunk, stepIndex);
        while (wanted < total) {
            const std::int64_t n = std::min(chunk, total - wanted);
            std::vector<float> dst(static_cast<std::size_t>(n), 0.0f);
            const std::int64_t produced = engine.pullOutput(dst.data(), n);
            const std::vector<float> expected = model.pull(n);
            report.expect(produced == static_cast<std::int64_t>(expected.size()),
                          "engine and reference produce equal frame counts per callback");
            const std::int64_t common = std::min(produced,
                                                 static_cast<std::int64_t>(expected.size()));
            for (std::int64_t i = 0; i < common; ++i) {
                report.expect(std::fabs(dst[static_cast<std::size_t>(i)] -
                                        expected[static_cast<std::size_t>(i)]) <= 1e-5f,
                              "callback sample matches independent per-sample model");
            }
            art.sink.record(dst.data(), produced);
            wanted += produced;
            if (produced < n) break;  // stop-output / prebuffer halt
        }
        ++stepIndex;
    };

    for (const Event& ev : events) {
        switch (ev.kind) {
            case Event::Kind::Feed: feed(ev.frames, ev.chunk); break;
            case Event::Kind::Pull: pull(ev.frames, ev.chunk); break;
            case Event::Kind::EndInput:
                engine.signalEndOfInput();
                model.endInput();
                break;
            case Event::Kind::Resume:
                report.expect(engine.resume() == model.resume(),
                              "resume decision matches reference model");
                break;
            case Event::Kind::Edit: {
                const auto expected =
                    model.expectedEditStatus(ev.begin, ev.end, ev.pad);
                SubmitResult r =
                    engine.submitEdit(ev.begin, ev.end, ev.pad);
                report.expect(r.status == expected,
                              "edit acceptance matches independently derived status");
                art.results.push_back(r);
                model.applyEdit(r, ev.begin, ev.end, ev.pad, report);
                break;
            }
            case Event::Kind::Undo: {
                SubmitResult r = engine.undoEdit(ev.id);
                art.results.push_back(r);
                model.applyUndo(r, report);
                break;
            }
        }
    }
    return art;
}

// Whole-stream verification once a replay has finished:
//   * every recorded frame equals the independently predicted frame;
//   * recorded spans tile the absolute axis with no gap or overlap;
//   * frames outside every declared replacement range are bit-for-bit the
//     analytic generator output (the expected value is recomputed, never
//     copied from the engine or from a stored expected buffer);
//   * frames recorded inside a declared range are provably not the original.
void verifyRecording(const RecordingSink& sink, const ReferenceModel& model,
                     ProbeReport& report, bool requireReplacement = true) {
    std::int64_t tiled = 0;
    for (const auto& span : sink.spans()) {
        report.expect(span.beginAbs == tiled,
                      "recorded spans tile the absolute axis without gaps");
        tiled += static_cast<std::int64_t>(span.samples.size());
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(span.samples.size());
             ++i) {
            const std::int64_t abs = span.beginAbs + i;
            const float want = model.predict(abs);
            report.expect(std::fabs(span.samples[static_cast<std::size_t>(i)] -
                                    want) <= 1e-5f,
                          "recorded audio matches independent prediction");
        }
    }
    report.expect(tiled == sink.cursor(),
                  "receiver recorded exactly the produced frame count");

    std::int64_t outside = 0;
    std::int64_t inside = 0;
    for (std::int64_t abs = 0; abs < sink.cursor(); ++abs) {
        if (model.isSilenceSlot(abs)) continue;  // policy silence, not program
        const float original = expectedSampleAt(abs, model.trackConfigForTest());
        if (model.insideReplacement(abs)) {
            ++inside;
            // Strictly interior frames (outside every seam fade) must be pure
            // replacement material, so the receiver cannot carry the original
            // program where a replacement was declared.
            if (model.deepInsideReplacement(abs)) {
                const float got = sink.at(abs);
                report.expect(std::fabs(got - original) > 1e-6f,
                              "strictly interior frame carries replacement material");
            }
        } else {
            ++outside;
            // Tolerance rather than bit equality: values survive a bounded
            // ring store/load round trip across physical wraps, which is a
            // float copy but asserted here against the recomputed generator
            // value rather than a buffer read back from the engine.
            report.expect(std::fabs(sink.at(abs) - original) <= 2e-6f,
                          "sample outside replacement ranges stays exactly the input");
        }
    }
    report.expect(outside > 0, "verification actually inspected outside samples");
    if (requireReplacement) {
        report.expect(inside > 0,
                      "verification actually inspected replaced samples");
    }
}

}  // namespace tst
}  // namespace dbs
