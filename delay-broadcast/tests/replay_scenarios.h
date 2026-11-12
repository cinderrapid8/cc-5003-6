#pragma once

#include "probe_report.h"
#include "reference_model.h"
#include "replay_harness.h"

#include "../src/audio_types.h"
#include "../src/delay_engine.h"
#include "../src/pads.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dbs {
namespace tst {

inline EngineConfig scenarioConfig() {
    EngineConfig cfg;
    cfg.sampleRate = 48000;
    cfg.capacityFrames = 1024;   // small ring forces repeated wraparound
    cfg.delayFrames = 0;
    cfg.fadeFrames = 32;
    cfg.underrun = UnderrunPolicy::InsertSilence;
    cfg.endOfInput = EndOfInputPolicy::InsertSilence;
    return cfg;
}

inline CodeTrackConfig scenarioTrack() {
    CodeTrackConfig t;
    t.sampleRate = 48000;
    t.blockFrames = 256;
    return t;
}

// Feed/drain discipline that never overflows the 1024-frame bounded ring:
// each feed is followed (or preceded) by a drain of at least the feed size.
inline std::vector<Event> wrapScenario() {
    return {
        evFeed(900, 127),
        evEdit(500, 560, "tone-440"),
        evPull(800, 131),
        evFeed(700, 97),
        evEdit(950, 1100, "chime"),
        evPull(700, 211),
        evFeed(700, 53),
        evEdit(1400, 1500, "tone-1k"),
        evPull(700, 333),
        evFeed(700, 199),
        evPull(700, 61),
        evEndInput(),
        evPull(1024, 257),
    };
}

// Overlapping replacement requests: newest sequence wins, including full
// cover, partial cover and nested edits at awkward boundaries.
inline std::vector<Event> overlapScenario() {
    return {
        evFeed(900, 127),
        evEdit(100, 600, "tone-440"),
        evPull(200, 131),
        evEdit(300, 800, "tone-1k"),
        evEdit(400, 500, "chime"),
        evEdit(150, 250, "tone-320"),
        evPull(700, 211),
        evEndInput(),
        evPull(128, 37),
    };
}

// Cancellation before airtime restores original material; a late undo and an
// undo of an unknown id must both be rejected without changing output.
inline std::vector<Event> cancelScenario() {
    return {
        evFeed(900, 127),
        evEdit(400, 700, "tone-1k"),                 // id 1
        evPull(100, 64),
        evUndo(1),                                   // quick undo
        evEdit(500, 600, "chime"),                   // id 2
        evPull(550, 211),                            // airs id 2
        evUndo(2),                                   // already aired
        evUndo(999),                                 // unknown
        evEndInput(),
        evPull(512, 257),
    };
}

// Submissions right up against the output head and the write head, exercising
// every rejection class plus an exactly-at-head acceptance.
inline std::vector<Event> nearAirScenario() {
    return {
        evFeed(900, 127),
        evPull(500, 131),                       // outputPos = 500
        evEdit(400, 600, "tone-440"),          // already aired
        evEdit(700, 901, "tone-440"),          // not buffered
        evEdit(500, 500, "tone-440"),          // bad range
        evEdit(500, 540, "no-such"),           // unknown pad
        evEdit(500, 540, "chime"),             // accepted
        evPull(400, 211),
        evEndInput(),
        evPull(256, 37),
    };
}

// Input underrun under the silence policy: a producer stall inserts silence
// in its own absolute slots; resumed real input lands at correct positions and
// stale program is never repeated.
inline std::vector<Event> underrunScenario() {
    return {
        evFeed(800, 127),
        evEdit(200, 350, "tone-1k"),
        evPull(800, 131),
        evPull(400, 97),    // stall: only silence can air
        evFeed(600, 53),    // resumes at absolute pos 1200
        evEdit(1300, 1400, "chime"),
        evPull(600, 211),
        evEndInput(),
        evPull(512, 257),
    };
}

// Stop-output underrun: output freezes at the stall, resumes only after fresh
// input, and the head never retreats or replays an old frame.
inline std::vector<Event> stopUnderrunScenario() {
    return {
        evFeed(800, 127),
        evEdit(200, 350, "tone-1k"),
        evPull(800, 131),
        evPull(300, 97),    // freezes: zero frames produced
        evResume(),
        evFeed(600, 53),
        evResume(),         // now succeeds
        evPull(600, 211),
        evEndInput(),
        evPull(256, 257),
    };
}

// Fixed-delay gate with wraparound: startup prebuffering must wait, and edits
// spanning multiple ring wraps still map to stable absolute positions.
inline std::vector<Event> delayedScenario() {
    return {
        evFeed(900, 127),    // prebuffer fills, nothing airs
        evEdit(300, 700, "chime"),
        evPull(400, 131),    // first air only after delay gate
        evFeed(500, 97),
        evEdit(1000, 1150, "tone-440"),
        evPull(500, 211),
        evFeed(500, 53),
        evPull(500, 333),
        evEndInput(),
        evPull(768, 257),
    };
}

inline EngineConfig delayedConfig() {
    EngineConfig cfg = scenarioConfig();
    cfg.delayFrames = 128;
    return cfg;
}

// Fault injection: hostile writes against a saturated bounded window must be
// refused (back-pressure), must not move any head, and sentinel junk can never
// reach the receiver. This drives the engine directly rather than the replay
// feeder, which guarantees full acceptance by construction.
template <typename Engine>
void backpressureProbe(Engine& engine, const ReferenceModel& model,
                       ProbeReport& report) {
    constexpr float kJunk = 12.5f;
    // Fill the bounded window with genuine generator frames first.
    std::int64_t total = static_cast<std::int64_t>(
        model.engineConfigForTest().capacityFrames);
    std::vector<float> fill(static_cast<std::size_t>(total));
    for (std::int64_t i = 0; i < total; ++i)
        fill[static_cast<std::size_t>(i)] =
            expectedSampleAt(engine.writePos() + i, model.trackConfigForTest());
    const std::int64_t accepted = engine.pushInput(fill.data(), total);
    report.expect(accepted == total, "window fills with exactly capacity frames");

    std::vector<float> junk(256, kJunk);
    const std::int64_t before = engine.writePos();
    const std::int64_t refused = engine.pushInput(junk.data(), 256);
    report.expect(refused == 0, "saturated ring refuses the entire hostile burst");
    report.expect(engine.writePos() == before,
                  "refused write never advances the write head");

    RecordingSink sink;
    std::vector<float> dst(256);
    std::int64_t produced = engine.pullOutput(dst.data(), 256);
    sink.record(dst.data(), produced);
    for (std::int64_t i = 0; i < produced; ++i) {
        const std::int64_t abs = i;
        report.expect(sink.at(abs) != kJunk,
                      "junk sentinel never reaches the receiver");
        report.expect(std::fabs(sink.at(abs) -
                                expectedSampleAt(abs, model.trackConfigForTest()))
                                  <= 2e-6f,
                      "aired frames are the genuine generator stream");
    }
}

// Deterministic permutation (LCG) so "out of order" control is reproducible
// rather than thread-scheduled. The same edit set submitted in two distinct
// orders must produce two distinct, internally consistent outputs, and each
// output must still match its own independently replayed model.
inline std::vector<std::size_t> deterministicPermutation(std::size_t n,
                                                         std::uint64_t seed) {
    std::vector<std::size_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = i;
    std::uint64_t state = seed;
    for (std::size_t i = n; i > 1; --i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t j = static_cast<std::size_t>(state % i);
        std::swap(idx[i - 1], idx[j]);
    }
    return idx;
}

struct EditSpec {
    std::int64_t begin, end;
    std::string pad;
};

// Out-of-order control injection: submit overlapping edits in scripted order
// and in a permuted order to two fresh engines; verify each against its own
// reference replay and prove the order actually changes the rendered result.
template <typename EngineFactory>
void outOfOrderControlProbe(EngineFactory makeEngine, ProbeReport& report) {
    const EngineConfig cfg = scenarioConfig();
    const auto track = scenarioTrack();
    const std::vector<EditSpec> specs = {
        {100, 700, "tone-440"},
        {300, 800, "tone-1k"},
        {450, 650, "chime"},
        {200, 500, "tone-320"},
        {600, 750, "tone-1k"},
    };

    auto runOrder = [&](const std::vector<std::size_t>& order) {
        auto engine = makeEngine(cfg);
        ReferenceModel model(cfg, PadLibrary::makeBuiltin(cfg.sampleRate), track);
        std::vector<Event> events;
        events.push_back(evFeed(900, 127));
        for (std::size_t k : order) {
            const EditSpec& s = specs[k];
            events.push_back(evEdit(s.begin, s.end, s.pad));
        }
        events.push_back(evEndInput());
        events.push_back(evPull(900, 257));
        auto art = replay(*engine, model, events, ChunkPolicy::AsScripted, report);
        verifyRecording(art.sink, model, report);
        return art.sink.flat();
    };

    const auto orderA = deterministicPermutation(specs.size(), 11);
    const auto orderB = deterministicPermutation(specs.size(), 977);
    report.expect(orderA != orderB, "permutation seed produces distinct orders");
    const auto flatA = runOrder(orderA);
    const auto flatB = runOrder(orderB);
    report.expect(flatA.size() == flatB.size(),
                  "both orders render the same frame count");
    int differing = 0;
    for (std::size_t i = 0; i < flatA.size(); ++i) {
        if (std::fabs(flatA[i] - flatB[i]) > 1e-6f) ++differing;
    }
    report.expect(differing > 0,
                  "out-of-order control visibly changes replacement ownership");
}

}  // namespace tst
}  // namespace dbs
