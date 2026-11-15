#include "audio/alsa_sink.h"

#include <chrono>
#include <cstdio>
#include <vector>

#include <alsa/asoundlib.h>

#include "audio/limiter.h"
#include "dsp/receiver.h"

namespace osx {

AlsaSink::AlsaSink(RingBuffer<float>& ring) : ring_(ring) {}
AlsaSink::~AlsaSink() { stop(); }

bool AlsaSink::start(const std::string& device) {
    if (running_.exchange(true)) return deviceOpen_.load();
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err >= 0) {
        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                 unsigned(Receiver::kAudioRate), 1 /* allow resampling */, 60000 /* us */);
        if (err < 0) {
            snd_pcm_close(pcm);
            pcm = nullptr;
        }
    }
    if (err < 0) {
        status_ = "no audio device (" + device + ": " + snd_strerror(err) + ")";
        std::fprintf(stderr, "[audio] %s\n", status_.c_str());
    } else {
        status_ = device;
    }
    pcm_ = pcm;
    deviceOpen_.store(pcm != nullptr);
    thread_ = std::thread(&AlsaSink::run, this);
    return pcm != nullptr;
}

void AlsaSink::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (pcm_) {
        snd_pcm_drop(static_cast<snd_pcm_t*>(pcm_));
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_));
        pcm_ = nullptr;
    }
    deviceOpen_.store(false);
    status_ = "stopped";
}

void AlsaSink::run() {
    constexpr size_t kPeriod = 480;  // 10 ms
    auto* pcm = static_cast<snd_pcm_t*>(pcm_);
    std::vector<float> buf(kPeriod * 2);
    std::vector<int16_t> pcm16(kPeriod * 2);
    OutputLimiter limiter(Receiver::kAudioRate);
    bool buffering = true;  // wait for the ring to reach its target fill before playing
    auto next = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        const size_t availFrames = ring_.readable() / 2;
        size_t got = 0;
        if (buffering && availFrames >= Receiver::kAudioTargetFrames) buffering = false;
        if (!buffering) {
            got = ring_.read(buf.data(), std::min(availFrames, kPeriod) * 2) / 2;
            if (got < kPeriod) {
                if (got > 0 || availFrames > 0) underruns_.fetch_add(1, std::memory_order_relaxed);
                buffering = true;
            }
        }
        std::fill(buf.begin() + std::ptrdiff_t(got * 2), buf.end(), 0.0f);
        limiter.process(buf.data(), kPeriod, volume.load(std::memory_order_relaxed), mute.load(std::memory_order_relaxed));
        peak_.store(limiter.peak, std::memory_order_relaxed);

        if (pcm) {
            for (size_t i = 0; i < buf.size(); ++i) pcm16[i] = int16_t(std::lrintf(buf[i] * 32767.0f));
            const int16_t* p = pcm16.data();
            size_t left = kPeriod;
            while (left > 0 && running_.load(std::memory_order_relaxed)) {
                snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, left);
                if (w < 0) {
                    w = snd_pcm_recover(pcm, int(w), 1);  // xrun / suspend
                    if (w < 0) break;
                    continue;
                }
                p += w * 2;
                left -= size_t(w);
            }
        } else {
            next += std::chrono::milliseconds(10);
            const auto now = std::chrono::steady_clock::now();
            if (now - next > std::chrono::milliseconds(200)) next = now;
            std::this_thread::sleep_until(next);
        }
    }
}

std::string AlsaSink::clampMixer(const std::string& card, float maxDb) {
    snd_mixer_t* mixer = nullptr;
    if (snd_mixer_open(&mixer, 0) < 0) return "mixer: open failed";
    std::string report = "mixer: no playback volume control found on " + card;
    if (snd_mixer_attach(mixer, card.c_str()) >= 0 && snd_mixer_selem_register(mixer, nullptr, nullptr) >= 0 &&
        snd_mixer_load(mixer) >= 0) {
        // First match wins, so attenuation is not stacked across several stages.
        static const char* priority[] = {"DAC", "Master", "PCM", "Headphone", "Speaker", "Playback"};
        snd_mixer_elem_t* chosen = nullptr;
        for (const char* want : priority) {
            for (snd_mixer_elem_t* e = snd_mixer_first_elem(mixer); e && !chosen; e = snd_mixer_elem_next(e)) {
                if (!snd_mixer_selem_is_active(e) || !snd_mixer_selem_has_playback_volume(e)) continue;
                if (std::string(snd_mixer_selem_get_name(e)).find(want) != std::string::npos) chosen = e;
            }
            if (chosen) break;
        }
        if (chosen) {
            long cur = 0;
            const long limit = long(maxDb * 100.0f);
            char line[160];
            if (snd_mixer_selem_get_playback_dB(chosen, SND_MIXER_SCHN_FRONT_LEFT, &cur) < 0) {
                std::snprintf(line, sizeof(line), "mixer: '%s' has no dB scale, left unchanged", snd_mixer_selem_get_name(chosen));
            } else if (cur > limit) {
                snd_mixer_selem_set_playback_dB_all(chosen, limit, -1 /* round down */);
                std::snprintf(line, sizeof(line), "mixer: '%s' clamped %.1f -> %.1f dB", snd_mixer_selem_get_name(chosen),
                              double(cur) / 100.0, double(limit) / 100.0);
            } else {
                std::snprintf(line, sizeof(line), "mixer: '%s' already at %.1f dB", snd_mixer_selem_get_name(chosen), double(cur) / 100.0);
            }
            report = line;
        }
    } else {
        report = "mixer: cannot attach to " + card;
    }
    snd_mixer_close(mixer);
    return report;
}

}  // namespace osx
