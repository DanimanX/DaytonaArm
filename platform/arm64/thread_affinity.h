// Portable Linux pthread core-pinning helper for 4-core ARM64 handhelds
// (Allwinner H700: unpinned main thread, Core 1 geometry, Core 2 sound, Core 3 2D).
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
#include <unistd.h>

namespace arm64 {

inline int cpu_count() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? int(n) : 1;
}

// Widen the calling thread to every online CPU. Use this for the main thread
// before creating any other thread, so library-spawned threads inherit the
// full set instead of a single core.
inline int pin_current_thread_all() {
    const int n = cpu_count();
    if (n <= 0) return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; ++i) CPU_SET(i, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
}

// Pin calling thread to `core` (0..N-1). Returns 0 on success, -1 if unpinned/skipped.
// Only for dedicated worker threads that pin themselves; never for the main thread.
inline int pin_current_thread(int core) {
    if (core < 0) return -1;
    const int n = cpu_count();
    if (n <= 1 || core >= n) return -1;
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
