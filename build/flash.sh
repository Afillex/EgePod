#!/usr/bin/env bash
# Flash EgePod binaries to the Redmi Note 10S via ADB.
# The device must already have a rooted, debuggable ROM (e.g. TWRP + Magisk).
#
# Usage: ./build/flash.sh [--kernel]

set -euo pipefail
API=34
OUT="out/$API"

# ── Verify binaries exist ────────────────────────────────────────────────────
for bin in egepod_audiod egepod_pwrd egepod_uid; do
    [ -f "$OUT/$bin" ] || { echo "ERROR: $OUT/$bin not found — run 'make' first"; exit 1; }
done

# ── Push binaries ─────────────────────────────────────────────────────────────
echo "Pushing daemons…"
adb root && adb remount

adb push "$OUT/egepod_audiod"  /system/bin/egepod_audiod
adb push "$OUT/egepod_pwrd"    /system/bin/egepod_pwrd
adb push "$OUT/egepod_uid"     /system/bin/egepod_uid
adb shell chmod 755 /system/bin/egepod_audiod
adb shell chmod 755 /system/bin/egepod_pwrd
adb shell chmod 755 /system/bin/egepod_uid

# ── Push init and config ──────────────────────────────────────────────────────
adb shell mkdir -p /system/etc/egepod
adb push init/egepod.rc       /system/etc/init/egepod.rc
adb push init/alsa_mixer.conf /system/etc/egepod/alsa_mixer.conf

# ── Optional kernel flash (--kernel flag) ────────────────────────────────────
if [ "${1:-}" = "--kernel" ]; then
    KIMAGE="out/kernel/rosemary/arch/arm64/boot/Image.gz-dtb"
    [ -f "$KIMAGE" ] || { echo "ERROR: kernel not built"; exit 1; }
    echo "Flashing kernel via fastboot…"
    adb reboot bootloader
    sleep 3
    fastboot flash boot "$KIMAGE"
    fastboot reboot
fi

adb shell sync
echo "Done. Reboot device to start EgePod."
