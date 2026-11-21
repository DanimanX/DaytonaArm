// Dedicated worker thread for the reference Sega Model 1 sound board
// (recompiled M68000 + 2x MultiPCM + YM3438), pinned to Core 2 on Allwinner H700.
#pragma once

#include "thread_affinity.h"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>

namespace arm64 {

class SoundWorker {
public:
    SoundWorker() = default;
    SoundWorker(const SoundWorker &) = delete;
    SoundWorker &operator=(const SoundWorker &) = delete;
    ~SoundWorker() { close(); }

    void set_core(int core) { requested_core_ = core; }
    bool threaded() const { return thread_.joinable(); }
    int affinity_result() const { return affinity_result_; }
    uint64_t last_sound_ticks() const { return last_sound_ticks_; }
    uint64_t last_audio_ticks() const { return last_audio_ticks_; }

    bool open() {
        close();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stopping_ = false;
            initialized_ = false;
            queued_ = false;
            done_ = true;
            busy_ = false;
        }
        try {
            thread_ = std::thread(&SoundWorker::entry, this);
        } catch (...) {
            return false;
        }
        std::unique_lock<std::mutex> lk(mutex_);
        cv_done_.wait(lk, [&] { return initialized_; });
        return true;
    }

    template <class Packet, class Output>
    void dispatch_packet(Packet &packet, Output &audio, uint64_t (*clock)()) {
        dispatch(&packet,
                 [](void *p) { return static_cast<Packet *>(p)->execute(); },
                 &audio,
                 [](void *p, void *output) {
                     if (auto *sound = static_cast<Packet *>(p)->sound())
                         static_cast<Output *>(output)->push(*sound);
                 },
                 clock);
    }

    void finish() {
        if (!thread_.joinable()) return;
        std::exception_ptr err;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (!busy_) return;
            cv_done_.wait(lk, [&] { return done_; });
            last_sound_ticks_ = ticks_;
            last_audio_ticks_ = queue_ticks_;
            err = error_;
            context_ = nullptr;
            error_ = nullptr;
            busy_ = false;
        }
        if (err) std::rethrow_exception(err);
    }

    void close() noexcept {
        try { finish(); } catch (...) {}
        if (thread_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                stopping_ = true;
            }
            cv_ready_.notify_one();
            thread_.join();
        }
    }

private:
    using Execute = uint64_t (*)(void *);
    using Queue = void (*)(void *, void *);
    using Clock = uint64_t (*)();
    struct Result { uint64_t sound = 0, audio = 0; std::exception_ptr error; };

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_ready_, cv_done_;
    void *context_ = nullptr;
    Execute execute_ = nullptr;
    void *queue_context_ = nullptr;
    Queue queue_ = nullptr;
    Clock queue_clock_ = nullptr;
    uint64_t ticks_ = 0, queue_ticks_ = 0, last_sound_ticks_ = 0, last_audio_ticks_ = 0;
    std::exception_ptr error_;
    bool queued_ = false, busy_ = false, done_ = true, stopping_ = false, initialized_ = false;
    int requested_core_ = 2;
    int affinity_result_ = -1;

    static Result run(void *context, Execute execute, void *output, Queue queue, Clock clock) {
        Result result;
        try {
            result.sound = execute(context);
            if (queue) {
                const uint64_t begin = clock ? clock() : 0;
                queue(context, output);
                const uint64_t end = clock ? clock() : 0;
                result.audio = end >= begin ? end - begin : 0;
            }
        } catch (...) {
            result.error = std::current_exception();
        }
        return result;
    }

    void dispatch(void *context, Execute execute, void *output, Queue queue, Clock clock) {
        finish();
        if (!thread_.joinable()) {
            const Result result = run(context, execute, output, queue, clock);
            last_sound_ticks_ = result.sound;
            last_audio_ticks_ = result.audio;
            if (result.error) std::rethrow_exception(result.error);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            context_ = context;
            execute_ = execute;
            queue_context_ = output;
            queue_ = queue;
            queue_clock_ = clock;
            done_ = false;
            queued_ = busy_ = true;
        }
        cv_ready_.notify_one();
    }

    void entry() {
        set_current_thread_name("daytona_sound");
        affinity_result_ = pin_current_thread(requested_core_);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            initialized_ = true;
        }
        cv_done_.notify_one();

        for (;;) {
            void *context = nullptr;
            Execute execute = nullptr;
            void *output = nullptr;
            Queue queue = nullptr;
            Clock clock = nullptr;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_ready_.wait(lk, [&] { return queued_ || stopping_; });
                if (stopping_) break;
                context = context_;
                execute = execute_;
                output = queue_context_;
                queue = queue_;
                clock = queue_clock_;
                queued_ = false;
            }
            const Result result = run(context, execute, output, queue, clock);
            {
                std::lock_guard<std::mutex> lk(mutex_);
                ticks_ = result.sound;
                queue_ticks_ = result.audio;
                error_ = result.error;
                done_ = true;
            }
            cv_done_.notify_one();
        }
    }
};

} // namespace arm64
