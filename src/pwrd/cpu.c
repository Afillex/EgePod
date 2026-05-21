/* 🔋 Power & Governor Warden
 *
 * CPU topology (Helio G95 / MT6785):
 *   CPU0-5 → Cortex-A55 @ 2.0 GHz  (keep online, cap at 1.2 GHz)
 *   CPU6-7 → Cortex-A76 @ 2.05 GHz (hotplug OFFLINE — not needed for audio)
 *
 * GPU: Mali-G76 MC4 — powered off entirely.
 * All writes are idempotent sysfs operations; safe to run multiple times. */

#include "cpu.h"
#include "../common/log.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/* ── sysfs write helper ────────────────────────────────────────────────── */

/* In simulation mode, redirect sysfs writes to EGEPOD_SYSFS_ROOT so we
 * never touch the host kernel's real sysfs. */
static const char *sysfs_root(void)
{
#ifdef SIMULATE
    const char *r = getenv("EGEPOD_SYSFS_ROOT");
    return r ? r : "/opt/egepod-sim/sysfs";
#else
    return "";
#endif
}

static int sysfs_write(const char *path, const char *value)
{
    char full[256];
    const char *root = sysfs_root();
    if (root[0])
        snprintf(full, sizeof(full), "%s/%s", root, path + 1); /* skip leading / */
    else
        snprintf(full, sizeof(full), "%s", path);

    FILE *f = fopen(full, "w");
    if (!f) {
        LOGW("pwrd: sysfs write(%s = '%s'): %s", full, value, strerror(errno));
        return -errno;
    }
    fputs(value, f);
    fclose(f);
    LOGD("pwrd: %s = %s", full, value);
    return 0;
}

/* ── power policy ──────────────────────────────────────────────────────── */

int cpu_apply_dap_policy(void)
{
    int errors = 0;   /* count of failed writes */

    /* 1. Hotplug A76 cores offline */
    if (sysfs_write("/sys/devices/system/cpu/cpu6/online", "0") != 0) errors++;
    if (sysfs_write("/sys/devices/system/cpu/cpu7/online", "0") != 0) errors++;

    /* 2. Governor: powersave on all remaining cores.
     *    Cap A55 max frequency to 1.2 GHz — plenty for a FLAC decoder. */
    const char *a55_cores[] = {"cpu0","cpu1","cpu2","cpu3","cpu4","cpu5"};
    for (size_t i = 0; i < sizeof(a55_cores)/sizeof(a55_cores[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_governor", a55_cores[i]);
        if (sysfs_write(path, "powersave") != 0) errors++;

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_max_freq", a55_cores[i]);
        if (sysfs_write(path, "1200000") != 0) errors++;   /* 1.2 GHz ceiling */

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_min_freq", a55_cores[i]);
        if (sysfs_write(path, "400000")  != 0) errors++;   /* 400 MHz floor */
    }

    /* 3. GPU: disable clocking (Mali-G76 on MT6785).
     * kgsl is Qualcomm/Adreno — wrong GPU vendor.  Mali path only. */
    sysfs_write("/sys/class/misc/mali0/device/power_policy",  "always_off");

    /* 4. Scheduler: hint cores that the workload is mostly idle */
    sysfs_write("/proc/sys/kernel/sched_energy_aware", "1");

    /* 5. VM: reduce swappiness to near-zero (audio buffers must stay in RAM) */
    sysfs_write("/proc/sys/vm/swappiness", "10");

    /* 6. Dirty-page flush: write-back less aggressively to lower eMMC wakeup */
    sysfs_write("/proc/sys/vm/dirty_writeback_centisecs", "6000");
    sysfs_write("/proc/sys/vm/dirty_expire_centisecs",    "12000");

    /* 7. Transparent hugepages: madvise mode — only allocate huge pages when
     * the caller opts in with madvise(MADV_HUGEPAGE).  Decoder does this for
     * the ~30 MB mlock'd PCM buffer, reducing TLB pressure during playback.
     * "never" would disable THP entirely, wasting the kernel config option.
     * "always" causes background compaction wakeups — not wanted for DAP. */
    sysfs_write("/sys/kernel/mm/transparent_hugepage/enabled", "madvise");
    sysfs_write("/sys/kernel/mm/transparent_hugepage/defrag",  "defer+madvise");

    /* 8. eMMC I/O scheduler: no-op removes periodic elevator timer wakeups */
    sysfs_write("/sys/block/mmcblk0/queue/scheduler", "none");

    /* 9. Timer migration: keep timers on their origin CPU — fewer cross-CPU IPIs */
    sysfs_write("/proc/sys/kernel/timer_migration", "0");

    if (errors > 0)
        LOGW("pwrd: %d sysfs writes failed (some paths may not exist on this kernel)",
             errors);
    else
        LOGI("pwrd: DAP power policy applied — A76 offline, A55 capped at 1.2 GHz");

    return errors;
}

void cpu_restore_defaults(void)
{
    sysfs_write("/sys/devices/system/cpu/cpu6/online", "1");
    sysfs_write("/sys/devices/system/cpu/cpu7/online", "1");

    const char *all_cores[] = {"cpu0","cpu1","cpu2","cpu3","cpu4","cpu5","cpu6","cpu7"};
    for (size_t i = 0; i < sizeof(all_cores)/sizeof(all_cores[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_governor", all_cores[i]);
        sysfs_write(path, "schedutil");

        /* Restore frequency limits to hardware maximums so the device is not
         * permanently throttled after EgePod exits. */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_max_freq", all_cores[i]);
        sysfs_write(path, "2000000");   /* A55 max 2.0 GHz */

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_min_freq", all_cores[i]);
        sysfs_write(path, "500000");    /* Android default floor */
    }
    sysfs_write("/sys/class/misc/mali0/device/power_policy", "demand");
    sysfs_write("/sys/kernel/mm/transparent_hugepage/enabled", "madvise");
    sysfs_write("/proc/sys/kernel/timer_migration", "1");
    sysfs_write("/proc/sys/vm/swappiness", "100");
    sysfs_write("/proc/sys/vm/dirty_writeback_centisecs", "500");
    sysfs_write("/proc/sys/vm/dirty_expire_centisecs",    "3000");
    LOGI("pwrd: default CPU policy restored");
}
