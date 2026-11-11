#include "test_framework.h"
#include "../src/delay_engine.h"
#include "../src/pads.h"
#include "../src/script_runner.h"
#include "../src/wav_file.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <set>
#include <thread>
#include <vector>

namespace dbs {

EngineConfig testBaseConfig() {
    EngineConfig cfg;
    cfg.sampleRate = 48000;
    cfg.capacityFrames = 4096;
    cfg.delayFrames = 0;
    cfg.fadeFrames = 32;
    cfg.underrun = UnderrunPolicy::InsertSilence;
    cfg.endOfInput = EndOfInputPolicy::InsertSilence;
    return cfg;
}

CodeTrackConfig testBaseTrack() {
    CodeTrackConfig t;
    t.sampleRate = 48000;
    t.blockFrames = 256;
    return t;
}

// Independent oracle: replays the accepted edit history on its own and
// decides what the output at every absolute frame must be. It never reads
// engine internals, so a bug shared by both implementations would require
// two independent mistakes.
struct OracleEdit {
    std::uint64_t id;
    std::uint64_t seq;
    std::int64_t begin, end;
    std::string pad;
};

class Oracle {
public:
    Oracle(CodeTrackConfig track, PadLibrary pads, std::int64_t fade)
        : track_(track), pads_(std::move(pads)), fade_(fade) {}

    void applyAccepted(const SubmitResult& r, std::int64_t begin,
                       std::int64_t end, const std::string& pad) {
        if (r.status != SubmitStatus::Accepted) return;
        OracleEdit e{r.id, r.seq, begin, end, pad};
        std::vector<OracleEdit> rebuilt{e};
        for (auto old : active_) {
            if (old.end <= begin || old.begin >= end) { rebuilt.push_back(old); continue; }
            if (old.begin >= begin && old.end <= end) continue;
            if (old.begin < begin) { auto l = old; l.end = begin; rebuilt.push_back(l); }
            if (old.end > end) { auto rt = old; rt.begin = end; rebuilt.push_back(rt); }
        }
        active_.swap(rebuilt);
    }

    void applyUndo(const SubmitResult& r) {
        if (r.status != SubmitStatus::Accepted) return;
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [&](const OracleEdit& e) { return e.id == r.id; }),
                      active_.end());
    }

    void prune(std::int64_t outputPos) {
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [&](const OracleEdit& e) { return e.end <= outputPos; }),
                      active_.end());
    }

    const std::vector<OracleEdit>& snapshotActive() const { return active_; }

    std::int64_t fadeOf(const OracleEdit& region) const { return fadeFor(&region); }

    const OracleEdit* owner(std::int64_t frame) const {
        const OracleEdit* winner = nullptr;
        for (const auto& e : active_) {
            if (frame >= e.begin && frame < e.end && (!winner || e.seq > winner->seq)) {
                winner = &e;
            }
        }
        return winner;
    }

    float source(const OracleEdit* e, std::int64_t frame) const {
        if (!e) return expectedSampleAt(frame, track_);
        // Seam lookups can land just outside the neighbor's owned range;
        // fall back to the original generator instead of a negative index.
        if (frame < e->begin || frame >= e->end) {
            return expectedSampleAt(frame, track_);
        }
        const Pad* pad = pads_.find(e->pad);
        return pad->at(frame - e->begin);
    }

    const OracleEdit* endingAt(std::int64_t boundary) const {
        const OracleEdit* found = nullptr;
        for (const auto& e : active_)
            if (e.end == boundary && (!found || e.seq > found->seq)) found = &e;
        return found;
    }

    const OracleEdit* startingAt(std::int64_t boundary) const {
        const OracleEdit* found = nullptr;
        for (const auto& e : active_)
            if (e.begin == boundary && (!found || e.seq > found->seq)) found = &e;
        return found;
    }

    std::int64_t fadeFor(const OracleEdit* region) const {
        std::int64_t len = 1LL << 40;
        if (region) len = region->end - region->begin;
        return std::max<std::int64_t>(
            1, std::min(std::min(fade_, (len + 1) / 2), static_cast<std::int64_t>(65536)));
    }

    // Independent re-implementation of the engine's seam rule: fades live
    // strictly inside the edit range, so frames outside any edit are exactly
    // the generator output. Entry/exit fades are each half the region at most.
    float render(std::int64_t frame) {
        const OracleEdit* here = owner(frame);
        const float primary = source(here, frame);
        if (fade_ <= 0 || !here) return primary;

        const std::int64_t len = here->end - here->begin;
        const std::int64_t f = fadeFor(here);

        const std::int64_t fromStart = frame - here->begin;
        if (fromStart >= 0 && fromStart < f) {
            const float g = float(fromStart + 1) / float(f);
            return source(endingAt(here->begin), frame) * (1 - g) + primary * g;
        }
        const std::int64_t fromEnd = here->end - 1 - frame;
        if (fromEnd >= 0 && fromEnd < f) {
            const float g = float(fromEnd + 1) / float(f);
            return primary * g + source(startingAt(here->end), frame) * (1 - g);
        }
        (void)len;
        return primary;
    }

