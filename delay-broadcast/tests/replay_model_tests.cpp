#include "test_framework.h"

#include "fault_engines.h"
#include "reference_model.h"
#include "replay_harness.h"
#include "replay_scenarios.h"
#include "ring_regression.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace dbs {
namespace {

// Bridge the local ProbeReport counters into the global DBS_TEST checker so
// failures surface inside the registered suite as well as the standalone
// acceptance entry.
void bridge(const tst::ProbeReport& r) {
    check(r.ok(), "all independent reference-model/ring probes pass");
}

}  // namespace

DBS_TEST(ring_regression_independent_shadow_model) {
    // Positive case: production ring across several capacities, including
    // non-power-of-two requests and many physical wrap crossings. Every probe
    // must pass.
    for (std::size_t cap : {64, 100, 257, 1024}) {
        bridge(tst::runRingProbes<RingBuffer>(cap));
    }
    // Negative cases: each injected ring fault must be detected, i.e. its
    // probe report must carry at least one failure. These are expected
    // rejections, so they run quietly and are not bridged into the global
    // failure counter.
    check(!tst::runRingProbes<tst::OverwriteRing>(256, false).ok(),
          "overwrite-on-wrap mutant is rejected");
    check(!tst::runRingProbes<tst::WrongMaskRing>(256, false).ok(),
          "wrong-wrap-mask mutant is rejected");
    check(!tst::runRingProbes<tst::StaleReadRing>(256, false).ok(),
          "stale-read mutant is rejected");
    return checksFailed() == 0;
}

DBS_TEST(replacement_reference_model_replay_all_callback_cuttings) {
    const auto cfg = tst::scenarioConfig();
    struct Case {
        std::string name;
        EngineConfig config;
        std::vector<tst::Event> events;
    };
    const std::vector<Case> cases = {
        {"wrap", cfg, tst::wrapScenario()},
        {"overlap", cfg, tst::overlapScenario()},
        {"cancel", cfg, tst::cancelScenario()},
        {"near-air", cfg, tst::nearAirScenario()},
        {"underrun", cfg, tst::underrunScenario()},
        {"delayed", tst::delayedConfig(), tst::delayedScenario()},
    };
    const tst::ChunkPolicy policies[] = {
        tst::ChunkPolicy::AsScripted, tst::ChunkPolicy::AllOne,
        tst::ChunkPolicy::PrimeBlocks, tst::ChunkPolicy::LargeBlocks,
        tst::ChunkPolicy::Alternating};

    for (const Case& tc : cases) {
        std::vector<float> baseline;
        for (tst::ChunkPolicy policy : policies) {
            EngineCore engine(tc.config,
                              PadLibrary::makeBuiltin(tc.config.sampleRate));
            tst::ReferenceModel model(
                tc.config, PadLibrary::makeBuiltin(tc.config.sampleRate),
                tst::scenarioTrack());
            tst::ProbeReport report;
            auto art = tst::replay(engine, model, tc.events, policy, report);
            tst::verifyRecording(art.sink, model, report);
            bridge(report);
            if (baseline.empty()) {
                baseline = art.sink.flat();
            } else {
                check(baseline == art.sink.flat(),
                      "callback re-cutting never changes recorded samples");
            }
        }
    }
    return checksFailed() == 0;
}

DBS_TEST(stop_output_underrun_matches_reference_model) {
    auto cfg = tst::scenarioConfig();
    cfg.underrun = UnderrunPolicy::StopOutput;
    cfg.endOfInput = EndOfInputPolicy::StopOutput;
    tst::ProbeReport report;
    EngineCore engine(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
    tst::ReferenceModel model(cfg, PadLibrary::makeBuiltin(cfg.sampleRate),
                              tst::scenarioTrack());
    auto art = tst::replay(engine, model, tst::stopUnderrunScenario(),
                           tst::ChunkPolicy::PrimeBlocks, report);
    tst::verifyRecording(art.sink, model, report);
    bridge(report);
    return checksFailed() == 0;
}

DBS_TEST(out_of_order_control_is_deterministic_and_verified) {
    tst::ProbeReport report;
    tst::outOfOrderControlProbe(
        [&](EngineConfig c) {
            return std::unique_ptr<EngineCore>(
                new EngineCore(c, PadLibrary::makeBuiltin(c.sampleRate)));
        },
        report);
    bridge(report);
    return checksFailed() == 0;
}

DBS_TEST(input_backpressure_fault_injection_keeps_stream_clean) {
    tst::ProbeReport report;
    const auto cfg = tst::scenarioConfig();
    EngineCore engine(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
    tst::ReferenceModel model(cfg, PadLibrary::makeBuiltin(cfg.sampleRate),
                              tst::scenarioTrack());
    tst::backpressureProbe(engine, model, report);
    bridge(report);
    return checksFailed() == 0;
}

DBS_TEST(faulty_delayed_broadcast_engines_are_rejected) {
    // Controlled errors in the delayed broadcast must each fail the same
    // receiver/reference assertions that the good engine passes. This proves
    // the suite can actually reject bad results rather than rubber-stamping.
    const auto cfg = tst::scenarioConfig();
    auto exercise = [&](auto& mutant) {
        tst::ProbeReport report;
        report.loud = false;  // failures are the expected outcome here
        {
            tst::ReferenceModel model(
                cfg, PadLibrary::makeBuiltin(cfg.sampleRate),
                tst::scenarioTrack());
            auto art = tst::replay(mutant, model, tst::underrunScenario(),
                                   tst::ChunkPolicy::PrimeBlocks, report);
            tst::verifyRecording(art.sink, model, report);
        }
        {
            tst::ReferenceModel model(
                cfg, PadLibrary::makeBuiltin(cfg.sampleRate),
                tst::scenarioTrack());
            auto art = tst::replay(mutant, model, tst::overlapScenario(),
                                   tst::ChunkPolicy::Alternating, report);
            tst::verifyRecording(art.sink, model, report);
        }
        return report;
    };

    {
        tst::StaleReplayEngine m(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
        check(!exercise(m).ok(), "stale-repeat underrun fault is rejected");
    }
    {
        tst::ShiftedFrameEngine m(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
        check(!exercise(m).ok(), "frame-shift fault is rejected");
    }
    {
        tst::BoundaryBleedEngine m(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
        check(!exercise(m).ok(), "boundary bleed fault is rejected");
    }
    {
        tst::DropFrameEngine m(cfg, PadLibrary::makeBuiltin(cfg.sampleRate));
        check(!exercise(m).ok(), "dropped-frame fault is rejected");
    }
    return checksFailed() == 0;
}

}  // namespace dbs
