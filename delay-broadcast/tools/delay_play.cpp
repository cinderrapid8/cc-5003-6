#include "../src/audio_sink.h"
#include "../src/logger.h"
#include "../src/wav_file.h"

#include <cstdio>
#include <string>

using namespace dbs;

namespace {

void usage() {
    std::printf(
        "delay-play — local WAV playback tool for delay-broadcast renders\n"
        "Usage: delay-play [--device default] [--rate 48000] file.wav\n"
        "Plays through ALSA when a sound card is available. Everything stays\n"
        "local: this tool only reads the given file and opens the local PCM\n"
        "device; no network or broadcast transmission is involved.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string device = "default";
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") { usage(); return 0; }
        if (a == "--device" && i + 1 < argc) { device = argv[++i]; continue; }
        if (!a.empty() && a[0] != '-') { path = a; continue; }
        std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
        return 2;
    }
    if (path.empty()) { usage(); return 2; }

    try {
        WavData wav = loadWavMono(path);
        std::printf("playing %s: %lld frames, %d Hz, %.2f s\n",
                    path.c_str(), (long long)wav.frames, wav.sampleRate,
                    static_cast<double>(wav.frames) / wav.sampleRate);

        AlsaSink sink(device, wav.sampleRate, wav.sampleRate / 50);
        const std::int64_t chunk = 1024;
        for (std::int64_t off = 0; off < wav.frames; off += chunk) {
            const std::int64_t n = std::min(chunk, wav.frames - off);
            sink.write(wav.mono.data() + off, n);
        }
        sink.drain();
        std::printf("playback complete\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
            "playback unavailable: %s\n"
            "The render WAV itself is valid; on a machine with an ALSA sound\n"
            "card run: delay-play %s\n"
            "Alternatively inspect it with any WAV-capable local player.\n",
            ex.what(), path.c_str());
        return 1;
    }
}