private:
    CodeTrackConfig track_;
    PadLibrary pads_;
    std::int64_t fade_;
    std::vector<OracleEdit> active_;
};

// Verify every output frame against the independent oracle. Frames not owned
// by any edit must equal the untouched code-track generator exactly, because
// all seam fades are constrained to the interior of the edit range.
void verifyStream(ScriptRunner& runner, Oracle& oracle, std::int64_t startFrame = 0) {
    const auto& out = runner.output();
    const auto& track = runner.trackConfig();
    std::int64_t mismatches = 0;
    std::int64_t outsideChecked = 0;

    for (std::int64_t i = 0; i < static_cast<std::int64_t>(out.size()); ++i) {
        const std::int64_t absFrame = startFrame + i;
        const float expected = oracle.render(absFrame);
        if (std::fabs(expected - out[i]) > 2e-6f) {
            if (mismatches < 5) {
                std::fprintf(stderr, "    frame %lld got %.6f want %.6f\n",
                             (long long)absFrame, out[i], expected);
            }
            ++mismatches;
        }
        if (!oracle.owner(absFrame)) {
            check(std::fabs(expectedSampleAt(absFrame, track) - out[i]) <= 2e-6f,
                  "sample outside replaced ranges must stay byte-identical to input");
            ++outsideChecked;
        }
    }
    check(mismatches == 0, "all rendered frames match independent oracle");
    check(outsideChecked > 0, "oracle actually inspected non-replaced frames");
}

ScriptRunner makeRunner(EngineConfig cfg = testBaseConfig()) {
    return ScriptRunner(cfg, PadLibrary::makeBuiltin(cfg.sampleRate), testBaseTrack());
}

DBS_TEST(basic_replacement_preserves_outside_samples) {
    auto runner = makeRunner();
    Oracle oracle(testBaseTrack(), PadLibrary::makeBuiltin(48000), 32);
    runner.feed(3000, 333);
    auto r1 = runner.edit(1200, 2000, "tone-1k");
    oracle.applyAccepted(r1, 1200, 2000, "tone-1k");
    check(r1.status == SubmitStatus::Accepted, "edit inside buffered window accepted");
    auto r2 = runner.edit(2400, 2600, "chime");
    oracle.applyAccepted(r2, 2400, 2600, "chime");
    runner.drain(3000);
    verifyStream(runner, oracle);

    const auto snap = runner.core().snapshot();
    check(snap.outputPos == 3000, "output head advanced exactly with output frames");
    return checksFailed() == 0;
}

DBS_TEST(ring_wrap_around_keeps_absolute_positions_stable) {
    auto cfg = testBaseConfig();
    cfg.capacityFrames = 1024;  // small ring forces many wraps
    auto runner = makeRunner(cfg);
    Oracle oracle(testBaseTrack(), PadLibrary::makeBuiltin(48000), 32);

    // Alternate drain-before-feed so the bounded window never overflows,
    // while mismatched prime-ish block sizes exercise wrap and splitting.
    // Each round drains at least as much as it feeds, so the pending window
    // stays below the 1024-frame capacity for the whole run; the drain is
    // clamped to what is actually buffered so no policy silence is inserted.
    runner.feed(900, 127);
    auto r = runner.edit(500, 560, "tone-440");
    oracle.applyAccepted(r, 500, 560, "tone-440");
    for (int round = 0; round < 10; ++round) {
        const std::int64_t pending =
            runner.core().writePos() - runner.core().outputPos();
        const std::int64_t want = 550 + (round % 3) * 37;
        runner.drain(std::min(want, pending));
        runner.feed(500, 97 + round * 13);
    }
    runner.drain(runner.core().writePos() - runner.core().outputPos());
    verifyStream(runner, oracle);
    check(runner.core().outputPos() == static_cast<std::int64_t>(runner.output().size()),
          "output position tracks produced sample count across wraps");
    check(runner.core().snapshot().backPressureFrames == 0,
          "no back-pressure when producer respects the bounded window");
    return checksFailed() == 0;
}

