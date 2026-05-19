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

static int sysfs_read_u32(const char *path, uint32_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -errno;
    int rc = fscanf(f, "%u", out);
    fclose(f);
    return (rc == 1) ? 0 : -EIO;
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
        if (sysfs_write(path, "1200000") != 0) errors++;   /* 1.2 GHz */
    }

    /* 3. GPU: disable clocking (Mali-G76) */
    sysfs_write("/sys/class/misc/mali0/device/power_policy",  "always_off");
    sysfs_write("/sys/class/kgsl/kgsl-3d0/pwrctrl/gpuclk",    "0");

    /* 4. Scheduler: hint cores that the workload is mostly idle */
    sysfs_write("/proc/sys/kernel/sched_energy_aware", "1");

    /* 5. VM: reduce swappiness to near-zero (audio buffers must stay in RAM) */
    sysfs_write("/proc/sys/vm/swappiness", "10");

    /* 6. Dirty-page flush: write-back less aggressively to lower eMMC wakeup */
    sysfs_write("/proc/sys/vm/dirty_writeback_centisecs", "6000");
    sysfs_write("/proc/sys/vm/dirty_expire_centisecs",    "12000");

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

    const char *a55_cores[] = {"cpu0","cpu1","cpu2","cpu3","cpu4","cpu5","cpu6","cpu7"};
    for (size_t i = 0; i < sizeof(a55_cores)/sizeof(a55_cores[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_governor", a55_cores[i]);
        sysfs_write(path, "schedutil");
    }
    sysfs_write("/sys/class/misc/mali0/device/power_policy", "demand");
    LOGI("pwrd: default CPU policy restored");
}

int cpu_read_power_mA(uint32_t *out_mA)
{
    /* Read from powercap RAPL-style sysfs if available on this platform.
     * MT6785 does not expose RAPL; this is a best-effort path. */
    uint32_t uw = 0;
    int rc = sysfs_read_u32(
        "/sys/class/power_supply/battery/current_now", &uw);
    if (rc == 0)
        *out_mA = uw / 1000;   /* µA → mA */
    return rc;
}
