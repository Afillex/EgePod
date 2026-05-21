#!/usr/bin/env bash
# Push EgePod binaries to a rooted Redmi Note 10S via ADB.
#
# Prerequisites:
#   - Device connected via USB with ADB enabled
#   - Rooted ROM (Magisk or equivalent) so 'adb root' works
#   - Binaries already built: make  (outputs to out/34/)
#
# Usage:
#   ./build/flash.sh           — push binaries, init.rc, mixer config
#   ./build/flash.sh --kernel  — also flash custom kernel via fastboot

set -euo pipefail

API=${API:-34}
OUT="out/${API}"

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[0;33m'; RST='\033[0m'
ok()   { echo -e "${GRN}✓ $*${RST}"; }
warn() { echo -e "${YLW}⚠ $*${RST}"; }
die()  { echo -e "${RED}✗ $*${RST}"; exit 1; }

# ── Preflight ─────────────────────────────────────────────────────────────────
for bin in egepod_audiod egepod_pwrd egepod_uid; do
    [ -f "$OUT/$bin" ] || die "$OUT/$bin not found — run 'make' first"
done

command -v adb >/dev/null || die "adb not found in PATH"
adb get-state >/dev/null 2>&1   || die "No ADB device connected"

echo "Gaining root access…"
adb root
adb wait-for-device
adb remount || warn "adb remount failed — device may be using overlayfs (try Magisk systemless)"

# Verify we built AArch64 (not accidentally host binary)
ARCH=$(file "$OUT/egepod_audiod" | grep -o 'ARM aarch64' || true)
[ -n "$ARCH" ] || die "$OUT/egepod_audiod is not AArch64 — wrong CC used"
ok "Binary architecture: AArch64"

# ── SELinux: switch to permissive for the DAP session ─────────────────────────
# The egepod services run with seclabel u:r:su:s0 which requires permissive mode
# on most stock SELinux policies.  This persists until next reboot unless baked
# into the kernel command line (androidboot.selinux=permissive).
echo "Setting SELinux permissive…"
adb shell setenforce 0 || warn "setenforce 0 failed — SELinux may block device access"
ok "SELinux set to permissive"

# ── Push binaries ─────────────────────────────────────────────────────────────
echo "Pushing daemons to /system/bin/…"
adb push "$OUT/egepod_audiod"  /system/bin/egepod_audiod
adb push "$OUT/egepod_pwrd"    /system/bin/egepod_pwrd
adb push "$OUT/egepod_uid"     /system/bin/egepod_uid
adb shell chmod 755 /system/bin/egepod_audiod \
                    /system/bin/egepod_pwrd  \
                    /system/bin/egepod_uid
ok "Daemons pushed"

# ── Push init and config ───────────────────────────────────────────────────────
echo "Pushing init.rc and mixer config…"
adb shell mkdir -p /system/etc/init /system/etc/egepod
adb push init/egepod.rc        /system/etc/init/egepod.rc
adb push init/alsa_mixer.conf  /system/etc/egepod/alsa_mixer.conf
ok "Init and config pushed"

# ── Persist SELinux permissive across reboots (optional) ──────────────────────
# Uncomment if you want SELinux permanently permissive (requires remount /system):
# adb shell "echo 'SELINUX=permissive' >> /system/etc/selinux/config" 2>/dev/null || true

# ── Optional kernel flash ─────────────────────────────────────────────────────
if [ "${1:-}" = "--kernel" ]; then
    KIMAGE="out/kernel/rosemary/arch/arm64/boot/Image.gz-dtb"
    [ -f "$KIMAGE" ] || die "Kernel not built — run: ./build/build_kernel.sh first"
    echo "Flashing kernel via fastboot…"
    adb reboot bootloader
    sleep 5
    fastboot flash boot "$KIMAGE"
    fastboot reboot
    ok "Kernel flashed"
fi

adb shell sync
echo ""
ok "Done. Reboot the device to start EgePod:"
echo "   adb reboot"
echo ""
echo "Monitor logs after reboot:"
echo "   adb logcat -s egepod_audiod egepod_pwrd egepod_uid"
