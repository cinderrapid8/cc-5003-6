#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace dbs {

struct WavData {
    int sampleRate = 0;
    std::int64_t frames = 0;
    std::vector<float> mono;  // normalized to [-1, 1]
};

WavData loadWavMono(const std::string& path);
void writeWavMono(const std::string& path, int sampleRate, const float* data,
                  std::int64_t frames);
void writeWavStereo(const std::string& path, int sampleRate,
                    const float* left, const float* right, std::int64_t frames);

class WavError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace dbs
