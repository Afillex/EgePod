#!/system/bin/sh
# EgePod power setup — mirrors what egepod_pwrd does via sysfs.
# Useful for manual testing without the daemon.
# Run as root: adb shell /system/bin/egepod_setup_power.sh

set -e

log() { echo "[egepod-power] $*"; }

# ── CPU ─────────────────────────────────────────────────────────────────────
log "Hotplugging A76 cores offline…"
echo 0 > /sys/devices/system/cpu/cpu6/online || log "WARN: cpu6 offline failed"
echo 0 > /sys/devices/system/cpu/cpu7/online || log "WARN: cpu7 offline failed"

log "Setting powersave governor on A55 cluster…"
for cpu in cpu0 cpu1 cpu2 cpu3 cpu4 cpu5; do
    echo powersave > /sys/devices/system/cpu/$cpu/cpufreq/scaling_governor || true
    echo 1200000   > /sys/devices/system/cpu/$cpu/cpufreq/scaling_max_freq  || true
done

# ── GPU ─────────────────────────────────────────────────────────────────────
log "Disabling GPU…"
echo always_off > /sys/class/misc/mali0/device/power_policy || \
    echo 0      > /sys/class/kgsl/kgsl-3d0/pwrctrl/gpuclk  || \
    log "WARN: GPU disable failed (may not be present)"

# ── VM / memory ─────────────────────────────────────────────────────────────
echo 10    > /proc/sys/vm/swappiness
echo 6000  > /proc/sys/vm/dirty_writeback_centisecs
echo 12000 > /proc/sys/vm/dirty_expire_centisecs
echo 1     > /proc/sys/kernel/sched_energy_aware

# ── rfkill ──────────────────────────────────────────────────────────────────
log "Blocking all radios…"
for idx in $(ls /sys/class/rfkill/ 2>/dev/null); do
    echo 1 > /sys/class/rfkill/$idx/soft || true
done

log "Power policy applied."
log "A55 cores (cpu0-5) active at max 1.2 GHz, A76 (cpu6-7) offline."
