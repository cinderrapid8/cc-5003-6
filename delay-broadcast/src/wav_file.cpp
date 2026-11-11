#include "wav_file.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

namespace dbs {

namespace {

template <typename T>
T readLe(const std::uint8_t* p) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(p[i]) << (8 * i);
    }
    return value;
}

template <typename T>
void writeLe(std::FILE* f, T value) {
    std::uint8_t buf[sizeof(T)];
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        buf[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    std::fwrite(buf, 1, sizeof(T), f);
}

float pcmToFloat(std::int32_t sample, int bits) {
    if (bits <= 8) {
        return (static_cast<int>(sample) - 128) / 128.0f;
    }
    const float scale = static_cast<float>(1 << (bits - 1));
    return static_cast<float>(sample) / scale;
}

std::int16_t floatToPcm16(float v) {
    v = std::fmax(-1.0f, std::fmin(1.0f, v));
    return static_cast<std::int16_t>(std::lround(v * 32767.0f));
}

}  // namespace

WavData loadWavMono(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw WavError("cannot open wav: " + path);

    char riff[12];
    if (std::fread(riff, 1, 12, f) != 12 ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        throw WavError("not a RIFF/WAVE file: " + path);
    }

    std::uint16_t channels = 0, bitsPerSample = 0, audioFormat = 0;
    std::uint32_t sampleRate = 0;
    std::vector<std::uint8_t> data;

    while (true) {
        char chunkId[4];
        std::uint32_t chunkSize = 0;
        if (std::fread(chunkId, 1, 4, f) != 4) break;
        if (std::fread(&chunkSize, 4, 1, f) != 1) break;
        const auto begin = std::ftell(f);

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            std::uint8_t fmt[16];
            if (std::fread(fmt, 1, 16, f) != 16) {
                std::fclose(f);
                throw WavError("truncated fmt chunk: " + path);
            }
            audioFormat = readLe<std::uint16_t>(fmt + 0);
            channels = readLe<std::uint16_t>(fmt + 2);
            sampleRate = readLe<std::uint32_t>(fmt + 4);
            bitsPerSample = readLe<std::uint16_t>(fmt + 14);
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            data.resize(chunkSize);
            if (chunkSize > 0 && std::fread(data.data(), 1, chunkSize, f) != chunkSize) {
                std::fclose(f);
                throw WavError("truncated data chunk: " + path);
            }
        }
        std::fseek(f, begin + static_cast<long>(chunkSize + (chunkSize & 1u)), SEEK_SET);
    }
    std::fclose(f);

    if (audioFormat != 1 || channels == 0 || bitsPerSample == 0 || data.empty()) {
        throw WavError("only uncompressed PCM wav supported: " + path);
    }
    if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) {
        throw WavError("unsupported bit depth: " + path);
    }

    const int bytesPerSample = bitsPerSample / 8;
    const std::int64_t totalFrames =
        static_cast<std::int64_t>(data.size()) / (channels * bytesPerSample);

    WavData out;
    out.sampleRate = static_cast<int>(sampleRate);
    out.frames = totalFrames;
    out.mono.resize(static_cast<std::size_t>(totalFrames));

    for (std::int64_t frame = 0; frame < totalFrames; ++frame) {
        double acc = 0.0;
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            const std::size_t off =
                static_cast<std::size_t>((frame * channels + ch) * bytesPerSample);
            std::int32_t sample = 0;
            if (bitsPerSample == 8) {
                sample = data[off];
            } else if (bitsPerSample == 16) {
                sample = readLe<std::int16_t>(data.data() + off);
            } else if (bitsPerSample == 24) {
                sample = static_cast<std::int32_t>(data[off]) |
                         (static_cast<std::int32_t>(data[off + 1]) << 8) |
                         (static_cast<std::int32_t>(data[off + 2]) << 16);
                if (sample & 0x800000) sample |= ~0xFFFFFF;
            } else {
                sample = static_cast<std::int32_t>(readLe<std::uint32_t>(data.data() + off));
            }
            acc += pcmToFloat(sample, bitsPerSample);
        }
        out.mono[static_cast<std::size_t>(frame)] =
            static_cast<float>(acc / channels);
    }
    return out;
}

namespace {

void writeHeader(std::FILE* f, std::uint16_t channels, std::uint32_t sampleRate,
                 std::uint16_t bits, std::uint32_t dataBytes) {
    const std::uint32_t blockAlign = channels * (bits / 8);
    const std::uint32_t byteRate = sampleRate * blockAlign;
    std::fwrite("RIFF", 1, 4, f);
    writeLe<std::uint32_t>(f, 36u + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    writeLe<std::uint32_t>(f, 16u);
    writeLe<std::uint16_t>(f, 1u);
    writeLe<std::uint16_t>(f, channels);
    writeLe<std::uint32_t>(f, sampleRate);
    writeLe<std::uint32_t>(f, byteRate);
    writeLe<std::uint16_t>(f, static_cast<std::uint16_t>(blockAlign));
    writeLe<std::uint16_t>(f, bits);
    std::fwrite("data", 1, 4, f);
    writeLe<std::uint32_t>(f, dataBytes);
}

}  // namespace

void writeWavMono(const std::string& path, int sampleRate, const float* data,
                  std::int64_t frames) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw WavError("cannot create wav: " + path);
    const auto dataBytes = static_cast<std::uint32_t>(frames * 2);
    writeHeader(f, 1, static_cast<std::uint32_t>(sampleRate), 16, dataBytes);
    for (std::int64_t i = 0; i < frames; ++i) {
        writeLe<std::int16_t>(f, floatToPcm16(data[i]));
    }
    std::fclose(f);
}

void writeWavStereo(const std::string& path, int sampleRate,
                    const float* left, const float* right, std::int64_t frames) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw WavError("cannot create wav: " + path);
    const auto dataBytes = static_cast<std::uint32_t>(frames * 4);
    writeHeader(f, 2, static_cast<std::uint32_t>(sampleRate), 16, dataBytes);
    for (std::int64_t i = 0; i < frames; ++i) {
        writeLe<std::int16_t>(f, floatToPcm16(left[i]));
        writeLe<std::int16_t>(f, floatToPcm16(right[i]));
    }
    std::fclose(f);
}

}  // namespace dbs
