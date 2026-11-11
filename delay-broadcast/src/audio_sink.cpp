#include "audio_sink.h"
#include "wav_file.h"
#include "logger.h"

#ifdef DBS_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#include <atomic>
#include <sstream>

namespace dbs {

#ifdef DBS_HAVE_ALSA

struct AlsaSink::Impl {
    snd_pcm_t* pcm = nullptr;
    std::string device;
    int sampleRate = 0;
    std::int64_t period = 0;
    std::vector<std::int16_t> interleave;
};

bool AlsaSink::available() {
#ifdef DBS_HAVE_ALSA
    return true;
#else
    return false;
#endif
}

AlsaSink::AlsaSink(std::string device, int sampleRate, std::int64_t periodFrames)
    : impl_(new Impl) {
    impl_->device = std::move(device);
    impl_->sampleRate = sampleRate;
    impl_->period = periodFrames;

    int rc = snd_pcm_open(&impl_->pcm, impl_->device.c_str(),
                          SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error("ALSA open failed for " + device + ": " +
                                 snd_strerror(rc));
    }

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(impl_->pcm, params);
    snd_pcm_hw_params_set_access(impl_->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(impl_->pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(impl_->pcm, params, 1);
    unsigned rate = static_cast<unsigned>(sampleRate);
    snd_pcm_hw_params_set_rate_near(impl_->pcm, params, &rate, nullptr);
    snd_pcm_uframes_t periodSize = static_cast<snd_pcm_uframes_t>(periodFrames);
    snd_pcm_hw_params_set_period_size_near(impl_->pcm, params, &periodSize, nullptr);
    snd_pcm_uframes_t bufferSize = periodSize * 4;
    snd_pcm_hw_params_set_buffer_size_near(impl_->pcm, params, &bufferSize);

    rc = snd_pcm_hw_params(impl_->pcm, params);
    if (rc < 0) {
        snd_pcm_close(impl_->pcm);
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error(std::string("ALSA hw_params failed: ") +
                                 snd_strerror(rc));
    }
    snd_pcm_prepare(impl_->pcm);

    std::ostringstream m;
    m << "ALSA sink opened device=" << impl_->device << " rate=" << rate
      << " period=" << periodSize << " buffer=" << bufferSize;
    DBS_LOG_INFO("audio", m.str());
}

AlsaSink::~AlsaSink() {
    close();
    delete impl_;
}

void AlsaSink::write(const float* frames, std::int64_t count) {
    if (!impl_ || !impl_->pcm) return;
    impl_->interleave.resize(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        float v = frames[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        impl_->interleave[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(v * 32767.0f);
    }

    std::int64_t off = 0;
    while (off < count) {
        snd_pcm_sframes_t rc = snd_pcm_writei(
            impl_->pcm, impl_->interleave.data() + off,
            static_cast<snd_pcm_uframes_t>(count - off));
        if (rc == -EPIPE) {
            DBS_LOG_WARN("audio", "ALSA underrun, recovering");
            snd_pcm_prepare(impl_->pcm);
            continue;
        }
        if (rc < 0) {
            DBS_LOG_ERROR("audio", std::string("ALSA write failed: ") +
                                       snd_strerror(static_cast<int>(rc)));
            break;
        }
        off += rc;
    }
}

void AlsaSink::drain() {
    if (impl_ && impl_->pcm) snd_pcm_drain(impl_->pcm);
}

void AlsaSink::close() {
    if (impl_ && impl_->pcm) {
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }
}

#else  // !DBS_HAVE_ALSA

struct AlsaSink::Impl {};

bool AlsaSink::available() { return false; }

AlsaSink::AlsaSink(std::string device, int, std::int64_t)
    : impl_(new Impl) {
    throw std::runtime_error("ALSA support not compiled in (device " + device +
                             "); use the rendered WAV output instead");
}

AlsaSink::~AlsaSink() { delete impl_; }
void AlsaSink::write(const float*, std::int64_t) {}
void AlsaSink::drain() {}
void AlsaSink::close() {}

#endif  // DBS_HAVE_ALSA

WavRecorder::WavRecorder(std::string path, int sampleRate)
    : path_(std::move(path)), sampleRate_(sampleRate) {}

void WavRecorder::write(const float* frames, std::int64_t count) {
    buffer_.insert(buffer_.end(), frames, frames + count);
    written_ += count;
}

void WavRecorder::finish() {
    writeWavMono(path_, sampleRate_, buffer_.data(),
                 static_cast<std::int64_t>(buffer_.size()));
    std::ostringstream m;
    m << "wav recording finalized: " << path_ << " frames=" << written_;
    DBS_LOG_INFO("audio", m.str());
}

}  // namespace dbs
