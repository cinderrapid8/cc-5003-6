#include "../src/delay_engine.h"
#include "../src/logger.h"
#include "../src/pads.h"
#include "../src/script_runner.h"
#include "../src/wav_file.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dbs;

namespace {

struct SimArgs {
    std::string scenario = "single";
    std::string outWav = "render.wav";
    std::string logPath;
    std::int64_t inputFrames = 48000;
    int capacityMs = 8000;
    int delayMs = 3000;
    int fadeMs = 5;
    UnderrunPolicy gap = UnderrunPolicy::InsertSilence;
    EndOfInputPolicy eoi = EndOfInputPolicy::InsertSilence;
    bool quiet = false;
};

void usage() {
    std::printf(
        "delay-sim — local delayed-broadcast simulator (demo audio only)\n"
        "Usage: delay-sim [options]\n"
        "  --scenario NAME     single|wrap|underrun|underrun-stop|overlap|"
        "undo|near-air|eoi-stop|all (default single)\n"
        "  --out FILE          output WAV path (default render.wav)\n"
        "  --log FILE          operation log path (default <scenario>.log)\n"
        "  --frames N          generated input frames (default 48000)\n"
        "  --delay-ms MS       broadcast delay in milliseconds\n"
        "  --capacity-ms MS    bounded ring capacity in milliseconds\n"
        "  --fade-ms MS        seam crossfade in milliseconds\n"
        "  --gap-mode MODE     insert-silence|stop-output\n"
        "  --eoi-mode MODE     insert-silence|stop-output\n"
        "  --quiet             suppress console log output\n"
        "  --help              this help\n");
}

SimArgs parseArgs(int argc, char** argv) {
    SimArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (arg == "--help") { usage(); std::exit(0); }
        else if (arg == "--scenario") a.scenario = next("--scenario");
        else if (arg == "--out") a.outWav = next("--out");
        else if (arg == "--log") a.logPath = next("--log");
        else if (arg == "--frames") a.inputFrames = std::stoll(next("--frames"));
        else if (arg == "--delay-ms") a.delayMs = std::stoi(next("--delay-ms"));
        else if (arg == "--capacity-ms") a.capacityMs = std::stoi(next("--capacity-ms"));
        else if (arg == "--fade-ms") a.fadeMs = std::stoi(next("--fade-ms"));
        else if (arg == "--gap-mode") {
            a.gap = next("--gap-mode") == "stop-output" ? UnderrunPolicy::StopOutput
                                                        : UnderrunPolicy::InsertSilence;
        } else if (arg == "--eoi-mode") {
            a.eoi = next("--eoi-mode") == "stop-output" ? EndOfInputPolicy::StopOutput
                                                        : EndOfInputPolicy::InsertSilence;
        } else if (arg == "--quiet") a.quiet = true;
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (a.logPath.empty()) a.logPath = a.scenario + ".log";
    return a;
}

EngineConfig makeConfig(const SimArgs& a) {
    EngineConfig cfg;
    cfg.sampleRate = 48000;
    cfg.capacityFrames = static_cast<std::int64_t>(cfg.sampleRate) * a.capacityMs / 1000;
    cfg.delayFrames = static_cast<std::int64_t>(cfg.sampleRate) * a.delayMs / 1000;
    cfg.fadeFrames = static_cast<std::int64_t>(cfg.sampleRate) * a.fadeMs / 1000;
    cfg.underrun = a.gap;
    cfg.endOfInput = a.eoi;
    return cfg;
}

void logScript(const char* tag, const SubmitResult& r) {
    std::ostringstream m;
    m << tag << " -> " << toString(r.status) << " id=" << r.id;
    if (!r.detail.empty()) m << " " << r.detail;
    if (r.status == SubmitStatus::Accepted)
        DBS_LOG_INFO("script", m.str());
    else
        DBS_LOG_WARN("script", m.str());
}

int runScenario(const std::string& name, const SimArgs& args) {
    auto cfg = makeConfig(args);
    CodeTrackConfig track;
    track.sampleRate = cfg.sampleRate;
    ScriptRunner runner(cfg, PadLibrary::makeBuiltin(cfg.sampleRate), track);

    const std::int64_t N = args.inputFrames;
    const std::int64_t third = N / 3;

    if (name == "single") {
        runner.feed(third, 333);
        logScript("single-edit submit",
                  runner.edit(third / 2, third / 2 + 4800, "tone-1k"));
        runner.feed(N - third, 257);
        runner.endInput();
        runner.drainToCompletion(N);
    } else if (name == "wrap") {
        runner.feed(900, 127);
        auto e = runner.edit(500, 700, "tone-440");
        logScript("wrap edit", e);
        for (int round = 0; round < 12; ++round) {
            runner.feed(500, 97 + round * 11);
            runner.drain(400 + (round % 3) * 37);
        }
        runner.endInput();
        // Drain exactly what remains buffered; no extra frames are requested,
        // so the rendered output contains no policy-silence tail.
        runner.drainToCompletion(runner.core().writePos() -
                                 runner.core().outputPos());
    } else if (name == "underrun") {
        // Zero delay exposes the producer gap to the output directly; with a
        // non-zero delay the bounded window would simply absorb a short stall.
        auto c2 = cfg;
        c2.delayFrames = 0;
        runner = ScriptRunner(c2, PadLibrary::makeBuiltin(c2.sampleRate), track);
        runner.feed(third, 256);
        runner.drain(third);
        runner.drain(2400);  // producer gap: silence fill, never replay
        runner.feed(third, 129);
        runner.drain(third);
        runner.feed(N - 2 * third, 200);
        runner.endInput();
        // Request exactly the real input length: the rendered output then
        // contains the program plus the gap-silence, with no EOS padding.
        runner.drainToCompletion(N - 32000);
    } else if (name == "underrun-stop") {
        // Zero delay so the stream goes on air immediately; the stop policy
        // then demonstrates a real mid-stream interruption (not startup fill).
        // EOS also stops, so the render ends at the real input head with no
        // policy silence tail masking the freeze/resume behaviour.
        auto c2 = cfg;
        c2.underrun = UnderrunPolicy::StopOutput;
        c2.endOfInput = EndOfInputPolicy::StopOutput;
        c2.delayFrames = 0;
        runner = ScriptRunner(c2, PadLibrary::makeBuiltin(c2.sampleRate), track);
        runner.feed(third, 256);
        runner.drain(third);
        runner.drain(1000);  // freezes: no frames, no stale replay
        runner.feed(third, 200);
        runner.core().resume();
        runner.drain(third);
        runner.feed(N - 2 * third, 200);
        runner.endInput();
        runner.drainToCompletion(N);
    } else if (name == "overlap") {
        runner.feed(N, 500);
        auto a = runner.edit(third, third * 2, "tone-440");
        auto b = runner.edit(third + 2400, third * 2 + 2400, "tone-1k");
        auto c = runner.edit(third + 4800, third + 9600, "chime");
        logScript("overlap A", a);
        logScript("overlap B", b);
        logScript("overlap C", c);
        std::ostringstream m;
        m << "overlap trio submitted: " << a.id << "," << b.id << "," << c.id;
        DBS_LOG_INFO("script", m.str());
        runner.endInput();
        runner.drainToCompletion(N);
    } else if (name == "undo") {
        runner.feed(third, 400);
        auto e = runner.edit(third / 2, third / 2 + 6000, "tone-1k");
        logScript("undo target edit", e);
        runner.drain(100);
        logScript("quick undo", runner.undo(e.id));
        runner.feed(N - third, 257);
        runner.endInput();
        runner.drainToCompletion(N);
    } else if (name == "near-air") {
        runner.feed(third, 256);
        runner.drain(third - 240);
        auto late = runner.edit(third - 480, third + 480, "chime");
        logScript("partly-aired edit", late);
        auto tight = runner.edit(runner.core().outputPos(),
                                 runner.core().outputPos() + 120, "tone-320");
        logScript("at-output-head edit", tight);
        runner.feed(N - third, 257);
        runner.endInput();
        runner.drainToCompletion(N);
    } else if (name == "eoi-stop") {
        // A short delay keeps the full input beyond the delay window, so part
        // of the stream airs before end-of-input halts the output head exactly
        // at the write head (no silence tail, no replay).
        auto c2 = cfg;
        c2.endOfInput = EndOfInputPolicy::StopOutput;
        c2.delayFrames = cfg.sampleRate / 2;  // 500 ms
        runner = ScriptRunner(c2, PadLibrary::makeBuiltin(c2.sampleRate), track);
        runner.feed(N / 2, 256);
        runner.endInput();
        runner.drainToCompletion(N);
    } else {
        throw std::runtime_error("unknown scenario: " + name);
    }

    const auto snap = runner.core().snapshot();
    writeWavMono(args.outWav, cfg.sampleRate, runner.output().data(),
                 static_cast<std::int64_t>(runner.output().size()));

    std::ostringstream m;
    m << "render complete: samples=" << runner.output().size()
      << " outputPos=" << snap.outputPos
      << " silenceFilled=" << snap.silenceFilled
      << " underrunEvents=" << snap.underrunEvents
      << " backPressure=" << snap.backPressureFrames
      << " wav=" << args.outWav;
    DBS_LOG_INFO("io", m.str());
    std::printf("scenario=%s outputFrames=%lld wav=%s log=%s\n",
                name.c_str(), (long long)runner.output().size(),
                args.outWav.c_str(), args.logPath.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SimArgs args;
    try {
        args = parseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "argument error: %s\n", ex.what());
        usage();
        return 2;
    }

    Logger::instance().setConsoleEnabled(!args.quiet);
    Logger::instance().openFile(args.logPath);
    DBS_LOG_INFO("sim", "all actions operate solely on the local demo audio stream");

    const std::vector<std::string> all = {
        "single", "wrap", "underrun", "underrun-stop",
        "overlap", "undo", "near-air", "eoi-stop"};
    try {
        if (args.scenario == "all") {
            int rc = 0;
            for (const auto& s : all) {
                SimArgs one = args;
                one.scenario = s;
                one.logPath = s + ".log";
                one.outWav = s + ".wav";
                Logger::instance().openFile(one.logPath);
                rc |= runScenario(s, one);
            }
            return rc;
        }
        return runScenario(args.scenario, args);
    } catch (const std::exception& ex) {
        DBS_LOG_ERROR("sim", std::string("fatal: ") + ex.what());
        return 1;
    }
}