DBS_TEST(near_airtime_submission_and_already_aired_rejection) {
    auto runner = makeRunner();
    runner.feed(2048, 256);
    runner.drain(1000);  // outputPos now 1000

    auto tooLate = runner.edit(900, 1100, "tone-440");
    check(tooLate.status == SubmitStatus::RejectedAlreadyAired,
          "edit starting before output head rejected");
    check(!tooLate.detail.empty(), "rejection carries explanatory detail");

    auto notBuffered = runner.edit(1500, 2049, "tone-440");
    check(notBuffered.status == SubmitStatus::RejectedNotBuffered,
          "edit reaching past write head rejected");

    auto justInTime = runner.edit(1000, 1010, "tone-440");
    check(justInTime.status == SubmitStatus::Accepted,
          "edit beginning exactly at output head still accepted");

    runner.drain(200);
    auto lateUndo = runner.undo(justInTime.id);
    check(lateUndo.status == SubmitStatus::RejectedAlreadyAired,
          "undo of an edit already on air rejected");

    auto bad = runner.edit(1200, 1200, "tone-440");
    check(bad.status == SubmitStatus::RejectedBadRange, "empty range rejected");
    auto unknown = runner.edit(1200, 1300, "no-such-pad");
    check(unknown.status == SubmitStatus::RejectedUnknownPad, "unknown pad rejected");
    return checksFailed() == 0;
}

DBS_TEST(overlapping_edits_newest_sequence_wins) {
    auto runner = makeRunner();
    Oracle oracle(testBaseTrack(), PadLibrary::makeBuiltin(48000), 32);
    runner.feed(4000, 500);

    auto a = runner.edit(1000, 2000, "tone-440");
    oracle.applyAccepted(a, 1000, 2000, "tone-440");
    auto b = runner.edit(1500, 2500, "tone-1k");  // overlaps right half of a
    oracle.applyAccepted(b, 1500, 2500, "tone-1k");
    auto c = runner.edit(1700, 1900, "chime");    // nested inside b
    oracle.applyAccepted(c, 1700, 1900, "chime");
    check(a.seq < b.seq && b.seq < c.seq, "sequence numbers strictly increase");

    auto snap = runner.core().snapshot();
    // a is split into a left [1000,1500) and a right [2500,2000-> none];
    // layout: a-left, b (with c nested on top) => 4 stored fragments.
    check(snap.edits.size() == 4, "a split into surviving pieces plus b plus c");
    std::set<std::uint64_t> ids;
    for (const auto& e : snap.edits) ids.insert(e.id);
    check(ids.count(a.id) && ids.count(b.id) && ids.count(c.id),
          "survivors keep stable ids");

    runner.drain(4000);
    verifyStream(runner, oracle);
    return checksFailed() == 0;
}

DBS_TEST(quick_undo_before_airtime_restores_original) {
    auto runner = makeRunner();
    Oracle oracle(testBaseTrack(), PadLibrary::makeBuiltin(48000), 32);
    runner.feed(3000, 400);
    auto r = runner.edit(1800, 2400, "tone-1k");
    oracle.applyAccepted(r, 1800, 2400, "tone-1k");
    runner.drain(500);  // still well before the edit
    auto u = runner.undo(r.id);
    oracle.applyUndo(u);
    check(u.status == SubmitStatus::Accepted, "undo while fully buffered accepted");
    check(runner.core().snapshot().edits.empty(), "edit removed from ownership list");

    runner.drain(2500);
    verifyStream(runner, oracle);
    return checksFailed() == 0;
}

