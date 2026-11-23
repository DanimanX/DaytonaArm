// Portable Linux pthread core-pinning helper for 4-core ARM64 handhelds.
//
// Masks are built from /sys/devices/system/cpu/online, never from a 0..n-1
// assumption: firmware hot-unplugs cores (Spruce on A133P can leave {0,3}), and
// a mask naming an offline CPU makes pthread_setaffinity_np fail with EINVAL,
// silently doing nothing.
//
// Worker threads (geometry, sound, System 24 2D) pin themselves to one core
// from inside their own entry point, which is safe: the mask only ever
// restricts that worker.
//
// The MAIN thread must NOT be pinned to a single core. Affinity masks are
// inherited by every thread created afterwards, so a single-core mask applied
// before SDL_Init()/gl_init() also traps PulseAudio's mainloop and hotplug
// threads, the SDL timer, and libmali's worker/queue threads on that one core.
// Measured on Rockchip RK3326 (RG351M): the render loop then competed with the
// GPU driver for CPU0 while cores 1-3 idled, spending ~35% of its time queued
// for a CPU and capping the port near 40 FPS. Spreading the main thread over
// all cores instead cut that queueing to ~5% and reached the 57.52 Hz target.
#pragma once

// Linux-only: both the core-pinning call and cpu_set_t come from
// pthread_setaffinity_np / sched.h, which MSVC, mingw and macOS do not provide.
// Fail loudly here instead of at a missing <pthread.h> deep in a build log.
#if !defined(__linux__)
#error "thread_affinity.h is Linux-only (needs pthread_setaffinity_np and cpu_set_t)"
#endif

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace arm64 {

inline int cpu_count() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? int(n) : 1;
}

// Parse a kernel cpu list ("0-3", "0", "0,3", "0-2,4") into `set`. Returns the
// number of CPUs added, 0 if the text is unusable.
//
// The distinction matters: a mask built as CPUs 0..n-1 is invalid whenever the
// online set is not contiguous. Spruce on Allwinner A133P hot-unplugs cores 1-2
// in its Powersave mode, leaving {0,3}; n is then 2 and 0..1 names core 1, which
// is offline. pthread_setaffinity_np returns EINVAL and silently does nothing.
inline int parse_cpu_list(const char *text, cpu_set_t &set) {
    CPU_ZERO(&set);
    if (!text) return 0;
    int count = 0;
    const char *p = text;
    while (*p) {
        if (*p < '0' || *p > '9') { ++p; continue; }
        char *end = nullptr;
        const long lo = ::strtol(p, &end, 10);
        p = end;
        long hi = lo;
        if (*p == '-') {
            ++p;
            hi = ::strtol(p, &end, 10);
            p = end;
        }
        if (lo < 0 || hi < lo) continue;
        for (long c = lo; c <= hi && c < CPU_SETSIZE; ++c) {
            CPU_SET(int(c), &set);
            ++count;
        }
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
    }
    return count;
}

// Fill `set` with the CPUs the kernel reports as online. Falls back to a
// contiguous 0..n-1 set only when /sys is unreadable. Returns the CPU count.
inline int online_cpu_mask(cpu_set_t &set) {
    char buf[256] = {};
    if (FILE *f = ::fopen("/sys/devices/system/cpu/online", "r")) {
        const char *got = ::fgets(buf, sizeof buf, f);
        ::fclose(f);
        if (got) {
            const int n = parse_cpu_list(buf, set);
            if (n > 0) return n;
        }
    }
    CPU_ZERO(&set);
    const int n = cpu_count();
    int count = 0;
    for (int i = 0; i < n && i < CPU_SETSIZE; ++i) {
        CPU_SET(i, &set);
        ++count;
    }
    return count;
}

// Widen the calling thread to every online CPU. Use this for the main thread
// before creating any other thread, so library-spawned threads inherit the
// full set instead of a single core.
inline int pin_current_thread_all() {
    cpu_set_t set;
    if (online_cpu_mask(set) <= 0) return -1;
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
}

// Pin calling thread to `core`. Returns 0 on success, -1 if the core is not
// online or the call failed.
// Only for dedicated worker threads that pin themselves; never for the main thread.
inline int pin_current_thread(int core) {
    if (core < 0 || core >= CPU_SETSIZE) return -1;
    cpu_set_t online;
    if (online_cpu_mask(online) <= 1) return -1;
    if (!CPU_ISSET(core, &online)) return -1; // offline core: a mask naming it is EINVAL
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
}

inline void set_current_thread_name(const char *name) {
#if defined(__linux__)
    if (name) ::pthread_setname_np(::pthread_self(), name);
#else
    (void)name;
#endif
}

} // namespace arm64
