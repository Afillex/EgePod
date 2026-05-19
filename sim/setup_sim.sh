#!/usr/bin/env bash
# EgePod simulation environment setup — run inside OrbStack Ubuntu 24.04 VM.
#
# Usage (from macOS):
#   orb run ubuntu:egepod-sim -- bash /mnt/mac/Desktop/Projects/EgePod/sim/setup_sim.sh

set -euo pipefail

log() { echo -e "\033[1;36m[sim-setup]\033[0m $*"; }

# ── 1. System packages ────────────────────────────────────────────────────────
log "Installing packages…"
apt-get update -qq
apt-get install -y --no-install-recommends \
    clang \
    make \
    libasound2-dev \
    libsdl2-dev \
    v4l-utils \
    usbutils \
    evtest \
    libevdev-dev \
    python3 \
    python3-pip \
    alsa-utils \
    rfkill \
    pciutils \
    curl \
    git

# ── 2. Virtual framebuffer ────────────────────────────────────────────────────
log "Loading virtual framebuffer (vfb) module…"
modprobe vfb video_mode=1080x2400-32 || true
# Verify /dev/fb0 exists
ls -la /dev/fb0 && log "/dev/fb0 ready"

# ── 3. Fake sysfs paths used by egepod_pwrd ──────────────────────────────────
log "Creating fake sysfs paths…"
FAKE=/opt/egepod-sim/sysfs
mkdir -p $FAKE

# CPU online / cpufreq
for i in 0 1 2 3 4 5 6 7; do
    mkdir -p $FAKE/devices/system/cpu/cpu$i/cpufreq
    echo "1"         > $FAKE/devices/system/cpu/cpu$i/online
    echo "schedutil" > $FAKE/devices/system/cpu/cpu$i/cpufreq/scaling_governor
    echo "1200000"   > $FAKE/devices/system/cpu/cpu$i/cpufreq/scaling_max_freq
done

# Battery
mkdir -p $FAKE/class/power_supply/battery
echo "85"       > $FAKE/class/power_supply/battery/capacity
echo "500000"   > $FAKE/class/power_supply/battery/current_now  # 500 mA = 500000 µA

# Thermal
mkdir -p $FAKE/class/thermal/thermal_zone0
echo "38000"    > $FAKE/class/thermal/thermal_zone0/temp        # 38.0°C

# Backlight
mkdir -p $FAKE/class/leds/lcd-backlight
echo "200"      > $FAKE/class/leds/lcd-backlight/brightness

# Bind fake sysfs over /sys using bind mounts so daemons see the right paths
for dir in devices/system/cpu class/power_supply class/thermal class/leds; do
    mkdir -p /sys/$dir 2>/dev/null || true
done
# We use EGEPOD_SYSFS_ROOT env var instead of bind-mounting for safety
log "Fake sysfs at $FAKE — export EGEPOD_SYSFS_ROOT=$FAKE before running daemons"

# ── 4. Fake music directory ───────────────────────────────────────────────────
log "Creating /sdcard/Music with test tone…"
mkdir -p /sdcard/Music
# Generate a 10-second 440 Hz stereo test tone WAV if sox/ffmpeg is available
if command -v ffmpeg &>/dev/null; then
    ffmpeg -f lavfi -i "sine=frequency=440:duration=10" \
           -ar 44100 -ac 2 /sdcard/Music/test_440hz.wav -y -loglevel quiet
    log "Test tone: /sdcard/Music/test_440hz.wav"
elif command -v sox &>/dev/null; then
    sox -n -r 44100 -c 2 /sdcard/Music/test_440hz.wav synth 10 sine 440
else
    log "WARN: ffmpeg/sox not found — copy audio files to /sdcard/Music manually"
fi

# ── 5. ALSA simulation ────────────────────────────────────────────────────────
log "Checking ALSA devices…"
aplay -l 2>/dev/null || log "WARN: no ALSA playback devices (OrbStack audio passthrough may need enabling)"

# ── 6. uinput for fake touch events ──────────────────────────────────────────
modprobe uinput || true
chmod 666 /dev/uinput 2>/dev/null || true

# ── 7. rfkill (real or stubbed) ───────────────────────────────────────────────
# In the VM there may be no real rfkill devices; rfkill_block_all() returns 0.
rfkill list || log "No rfkill devices (expected in VM)"

log ""
log "======================================================"
log " Simulation environment ready."
log " Run: cd /mnt/mac/Desktop/Projects/EgePod"
log "      make -f sim/Makefile.sim"
log "      sim/run_sim.sh"
log "======================================================"