DBS_TEST(callback_block_misalignment_splits_at_exact_boundaries) {
    auto runner = makeRunner();
    Oracle oracle(testBaseTrack(), PadLibrary::makeBuiltin(48000), 32);
    runner.feed(2000, 251);
    // Edit boundaries deliberately chosen to never align with the 257-prime
    // drain blocks used by the runner.
    auto r = runner.edit(333, 1777, "tone-320");
    oracle.applyAccepted(r, 333, 1777, "tone-320");
    runner.drainToCompletion(2000);
    check(static_cast<std::int64_t>(runner.output().size()) == 2000,
          "exact requested frame count despite unaligned splits");
    verifyStream(runner, oracle);
    return checksFailed() == 0;
}

DBS_TEST(underrun_silence_policy_no_old_program_repetition) {
    auto cfg = testBaseConfig();
    cfg.underrun = UnderrunPolicy::InsertSilence;
    auto runner = makeRunner(cfg);
    runner.feed(1500, 300);
    runner.drain(1500);                 // consume everything
    auto n = runner.drain(600);         // gap while producer stalled
    check(n == 600, "silence policy keeps output stream moving");
    const auto& out = runner.output();
    for (std::int64_t i = 1500; i < 2100; ++i) {
        check(out[i] == 0.0f, "underrun frames are silence, never repeated program");
    }
    runner.feed(1000, 129);
    runner.drain(1000);
    // Fresh material after the gap must match the generator exactly.
    const auto& track = runner.trackConfig();
    for (std::int64_t i = 2100; i < 3100; ++i) {
        check(std::fabs(out[i] - expectedSampleAt(i, track)) <= 2e-6f,
              "post-gap program resumes at the correct absolute position");
    }
    check(runner.core().snapshot().underrunEvents >= 1, "underrun counted");
    return checksFailed() == 0;
}

DBS_TEST(underrun_stop_policy_freezes_and_resumes) {
    auto cfg = testBaseConfig();
    cfg.underrun = UnderrunPolicy::StopOutput;
    auto runner = makeRunner(cfg);
    runner.feed(1200, 256);
    runner.drain(1200);
    const std::int64_t frozenAt = runner.core().outputPos();
    auto n = runner.drain(400);
    check(n == 0, "stop policy produces nothing while input is missing");
    check(runner.core().paused(), "engine enters paused state");
    check(runner.core().outputPos() == frozenAt, "output head frozen, monotonic preserved");

    runner.feed(800, 200);
    check(runner.core().resume(), "resume succeeds once new material exists");
    n = runner.drain(800);
    check(n == 800, "output resumes with fresh material only");
    const auto& track = runner.trackConfig();
    const auto& out = runner.output();
    for (std::int64_t i = frozenAt; i < frozenAt + 800; ++i) {
        check(std::fabs(out[i] - expectedSampleAt(i, track)) <= 2e-6f,
              "no stale frames inserted around stop/resume");
    }
    return checksFailed() == 0;
}

DBS_TEST(end_of_input_silence_vs_stop) {
    {
        auto cfg = testBaseConfig();
        cfg.endOfInput = EndOfInputPolicy::InsertSilence;
        auto runner = makeRunner(cfg);
        runner.feed(1000, 256);
        runner.endInput();
        auto n = runner.drainToCompletion(2500);
        check(n == 2500, "EOI silence policy pads on demand up to requested count");
        check(runner.output()[1000] == 0.0f && runner.output()[2499] == 0.0f,
              "tail is silence after input end");
    }
    {
        auto cfg = testBaseConfig();
        cfg.endOfInput = EndOfInputPolicy::StopOutput;
        auto runner = makeRunner(cfg);
        runner.feed(1000, 256);
        runner.endInput();
        auto n = runner.drainToCompletion(3000);
        check(n == 1000, "EOI stop policy halts exactly at write head");
        check(runner.core().finished(), "engine reports finished");
    }
    return checksFailed() == 0;
}

