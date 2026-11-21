// Adaptive-rate SDL2 reference audio output for Linux ARM64 handhelds.
// Reuses platform/vita/audio_rate.h to absorb 57.52 Hz <-> 60 Hz pacing smoothly
// without crackles, and skips silent FM blocks (saving ~1.6 ms/frame on the sound core).
#pragma once

#include "runtime/sound_board.h"
#include "audio_rate.h"

#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace arm64 {

class Audio {
public:
    struct Stats {
        unsigned underruns = 0;
        double ratio = 1.0;
        double queued_ms = 0.0;
    };
    Audio() = default;
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    ~Audio() { close(); }

    bool open() {
        SDL_AudioSpec want{}, got{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = callback;
        want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (!device_) return false;
        rate_ = got.freq;
        fm_rate_ = int(snd::SoundBoard::kYmClock / 144.0 + 0.5);
        fm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, fm_rate_, AUDIO_F32SYS, 2, rate_);
        pcm_ = SDL_NewAudioStream(AUDIO_F32SYS, 2, int(snd::SoundBoard::kPcmClock / 224.0 + 0.5),
                                  AUDIO_F32SYS, 2, rate_);
        if (!fm_ || !pcm_) { close(); return false; }
        restart_locked();
        return true;
    }

    void push(snd::SoundBoard &board) {
        const auto fm = board.take_fm(), pcm = board.take_pcm();
        if (!device_) return;
        SDL_LockAudioDevice(device_);
        const int limit = rate_ * 2 * int(sizeof(float)) / 4; // cap at 250 ms
        if (SDL_AudioStreamAvailable(fm_) > limit || SDL_AudioStreamAvailable(pcm_) > limit) {
            SDL_AudioStreamClear(fm_);
            SDL_AudioStreamClear(pcm_);
            fm_active_ = false;
        }
        bool fm_ok = true;
        if (std::any_of(fm.begin(), fm.end(), [](float v) { return v != 0.0f; })) {
            if (!fm_active_) {
                const int behind = (SDL_AudioStreamAvailable(pcm_) - SDL_AudioStreamAvailable(fm_)) / kFrameBytes;
                if (behind > 0) {
                    const std::vector<float> silence(size_t(int64_t(behind) * fm_rate_ / rate_) * 2u, 0.0f);
                    if (!silence.empty())
                        fm_ok = SDL_AudioStreamPut(fm_, silence.data(), int(silence.size() * sizeof(float))) >= 0;
                }
                fm_active_ = true;
            }
            fm_ok = fm_ok && SDL_AudioStreamPut(fm_, fm.data(), int(fm.size() * sizeof(float))) >= 0;
        } else {
            fm_active_ = false;
        }
        if (!fm_ok || SDL_AudioStreamPut(pcm_, pcm.data(), int(pcm.size() * sizeof(float))) < 0)
            std::fprintf(stderr, "audio queue: %s\n", SDL_GetError());
        const int prime = vita::AudioRate::kPrimeFrames * kFrameBytes;
        const bool ready = SDL_AudioStreamAvailable(pcm_) >= prime;
        SDL_UnlockAudioDevice(device_);
        if (!playing_ && ready) {
            SDL_PauseAudioDevice(device_, 0);
            playing_ = true;
        }
    }

    void pause() {
        if (!device_) return;
        SDL_PauseAudioDevice(device_, 1);
        SDL_LockAudioDevice(device_);
        SDL_AudioStreamClear(fm_);
        SDL_AudioStreamClear(pcm_);
        fm_active_ = false;
        restart_locked();
        SDL_UnlockAudioDevice(device_);
        playing_ = false;
    }

    Stats stats() {
        Stats out;
        if (!device_) return out;
        SDL_LockAudioDevice(device_);
        out.underruns = underruns_;
        out.ratio = rate_control_.ratio();
        out.queued_ms = rate_control_.average() * 1000.0 / double(rate_);
        SDL_UnlockAudioDevice(device_);
        return out;
    }

    void volume(float value) {
        if (device_) SDL_LockAudioDevice(device_);
        volume_ = std::clamp(value, 0.0f, 1.0f);
        if (device_) SDL_UnlockAudioDevice(device_);
    }

    void mute(bool muted) {
        if (device_) SDL_LockAudioDevice(device_);
        muted_ = muted;
        if (device_) SDL_UnlockAudioDevice(device_);
    }

    void close() {
        if (device_) SDL_CloseAudioDevice(device_);
        device_ = 0;
        if (fm_) SDL_FreeAudioStream(fm_);
        if (pcm_) SDL_FreeAudioStream(pcm_);
        fm_ = pcm_ = nullptr;
        playing_ = false;
    }

private:
    static constexpr int kFrameBytes = 2 * int(sizeof(float));
    static constexpr int kChunk = 512;
    static constexpr int kMaxInput = kChunk * 2;

    void restart_locked() {
        buffering_ = true;
        resampler_.reset();
    }
    int queued_frames_locked() const { return SDL_AudioStreamAvailable(pcm_) / kFrameBytes; }

    static void callback(void *userdata, Uint8 *buffer, int bytes) {
        auto &self = *static_cast<Audio *>(userdata);
        std::memset(buffer, 0, size_t(bytes));
        auto *out = reinterpret_cast<int16_t *>(buffer);
        int frames = bytes / (2 * int(sizeof(int16_t)));
        int queued = self.queued_frames_locked();
        if (self.buffering_) {
            if (queued < vita::AudioRate::kPrimeFrames) return;
            self.buffering_ = false;
            self.rate_control_.reset(queued);
        }
        const double ratio = self.rate_control_.update(queued);
        while (frames > 0) {
            const int n = std::min(frames, kChunk);
            const int need = self.resampler_.needed(n, ratio);
            if (need > queued) {
                ++self.underruns_;
                self.restart_locked();
                return;
            }
            float *fm = self.fm_in_, *pcm = self.pcm_in_, *mixed = self.mixed_;
            const int got = std::min(need, kMaxInput);
            if (got > 0) {
                SDL_AudioStreamGet(self.pcm_, pcm, got * kFrameBytes);
                const int fm_frames = std::min(got, SDL_AudioStreamAvailable(self.fm_) / kFrameBytes);
                if (fm_frames > 0) {
                    SDL_AudioStreamGet(self.fm_, fm, fm_frames * kFrameBytes);
                    for (int i = 0; i < fm_frames * 2; ++i) pcm[i] += fm[i];
                }
            }
            queued -= got;
            self.resampler_.run(pcm, got, mixed, n, ratio);
            for (int i = 0; i < n * 2; ++i) {
                float sample = self.muted_ ? 0.f : mixed[i] * self.volume_;
                if (!std::isfinite(sample)) sample = 0.f;
                out[i] = int16_t(std::clamp(sample, -1.f, 1.f) * 32767.f);
            }
            frames -= n;
            out += n * 2;
        }
    }

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int rate_ = 48000, fm_rate_ = 55556;
    bool fm_active_ = false;
    bool playing_ = false, muted_ = false;
    float volume_ = 0.8f;
    vita::AudioRate rate_control_;
    vita::StereoResampler resampler_;
    bool buffering_ = true;
    unsigned underruns_ = 0;
    float fm_in_[kMaxInput * 2] = {}, pcm_in_[kMaxInput * 2] = {}, mixed_[kChunk * 2] = {};
};

} // namespace arm64
