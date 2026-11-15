// ALSA back end (Linux / Cardputer Zero).
#include <cstdio>

#include <alsa/asoundlib.h>

#include "audio/audio_sink.h"

namespace osx {

namespace {
// 48 kHz, stereo S16_LE, 1024-frame periods, 4 periods of buffer.
int openPcm(const std::string& dev, snd_pcm_t** out, std::string& why) {
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) { why = dev + ": " + snd_strerror(err); return err; }
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    unsigned rate = AudioSink::kRate;
    snd_pcm_uframes_t period = AudioSink::kPeriod, buffer = AudioSink::kPeriod * 4;
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr)) < 0 ||
        (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
        (err = snd_pcm_hw_params(pcm, hw)) < 0) {
        why = dev + ": " + snd_strerror(err);
        snd_pcm_close(pcm);
        return err;
    }
    if (rate != AudioSink::kRate) {
        why = dev + ": no 48 kHz (got " + std::to_string(rate) + ")";
        snd_pcm_close(pcm);
        return -EINVAL;
    }
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer - period);
    snd_pcm_sw_params(pcm, sw);
    char d[128];
    std::snprintf(d, sizeof(d), "%s (period %lu, buffer %lu)", dev.c_str(), (unsigned long)period, (unsigned long)buffer);
    why = d;
    *out = pcm;
    return 0;
}
}  // namespace

const char* AudioSink::backend() const { return "ALSA"; }

bool AudioSink::start(const std::string& device) {
    if (running_.exchange(true)) return deviceOpen_.load();
    snd_pcm_t* pcm = nullptr;
    std::string why, firstWhy;
    if (openPcm(device, &pcm, why) < 0) {
        firstWhy = why;
        if (device != "default" && openPcm("default", &pcm, why) < 0) pcm = nullptr;
    }
    status_ = pcm ? why : "no audio device (" + firstWhy + (device != "default" ? "; default: " + why : "") + ")";
    if (!pcm) std::fprintf(stderr, "[audio] %s\n", status_.c_str());
    handle_ = pcm;
    deviceOpen_.store(pcm != nullptr);
    buffering_ = true;
    thread_ = std::thread(pcm ? &AudioSink::runAlsa : &AudioSink::runTimer, this);
    return pcm != nullptr;
}

void AudioSink::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (handle_) {
        snd_pcm_drop(static_cast<snd_pcm_t*>(handle_));
        snd_pcm_close(static_cast<snd_pcm_t*>(handle_));
        handle_ = nullptr;
    }
    deviceOpen_.store(false);
    status_ = "stopped";
}

void AudioSink::runAlsa() {
    auto* pcm = static_cast<snd_pcm_t*>(handle_);
    int16_t buf[kPeriod * 2];
    while (running_.load(std::memory_order_relaxed)) {
        render(buf, kPeriod);
        const int16_t* p = buf;
        snd_pcm_uframes_t left = kPeriod;
        while (left > 0 && running_.load(std::memory_order_relaxed)) {
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, left);
            if (w < 0) {
                if (w == -EPIPE) underruns_.fetch_add(1, std::memory_order_relaxed);
                w = snd_pcm_recover(pcm, int(w), 1);  // XRUN / suspend
                if (w < 0) {
                    std::fprintf(stderr, "[audio] unrecoverable: %s\n", snd_strerror(int(w)));
                    break;
                }
                continue;
            }
            p += w * 2;
            left -= snd_pcm_uframes_t(w);
        }
    }
}

std::string AudioSink::clampMixer(const std::string& card, float maxDb) {
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
            const long target = long(maxDb * 100.0f);
            char line[160];
            long before = 0, after = 0;
            const bool hasDb = snd_mixer_selem_get_playback_dB(chosen, SND_MIXER_SCHN_FRONT_LEFT, &before) >= 0;
            if (!hasDb) {
                std::snprintf(line, sizeof(line), "mixer: '%s' has no dB scale, left unchanged", snd_mixer_selem_get_name(chosen));
            } else {
                snd_mixer_selem_set_playback_dB_all(chosen, target, -1 /* round down: never above -6 dB */);
                if (snd_mixer_selem_has_playback_switch(chosen)) snd_mixer_selem_set_playback_switch_all(chosen, 1);
                snd_mixer_selem_get_playback_dB(chosen, SND_MIXER_SCHN_FRONT_LEFT, &after);
                std::snprintf(line, sizeof(line), "mixer: '%s' locked %.1f -> %.1f dB", snd_mixer_selem_get_name(chosen),
                              double(before) / 100.0, double(after) / 100.0);
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
