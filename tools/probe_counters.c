/* Can this machine count hardware events for a user process?
 *
 * Opens each event with perf_event_open, runs a busy loop, reads the count.
 * A software event (task-clock) is tried first: if even that fails, the problem
 * is permissions (kernel.perf_event_paranoid), not the hardware. If the software
 * event works but the hardware ones do not, there is no PMU exposed -- which is
 * what a VM usually looks like.
 *
 *   gcc -O1 -o probe probe_counters.c && ./probe
 */
#include <errno.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static long long count(uint32_t type, uint64_t config, int *err) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = type;
    a.config = config;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    int fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (fd < 0) { *err = errno; return -1; }
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    volatile unsigned long x = 0;
    for (unsigned long k = 0; k < 50000000UL; k++) x += k;
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    long long v = 0;
    if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) v = -1;
    close(fd);
    *err = 0;
    return v;
}

int main(void) {
    int err = 0;
    long long sw = count(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, &err);
    printf("%-14s %s\n", "task-clock",
           sw > 0 ? "ok (software events work)" : strerror(err ? err : EINVAL));
    if (sw <= 0) {
        printf("\nRESULT: PERMISSION PROBLEM, not a hardware one.\n");
        printf("  sudo sysctl -w kernel.perf_event_paranoid=2   then run again\n");
        return 2;
    }

    const char *names[] = {"cycles", "instructions", "cache-misses", "branch-misses"};
    const uint64_t cfg[] = {PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS,
                            PERF_COUNT_HW_CACHE_MISSES, PERF_COUNT_HW_BRANCH_MISSES};
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        long long v = count(PERF_TYPE_HARDWARE, cfg[i], &err);
        if (v < 0) printf("%-14s FAIL  %s\n", names[i], strerror(err));
        else       printf("%-14s %s  %lld\n", names[i], v > 0 ? "ok  " : "ZERO", v);
        /* An event that opens but counts zero is not counting. */
        if (v > 0) ok++;
    }
    if (ok >= 3) {
        printf("\nRESULT: HARDWARE COUNTERS AVAILABLE\n");
        return 0;
    }
    printf("\nRESULT: NO HARDWARE COUNTERS. Software events work, so this is not\n");
    printf("a permission problem: the PMU is not exposed here (typical of a VM).\n");
    return 1;
}
