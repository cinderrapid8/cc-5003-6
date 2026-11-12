// Automated acceptance entry for ring-buffer regression and the independent
// replacement reference model. Exit code 0 requires both:
//   1. every positive probe passes against the production implementations;
//   2. every injected fault (ring mutants and delayed-broadcast engine
//      mutants) is rejected by the same assertions.
//
// Expected values throughout are derived from independent analytic generators
// and a shadow model; the ring under test and the engine are never read to
// manufacture an answer.

#include "fault_engines.h"
#include "reference_model.h"
#include "replay_harness.h"
#include "replay_scenarios.h"
#include "ring_regression.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace dbs;
using namespace dbs::tst;

namespace {

int g_failures = 0;

void section(const char* title) {
    std::printf("\n== %s ==\n", title);
}

void require(const std::string& name, ProbeReport r, bool mustFail) {
    if (mustFail) r.loud = false;  // expected rejections stay quiet
    const bool behaved = mustFail ? !r.ok() : r.ok();
    std::printf("  [%s] %s (%lld checks, %lld failures)\n",
                behaved ? "OK" : "BAD", name.c_str(),
                static_cast<long long>(r.checks),
                static_cast<long long>(r.failures));
    if (!behaved) ++g_failures;
}

// Replay one scenario against a fresh engine under every callback cutting,
// asserting all replays agree with the independent per-sample model and with
// each other on recorded content.
template <typename Engine>
ProbeReport replayScenarioAllCuttings(
    const std::string& name, const EngineConfig& cfg,
    const std::vector<Event>& events, bool requireReplacement) {
    ProbeReport combined;
    std::vector<float> firstFlat;
    const ChunkPolicy policies[] = {
        ChunkPolicy::AsScripted, ChunkPolicy::AllOne,
        ChunkPolicy::PrimeBlocks, ChunkPolicy::LargeBlocks,
        ChunkPolicy::Alternating};
    for (ChunkPolicy policy : policies) {
        Engine engine(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
        ReferenceModel model(cfg, PadLibrary::makeBuiltin(cfg.sampleRate),
                             scenarioTrack());
        auto art = replay(engine, model, events, policy, combined);
        verifyRecording(art.sink, model, combined, requireReplacement);
        if (firstFlat.empty()) {
            firstFlat = art.sink.flat();
        } else {
            combined.expect(firstFlat == art.sink.flat(),
                "replay under a different callback cutting is sample-identical");
        }
    }
    std::printf("    scenario %-14s replay cuttings: %lld checks\n",
                name.c_str(), static_cast<long long>(combined.checks));
    return combined;
}

}  // namespace

int main() {
    std::printf("== delay-broadcast ring/replacement acceptance ==\n");

    // ---- 1. Ring-buffer regression: positive probes ---------------------
    section("ring positive probes (production RingBuffer)");
    const std::size_t capacities[] = {64, 100, 257, 1024};
    for (std::size_t cap : capacities) {
        require("RingBuffer cap=" + std::to_string(cap),
                runRingProbes<RingBuffer>(cap), false);
    }

    // ---- 2. Ring mutants: each must be rejected -------------------------
    section("ring fault injection (mutants must fail probes)");
    require("OverwriteRing rejected",
            runRingProbes<OverwriteRing>(256, false), true);
    require("WrongMaskRing rejected",
            runRingProbes<WrongMaskRing>(256, false), true);
    require("StaleReadRing rejected",
            runRingProbes<StaleReadRing>(256, false), true);

    // ---- 3. Replacement reference model: positive scenario replays ------
    section("replacement scenarios against the independent reference model");
    const EngineConfig cfg = scenarioConfig();
    require("wrap-around",
            replayScenarioAllCuttings<EngineCore>(
                "wrap", cfg, wrapScenario(), true), false);
    require("overlapping requests",
            replayScenarioAllCuttings<EngineCore>(
                "overlap", cfg, overlapScenario(), true), false);
    require("cancellation",
            replayScenarioAllCuttings<EngineCore>(
                "cancel", cfg, cancelScenario(), true), false);
    require("near-airtime boundaries",
            replayScenarioAllCuttings<EngineCore>(
                "near-air", cfg, nearAirScenario(), true), false);
    require("input underrun (silence)",
            replayScenarioAllCuttings<EngineCore>(
                "underrun", cfg, underrunScenario(), true), false);

    {
        EngineConfig stopCfg = cfg;
        stopCfg.underrun = UnderrunPolicy::StopOutput;
        stopCfg.endOfInput = EndOfInputPolicy::StopOutput;
        require("input underrun (stop-output)",
                replayScenarioAllCuttings<EngineCore>(
                    "stop", stopCfg, stopUnderrunScenario(), true), false);
    }
    require("fixed delay with wraparound",
            replayScenarioAllCuttings<EngineCore>(
                "delayed", delayedConfig(), delayedScenario(), true), false);

    // ---- 4. Out-of-order control and back-pressure fault injection ------
    section("control/back-pressure fault injection");
    {
        ProbeReport ooo;
        outOfOrderControlProbe(
            [&](EngineConfig c) {
                return std::unique_ptr<EngineCore>(
                    new EngineCore(c, PadLibrary::makeBuiltin(c.sampleRate)));
            },
            ooo);
        require("out-of-order control determinism", ooo, false);
    }
    {
        ProbeReport bp;
        EngineCore engine(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
        ReferenceModel model(cfg, PadLibrary::makeBuiltin(cfg.sampleRate),
                             scenarioTrack());
        backpressureProbe(engine, model, bp);
        require("back-pressure never leaks hostile frames", bp, false);
    }

    // ---- 5. Delayed-broadcast engine mutants: all must be rejected ------
    section("delayed-broadcast fault injection (mutants must be rejected)");
    auto runMutant = [&](const std::string& name, auto make) {
        ProbeReport report;
        report.loud = false;  // mutant failures are the expected outcome
        // Underrun and boundary scenarios give every fault a chance to show.
        {
            auto engine = make(cfg);
            ReferenceModel model(cfg,
                                 PadLibrary::makeBuiltin(cfg.sampleRate),
                                 scenarioTrack());
            auto art = replay(*engine, model, underrunScenario(),
                              ChunkPolicy::PrimeBlocks, report);
            verifyRecording(art.sink, model, report);
        }
        {
            auto engine = make(cfg);
            ReferenceModel model(cfg,
                                 PadLibrary::makeBuiltin(cfg.sampleRate),
                                 scenarioTrack());
            auto art = replay(*engine, model, overlapScenario(),
                              ChunkPolicy::Alternating, report);
            verifyRecording(art.sink, model, report);
        }
        require(name, report, true);
    };

    runMutant("StaleReplayEngine rejected", [&](EngineConfig c) {
        return std::make_unique<StaleReplayEngine>(
            c, PadLibrary::makeBuiltin(c.sampleRate));
    });
    runMutant("ShiftedFrameEngine rejected", [&](EngineConfig c) {
        return std::make_unique<ShiftedFrameEngine>(
            c, PadLibrary::makeBuiltin(c.sampleRate));
    });
    runMutant("BoundaryBleedEngine rejected", [&](EngineConfig c) {
        return std::make_unique<BoundaryBleedEngine>(
            c, PadLibrary::makeBuiltin(c.sampleRate));
    });
    runMutant("DropFrameEngine rejected", [&](EngineConfig c) {
        return std::make_unique<DropFrameEngine>(
            c, PadLibrary::makeBuiltin(c.sampleRate));
    });

    std::printf("\nacceptance %s\n", g_failures == 0 ? "PASSED" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
