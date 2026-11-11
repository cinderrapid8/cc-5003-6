#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dbs {

// Real-time mono playback through ALSA. All actions stay on the local demo
// audio stream; nothing is transmitted anywhere.
class AlsaSink {
public:
    AlsaSink(std::string device, int sampleRate, std::int64_t periodFrames);
    ~AlsaSink();

    void write(const float* frames, std::int64_t count);
    void drain();
    void close();

    static bool available();

private:
    struct Impl;
    Impl* impl_;
};

// Collects every rendered block into a WAV file (offline audible result).
class WavRecorder {
public:
    WavRecorder(std::string path, int sampleRate);
    void write(const float* frames, std::int64_t count);
    void finish();
    std::int64_t frames() const { return written_; }

private:
    std::string path_;
    int sampleRate_;
    std::int64_t written_ = 0;
    std::vector<float> buffer_;
};

}  // namespace dbs