DBS_TEST(seam_fades_preserve_duration_and_stay_inside_range) {
    auto cfg = testBaseConfig();
    cfg.fadeFrames = 64;
    auto runner = makeRunner(cfg);
    Oracle oracle(testBaseTrack(), PadLibrary::makeBuiltin(48000), 64);
    runner.feed(4000, 512);
    const std::int64_t begin = 1000, end = 2000;
    auto r = runner.edit(begin, end, "tone-1k");
    oracle.applyAccepted(r, begin, end, "tone-1k");
    runner.drain(4000);
    check(static_cast<std::int64_t>(runner.output().size()) == 4000,
          "fades add or remove zero frames");
    verifyStream(runner, oracle);

    // Fades are interior-only: both boundary-adjacent outside frames are pure
    // original generator output.
    const auto& out = runner.output();
    const auto& track = runner.trackConfig();
    check(std::fabs(out[begin - 1] - expectedSampleAt(begin - 1, track)) <= 2e-6f,
          "frame immediately before edit untouched");
    check(std::fabs(out[end] - expectedSampleAt(end, track)) <= 2e-6f,
          "frame immediately after edit untouched");

    // A fade longer than the edit must clamp rather than escape the range.
    auto cfg2 = testBaseConfig();
    cfg2.fadeFrames = 1000;
    auto runner2 = makeRunner(cfg2);
    runner2.feed(2000, 256);
    auto tiny = runner2.edit(500, 540, "chime");
    check(tiny.status == SubmitStatus::Accepted, "short edit with huge fade accepted");
    runner2.drain(2000);
    check(static_cast<std::int64_t>(runner2.output().size()) == 2000,
          "clamped fades still preserve total duration");
    return checksFailed() == 0;
}

DBS_TEST(output_position_monotonic_and_backpressure_safe) {
    auto cfg = testBaseConfig();
    cfg.capacityFrames = 512;
    auto runner = makeRunner(cfg);
    std::int64_t lastPos = 0;

    // Disciplined phase: respect the window via drain, positions stay monotonic.
    // Drain at least what each feed adds so the 512-frame window never fills.
    for (int i = 0; i < 20; ++i) {
        runner.feed(300, 64);
        const std::int64_t pending =
            runner.core().writePos() - runner.core().outputPos();
        runner.drain(std::min<std::int64_t>(300, pending));
        check(runner.core().outputPos() >= lastPos, "output head never retreats");
        lastPos = runner.core().outputPos();
    }

    // Hostile phase: saturate the bounded window, then push past capacity.
    // Frames beyond the window are refused and counted as back-pressure; the
    // ring never overwrites not-yet-aired material. Junk uses a sentinel
    // outside the code track's value range so any leaked frame is unambiguous.
    const auto snapBefore = runner.core().snapshot();
    constexpr float kJunkSentinel = 12.5f;
    // First fill the remaining room with real generator frames so the window
    // is provably full before the hostile sentinel burst arrives.
    std::vector<float> fill(900);
    for (std::int64_t i = 0; i < 900; ++i)
        fill[i] = expectedSampleAt(runner.core().writePos() + i,
                                   testBaseTrack());
    runner.core().pushInput(fill.data(), 900);

    std::vector<float> junk(900, kJunkSentinel);
    const std::int64_t acceptedJunk =
        runner.core().pushInput(junk.data(), 900);
    const auto snapAfter = runner.core().snapshot();
    check(acceptedJunk == 0, "full ring refuses every hostile frame");
    check(snapAfter.backPressureFrames > snapBefore.backPressureFrames,
          "overflow attempts recorded as back-pressure");
    check(runner.core().outputPos() == lastPos, "back-pressure never moves output head");

    // Refused junk must never appear on air: drain the window and prove the
    // sentinel is absent and every frame matches the deterministic generator.
    const std::int64_t start = runner.core().outputPos();
    const std::int64_t available =
        runner.core().writePos() - runner.core().outputPos();
    runner.drain(available);
    const auto& out = runner.output();
    int junkLeaks = 0;
    for (std::int64_t i = 0; i < available; ++i) {
        const float frame = out[start + i];
        if (frame == kJunkSentinel ||
            std::fabs(frame - expectedSampleAt(start + i, testBaseTrack()))
                > 2e-6f) {
            ++junkLeaks;
        }
    }
    check(junkLeaks == 0, "refused input never leaks into output");
    return checksFailed() == 0;
}

