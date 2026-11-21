// ARM64 Linux replacement of src/runtime/geo.cpp:
//   1. Unmodified src/runtime/geo.cpp compiled as rt::GeoCore.
//   2. rt::Geo wrapper running GeoCore::parse() pipelined on Core 1 via std::thread.
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "../../thread_affinity.h"

#define DAYTONA_ARM64_GEO_CORE_TU 1
#include "../../../../src/runtime/geo.cpp"
#undef Geo
#undef DAYTONA_ARM64_GEO_CORE_TU

#include "runtime/geo.h"

namespace rt {

namespace {
std::vector<Geo *> &registry() {
    static std::vector<Geo *> geos;
    return geos;
}
} // namespace

const char *geo_mode_name(GeoMode mode) {
    switch (mode) {
    case GeoMode::Sync: return "SYNC";
    case GeoMode::Pipelined: return "ASYNC_PIPELINED";
    }
    return "?";
}

Geo::Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer)
    : live_(buffer), snapshot_(new uint32_t[kBufferWords]), core_(polygons, textures, snapshot_.get()) {
    std::memcpy(snapshot_.get(), live_, kBufferWords * sizeof(uint32_t));
    registry().push_back(this);
}

Geo::~Geo() {
    try { wait(); } catch (...) {}
    stop_thread();
    auto &geos = registry();
    geos.erase(std::remove(geos.begin(), geos.end(), this), geos.end());
}

Geo *Geo::find(const uint32_t *live_buffer) {
    for (Geo *geo : registry())
        if (geo->live_ == live_buffer) return geo;
    return nullptr;
}

void Geo::worker_loop() {
    arm64::set_current_thread_name("daytona_geo");
    std::fesetround(worker_.fe_round_main);
    worker_.fe_round_worker = std::fegetround();
    worker_.affinity_result = arm64::pin_current_thread(worker_.core);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        initialized_ = true;
    }
    cv_done_.notify_one();

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_start_.wait(lk, [&] { return job_ready_ || stop_; });
            if (stop_) break;
            job_ready_ = false;
        }
        run_job(*this);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            job_done_ = true;
        }
        cv_done_.notify_one();
    }
}

bool Geo::start_thread(int core) {
    if (thread_.joinable()) return true;
    worker_ = {};
    worker_.core = core;
    worker_.fe_round_main = std::fegetround();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = false;
        job_ready_ = false;
        job_done_ = true;
        initialized_ = false;
    }
    try {
        thread_ = std::thread(&Geo::worker_loop, this);
    } catch (...) {
        return false;
    }
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_done_.wait(lk, [&] { return initialized_; });
    }
    worker_.threaded = true;
    return true;
}

void Geo::stop_thread() noexcept {
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_start_.notify_one();
        thread_.join();
    }
    worker_.threaded = false;
}

bool Geo::configure(GeoMode mode, int core, Clock clock) {
    wait();
    clock_ = clock;
    if (mode != GeoMode::Sync && !start_thread(core)) mode = GeoMode::Sync;
    if (mode == GeoMode::Sync) publish();
    mode_ = mode;
    return mode == GeoMode::Sync || worker_.threaded;
}

void Geo::run_job(Geo &self) {
    const uint64_t begin = self.now();
    try {
        self.core_.zclip_w(self.job_zclip_);
        self.core_.parse(self.job_read_start_);
    } catch (...) {
        self.job_error_ = std::current_exception();
    }
    self.job_count_ = uint32_t(self.core_.polys.size());
    self.job_windows_ = self.core_.windows();
    self.job_end_ = self.now();
    self.job_ticks_ = self.job_end_ >= begin ? self.job_end_ - begin : 0;
}

void Geo::wait() {
    if (!in_flight_) return;
    const uint64_t begin = now();
    bool blocked = false;
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (!job_done_) {
            blocked = true;
            cv_done_.wait(lk, [&] { return job_done_; });
        }
    }
    const uint64_t end = now();
    in_flight_ = false;
    const uint64_t waited = end >= begin ? end - begin : 0;
    stats_.wait_ticks += waited;
    stats_.wait_max = std::max(stats_.wait_max, waited);
    if (blocked) ++stats_.blocked_waits;
    ++stats_.jobs;
    stats_.worker_ticks += job_ticks_;
    const uint64_t latency = job_end_ >= job_start_ ? job_end_ - job_start_ : 0;
    stats_.latency_ticks += latency;
    stats_.latency_max = std::max(stats_.latency_max, latency);
    stats_.polys += job_count_;
    stats_.polys_max = std::max<uint64_t>(stats_.polys_max, job_count_);
    latest_count_ = job_count_;
    if (job_error_) {
        ++stats_.failures;
        pending_error_ = std::exchange(job_error_, nullptr);
    }
}

void Geo::rethrow_pending() {
    if (pending_error_) std::rethrow_exception(std::exchange(pending_error_, nullptr));
}

void Geo::publish() {
    if (!unpublished_) return;
    front_.swap(core_.polys);
    front_windows_ = job_windows_;
    unpublished_ = false;
}

void Geo::parse(uint32_t read_start) {
    stats_.last_parse_call = now();
    ++stats_.parse_calls;
    wait();
    rethrow_pending();
    if (mode_ == GeoMode::Pipelined) publish();
    const uint64_t t0 = now();
    std::memcpy(snapshot_.get(), live_, kBufferWords * sizeof(uint32_t));
    const uint64_t t1 = now();
    stats_.snapshot_ticks += t1 - t0;
    job_read_start_ = read_start;
    job_zclip_ = zclip_;
    job_start_ = t1;
    job_age_ = 0;
    pair_ = Read::None;
    unpublished_ = true;
    if (mode_ == GeoMode::Sync) {
        run_job(*this);
        stats_.inline_ticks += job_ticks_;
        stats_.polys += job_count_;
        stats_.polys_max = std::max<uint64_t>(stats_.polys_max, job_count_);
        latest_count_ = job_count_;
        if (job_error_) {
            ++stats_.failures;
            unpublished_ = false;
            std::rethrow_exception(std::exchange(job_error_, nullptr));
        }
        publish();
    } else {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            job_done_ = false;
            job_ready_ = true;
            in_flight_ = true;
        }
        cv_start_.notify_one();
    }
    stats_.last_parse_return = now();
}

void Geo::set_wide_margin(int pixels) {
    wait();
    core_.set_wide_margin(pixels);
}

void Geo::output_read(Read kind) {
    if (pair_ != Read::None && pair_ != kind) {
        pair_ = Read::None;
        return;
    }
    pair_ = kind;
    stats_.last_output_call = now();
    if (mode_ == GeoMode::Pipelined && unpublished_) {
        if (job_age_ >= 1) {
            wait();
            rethrow_pending();
            publish();
        } else {
            ++job_age_;
        }
    }
}

int Geo::windows() const {
    auto &self = const_cast<Geo &>(*this);
    self.output_read(Read::Windows);
    return front_windows_;
}

size_t Geo::PolyList::size() const {
    ++geo_->stats_.count_reads;
    geo_->wait();
    geo_->rethrow_pending();
    return geo_->latest_count_;
}

Geo::PolyList::operator const std::vector<GeoPoly> &() const {
    geo_->output_read(Read::List);
    return geo_->front_;
}

} // namespace rt
