// Regression guard for platform/arm64/thread_affinity.h.
//
// The render loop used to be pinned to a single core. Affinity masks are
// inherited by threads created afterwards, so that one call also trapped
// PulseAudio and every libmali worker thread on that core while the others
// idled: on Rockchip RK3326 the main thread spent ~34% of its time queued for
// a CPU and the port capped near 40 FPS instead of 57.52. Widening the main
// thread to every core dropped that queueing to ~9% and reached the target.
//
// These checks keep the two halves of that contract honest: the main thread
// gets the full set, and worker threads still get exactly their own core.
#include "../platform/arm64/thread_affinity.h"

#include <pthread.h>
#include <sched.h>

#include <cstdio>
#include <thread>
#include <vector>

namespace {

int allowed_cpu_count() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof set, &set) != 0) return -1;
    int n = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &set)) ++n;
    }
    return n;
}

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

} // namespace

int main() {
    const int cpus = arm64::cpu_count();
    check(cpus >= 1, "cpu_count reports at least one online CPU");

    // The main thread starts unrestricted on a normal host; widening must keep
    // it that way rather than narrowing it to one core.
    check(arm64::pin_current_thread_all() == 0, "pin_current_thread_all succeeds");
    const int after_all = allowed_cpu_count();
    check(after_all == cpus, "main thread keeps every online CPU after pin_current_thread_all");
    if (cpus > 1) check(after_all > 1, "main thread is NOT narrowed to a single core");

    // Regression guard for the old behaviour: pinning the calling (main) thread
    // to one core is what caused the starvation.
    if (cpus > 1) {
        arm64::pin_current_thread(0);
        check(allowed_cpu_count() == 1, "pin_current_thread(0) still restricts a thread to one core");
        arm64::pin_current_thread_all();
        check(allowed_cpu_count() == cpus, "pin_current_thread_all widens back to every CPU");
    }

    // Worker threads must each land on exactly the core they ask for.
    std::vector<int> worker_cpus;
    std::vector<int> worker_allowed;
    auto worker = [&](int core) {
        arm64::set_current_thread_name("affinity_test");
        arm64::pin_current_thread(core);
        worker_cpus.push_back(core);
        worker_allowed.push_back(allowed_cpu_count());
    };
    {
        std::thread a(worker, 0), b(worker, cpus > 1 ? 1 : 0);
        a.join();
        b.join();
    }
    check(worker_allowed.size() == 2, "two worker threads ran");
    bool each_pinned = true;
    for (int n : worker_allowed) {
        if (n != 1) each_pinned = false;
    }
    check(each_pinned, "each pinned worker is restricted to a single core");

    // Out-of-range and negative requests are reported, not silently applied.
    check(arm64::pin_current_thread(-1) == -1, "negative core is refused");
    check(arm64::pin_current_thread(cpus + 4) == -1, "out-of-range core is refused");

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
