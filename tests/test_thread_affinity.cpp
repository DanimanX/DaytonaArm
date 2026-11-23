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
    // Each thread writes only its own slot: sharing a std::vector here would be
    // a data race and made this check fail intermittently.
    int allowed_first = -1, allowed_second = -1;
    bool pinned_first = false, pinned_second = false;
    auto spawn = [](int core, int &allowed, bool &pinned) {
        std::thread t([core, &allowed, &pinned] {
            arm64::set_current_thread_name("affinity_test");
            pinned = arm64::pin_current_thread(core) == 0;
            allowed = allowed_cpu_count();
        });
        t.join();
    };
    spawn(0, allowed_first, pinned_first);
    spawn(cpus > 1 ? 1 : 0, allowed_second, pinned_second);
    check(pinned_first && pinned_second, "pinning a worker to an online core succeeds");
    check(allowed_first == 1 && allowed_second == 1,
          "each pinned worker is restricted to a single core");

    // Out-of-range and negative requests are reported, not silently applied.
    check(arm64::pin_current_thread(-1) == -1, "negative core is refused");
    check(arm64::pin_current_thread(cpus + 4) == -1, "out-of-range core is refused");

    // Kernel cpu-list parsing. A mask built as CPUs 0..n-1 is wrong whenever the
    // online set is not contiguous (Spruce/A133P Powersave leaves {0,3}).
    {
        cpu_set_t set;
        check(arm64::parse_cpu_list("0-3\n", set) == 4, "parse_cpu_list(\"0-3\") yields 4 CPUs");
        check(CPU_ISSET(0, &set) && CPU_ISSET(3, &set), "parse_cpu_list(\"0-3\") sets 0 and 3");

        check(arm64::parse_cpu_list("0,3\n", set) == 2, "parse_cpu_list(\"0,3\") yields 2 CPUs");
        check(CPU_ISSET(0, &set) && CPU_ISSET(3, &set), "parse_cpu_list(\"0,3\") sets 0 and 3");
        check(!CPU_ISSET(1, &set), "parse_cpu_list(\"0,3\") does NOT name offline CPU 1");

        check(arm64::parse_cpu_list("0\n", set) == 1, "parse_cpu_list(\"0\") yields 1 CPU");
        check(arm64::parse_cpu_list("0-2,4\n", set) == 4, "parse_cpu_list(\"0-2,4\") yields 4 CPUs");
        check(!CPU_ISSET(3, &set) && CPU_ISSET(4, &set), "parse_cpu_list(\"0-2,4\") skips 3, includes 4");
        check(arm64::parse_cpu_list("", set) == 0, "parse_cpu_list(\"\") yields 0");
        check(arm64::parse_cpu_list(nullptr, set) == 0, "parse_cpu_list(nullptr) yields 0");
        check(arm64::parse_cpu_list("70000-70001", set) == 0, "parse_cpu_list refuses out-of-range CPUs");
    }

    // The online mask must reflect the kernel, not a 0..n-1 assumption.
    {
        cpu_set_t set;
        const int n = arm64::online_cpu_mask(set);
        check(n == cpus, "online_cpu_mask agrees with cpu_count");
        int counted = 0;
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &set)) ++counted;
        }
        check(counted == n, "online_cpu_mask sets exactly the reported number of CPUs");
    }

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
