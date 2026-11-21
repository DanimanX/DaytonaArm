// ARM64 Linux replacement of src/runtime/geo.h: runs the unmodified MAME
// Model 2 geometrizer on its own dedicated CPU core (Core 1 on Allwinner H700).
//
// Build places platform/arm64/src before src on the include path so every
// "runtime/geo.h" resolves here while src/runtime/ remains unmodified.
#ifndef DAYTONA_ARM64_GEO_CORE_INCLUDED
#define DAYTONA_ARM64_GEO_CORE_INCLUDED

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#define Geo GeoCore
#include "../../../../src/runtime/geo.h"
#ifndef DAYTONA_ARM64_GEO_CORE_TU
#undef Geo
#endif

#endif // DAYTONA_ARM64_GEO_CORE_INCLUDED

#if !defined(DAYTONA_ARM64_GEO_CORE_TU) && !defined(DAYTONA_ARM64_GEO_WRAPPER_INCLUDED)
#define DAYTONA_ARM64_GEO_WRAPPER_INCLUDED
namespace rt {

enum class GeoMode : uint8_t { Sync = 0, Pipelined = 1 };
const char *geo_mode_name(GeoMode mode);

class Geo {
public:
    using Clock = uint64_t (*)();
    static constexpr uint32_t kBufferWords = 0x8000;

    struct Stats {
        uint64_t parse_calls = 0;
        uint64_t jobs = 0;
        uint64_t worker_ticks = 0;
        uint64_t inline_ticks = 0;
        uint64_t latency_ticks = 0;
        uint64_t latency_max = 0;
        uint64_t snapshot_ticks = 0;
        uint64_t wait_ticks = 0;
        uint64_t wait_max = 0;
        uint64_t blocked_waits = 0;
        uint64_t count_reads = 0;
        uint64_t polys = 0;
        uint64_t polys_max = 0;
        uint64_t failures = 0;
        uint64_t last_parse_call = 0, last_parse_return = 0;
        uint64_t last_output_call = 0;
    };

    struct Worker {
        bool threaded = false;
        int core = -1;
        int affinity_result = -1;
        int fe_round_main = 0;
        int fe_round_worker = 0;
    };

    Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer);
    ~Geo();
    Geo(const Geo &) = delete;
    Geo &operator=(const Geo &) = delete;

    void parse(uint32_t read_start);
    void zclip_w(uint32_t data) { zclip_ = data; }
    void set_wide_margin(int pixels);
    int windows() const;

    class PolyList {
    public:
        size_t size() const;
        operator const std::vector<GeoPoly> &() const;
    private:
        friend class Geo;
        explicit PolyList(Geo *geo) : geo_(geo) {}
        Geo *geo_;
    };
    PolyList polys{this};

    static Geo *find(const uint32_t *live_buffer);
    bool configure(GeoMode mode, int core, Clock clock);
    GeoMode mode() const { return mode_; }
    const Stats &stats() const { return stats_; }
    const Worker &worker() const { return worker_; }
    void wait();

private:
    void rethrow_pending();
    void publish();
    bool start_thread(int core);
    void stop_thread() noexcept;
    enum class Read : uint8_t { None, List, Windows };
    void output_read(Read kind);
    uint64_t now() const { return clock_ ? clock_() : 0; }
    static void run_job(Geo &self);
    void worker_loop();

    uint32_t *live_;
    std::unique_ptr<uint32_t[]> snapshot_;
    GeoCore core_;
    GeoMode mode_ = GeoMode::Sync;
    Clock clock_ = nullptr;
    uint32_t zclip_ = 0;
    bool in_flight_ = false, unpublished_ = false;
    unsigned job_age_ = 0;
    Read pair_ = Read::None;
    std::vector<GeoPoly> front_;
    int front_windows_ = 0;
    uint32_t latest_count_ = 0;
    std::exception_ptr pending_error_;
    Stats stats_;
    Worker worker_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_start_;
    std::condition_variable cv_done_;
    bool job_ready_ = false;
    bool job_done_ = true;
    bool stop_ = false;
    bool initialized_ = false;
    uint32_t job_read_start_ = 0, job_zclip_ = 0;
    uint64_t job_start_ = 0, job_end_ = 0, job_ticks_ = 0;
    uint32_t job_count_ = 0;
    int job_windows_ = 0;
    std::exception_ptr job_error_;
};

} // namespace rt
#endif
