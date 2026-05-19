#!/usr/bin/env bash
# Start the full EgePod simulation inside the OrbStack VM.
# Launches all three daemons + the framebuffer viewer.
# Usage: sim/run_sim.sh [--no-viewer]

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out/sim"

# ── Verify build ──────────────────────────────────────────────────────────────
for bin in egepod_audiod egepod_pwrd egepod_uid fb_viewer; do
    [ -f "$OUT/$bin" ] || { echo "ERROR: $OUT/$bin not found — run: make -f sim/Makefile.sim"; exit 1; }
done

# ── Fake sysfs ────────────────────────────────────────────────────────────────
export EGEPOD_SYSFS_ROOT="${EGEPOD_SYSFS_ROOT:-/opt/egepod-sim/sysfs}"
[ -d "$EGEPOD_SYSFS_ROOT" ] || {
    echo "WARN: $EGEPOD_SYSFS_ROOT not found — run sim/setup_sim.sh first"
    mkdir -p "$EGEPOD_SYSFS_ROOT"
}

# ── Virtual framebuffer ───────────────────────────────────────────────────────
if [ ! -e /dev/fb0 ]; then
    echo "Loading vfb kernel module…"
    sudo modprobe vfb video_mode=1080x2400-32
    sleep 0.5
fi
sudo chmod 666 /dev/fb0

# ── Socket directory ──────────────────────────────────────────────────────────
sudo mkdir -p /dev/socket
sudo chmod 777 /dev/socket

# ── Music directory ───────────────────────────────────────────────────────────
mkdir -p /sdcard/Music
if [ -z "$(ls /sdcard/Music/*.wav 2>/dev/null)" ] && \
   [ -z "$(ls /sdcard/Music/*.flac 2>/dev/null)" ] && \
   [ -z "$(ls /sdcard/Music/*.mp3 2>/dev/null)" ]; then
    echo "WARN: /sdcard/Music is empty."
    echo "  Generate a test tone:  ffmpeg -f lavfi -i 'sine=440:d=30' -ar 44100 -ac 2 /sdcard/Music/test.wav"
    echo "  Or copy your own files to /sdcard/Music/"
fi

# ── Cleanup from previous run ─────────────────────────────────────────────────
rm -f /dev/socket/egepod_audiod /dev/socket/egepod_pwrd
rm -f /dev/egepod_pwrd_ready /dev/egepod_audiod_ready
pkill -f "egepod_audiod|egepod_pwrd|egepod_uid|fb_viewer" 2>/dev/null || true
sleep 0.3

PIDS=()
trap 'echo; echo "Stopping simulation…"; kill "${PIDS[@]}" 2>/dev/null; exit 0' INT TERM

# ── Start daemons in order ────────────────────────────────────────────────────
echo "Starting egepod_pwrd…"
"$OUT/egepod_pwrd" &
PIDS+=($!)
sleep 0.5

echo "Starting egepod_audiod…"
"$OUT/egepod_audiod" &
PIDS+=($!)
sleep 1

echo "Starting egepod_uid…"
"$OUT/egepod_uid" &
PIDS+=($!)
sleep 0.5

# ── Framebuffer viewer ────────────────────────────────────────────────────────
if [ "${1:-}" != "--no-viewer" ]; then
    echo "Starting fb_viewer (SDL2 window)…"
    # In OrbStack, DISPLAY is set automatically for X11 forwarding
    DISPLAY="${DISPLAY:-:0}" "$OUT/fb_viewer" &
    PIDS+=($!)
fi

echo ""
echo "======================================================="
echo " EgePod simulation running."
echo " Daemons: ${PIDS[*]}"
echo " Logs: sudo journalctl -t egepod_audiod -t egepod_pwrd -t egepod_uid -f"
echo " Press Ctrl+C to stop all."
echo "======================================================="

wait "${PIDS[@]}"