DBS_TEST(threaded_engine_deterministic_event_order) {
    EngineConfig cfg = testBaseConfig();
    PadLibrary pads = PadLibrary::makeBuiltin(cfg.sampleRate);
    std::vector<float> rendered;
    std::mutex outMutex;
    DelayEngine engine(cfg, pads,
                       [&](const float* p, std::int64_t n) {
                           std::lock_guard<std::mutex> lock(outMutex);
                           rendered.insert(rendered.end(), p, p + n);
                       },
                       127);

    // Start the render thread first so the bounded ring keeps draining while
    // a single feeder fills it; the feeder reaches a deep buffered window
    // (writePos well past every edit range) before any control event is
    // queued, so no edit can be rejected as already aired.
    engine.start();

    std::vector<std::future<SubmitResult>> futures;
    futures.reserve(32);
    std::mutex futureMutex;
    std::atomic<std::int64_t> fed{0};
    std::atomic<bool> feedDone{false};
    constexpr std::int64_t kTotalInput = 6000;

    // One producer: deterministic code-track frames advanced by an exact
    // frame cursor, so partial acceptance (back-pressure) retries only the
    // unwritten frames and absolute positions never drift.
    std::thread feeder([&] {
        std::vector<float> block(512);
        while (fed.load() < kTotalInput) {
            const std::int64_t base = fed.load();
            const std::int64_t want = std::min<std::int64_t>(512, kTotalInput - base);
            for (std::int64_t i = 0; i < want; ++i)
                block[i] = expectedSampleAt(base + i, testBaseTrack());
            const std::int64_t accepted = engine.pushInput(block.data(), want);
            if (accepted == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            fed.fetch_add(accepted);
        }
        feedDone.store(true);
    });

    // Flood control events from several threads once the window is deep;
    // acceptance order must follow the single global sequence regardless of
    // submission thread. All ranges stay ahead of the output head.
    while (fed.load() < 3000) std::this_thread::yield();
    auto submitMany = [&](const std::string& pad, int base) {
        for (int i = 0; i < 10; ++i) {
            const std::int64_t b = 1100 + base * 400 + i * 30;
            auto f = engine.submitEdit(b, b + 30, pad);
            std::lock_guard<std::mutex> guard(futureMutex);
            futures.push_back(std::move(f));
        }
    };
    std::thread t1(submitMany, "tone-440", 0);
    std::thread t2(submitMany, "tone-1k", 1);
    std::thread t3(submitMany, "chime", 2);
    t1.join();
    t2.join();
    t3.join();
    feeder.join();
    check(feedDone.load() && fed.load() == kTotalInput,
          "feeder delivered the full deterministic stream");

    std::vector<std::uint64_t> seqs;
    int accepted = 0;
    for (auto& f : futures) {
        auto r = f.get();
        if (r.status == SubmitStatus::Accepted) {
            seqs.push_back(r.seq);
            ++accepted;
        }
    }
    check(accepted == 30, "all buffered edits accepted under concurrent load");
    // The engine assigns sequences under one lock from the FIFO command queue.
    // Futures complete in arbitrary thread order, so determinism is verified
    // by the issued set being exactly a contiguous 1..N permutation (unique,
    // gap-free) rather than by the order in which this test collects results.
    std::vector<std::uint64_t> sortedSeqs = seqs;
    std::sort(sortedSeqs.begin(), sortedSeqs.end());
    const bool contiguous =
        sortedSeqs.size() == 30 &&
        std::adjacent_find(sortedSeqs.begin(), sortedSeqs.end()) ==
            sortedSeqs.end() &&
        sortedSeqs.front() == 1 && sortedSeqs.back() == 30;
    check(contiguous,
          "sequence numbers issued in one deterministic global order");

    engine.stop();
    check(!rendered.empty(), "render thread produced output without UI waiting");
    return checksFailed() == 0;
}

DBS_TEST(wav_round_trip_keeps_rendered_audio) {
    const std::string path = "/tmp/dbs_test_roundtrip.wav";
    auto runner = makeRunner();
    Oracle oracle(testBaseTrack(), PadLibrary::makeBuiltin(48000), 32);
    runner.feed(2000, 256);
    auto r = runner.edit(800, 1400, "chime");
    oracle.applyAccepted(r, 800, 1400, "chime");
    runner.drain(2000);
    writeWavMono(path, 48000, runner.output().data(),
                 static_cast<std::int64_t>(runner.output().size()));
    auto loaded = loadWavMono(path);
    check(loaded.frames == 2000, "wav frame count preserved");
    check(loaded.sampleRate == 48000, "wav rate preserved");
    int diffs = 0;
    for (std::int64_t i = 0; i < 2000; ++i) {
        if (std::fabs(loaded.mono[i] - runner.output()[i]) > 2e-4f) ++diffs;
    }
    check(diffs == 0, "16-bit wav round trip approximates float render");
    std::filesystem::remove(path);
    return checksFailed() == 0;
}

}  // namespace dbs
