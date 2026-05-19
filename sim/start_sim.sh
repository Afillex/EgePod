#!/bin/bash
# Start all EgePod simulation daemons in OrbStack VM.
# Usage: orb run -m egepod-sim bash /Users/ege/Desktop/Projects/EgePod/sim/start_sim.sh
#
# MUSIC:  Drop .flac/.wav/.mp3 files anywhere on macOS and set MUSIC_DIR below.
#         The VM sees the same path (VirtioFS maps /Users/ege/).
#         Default is /sdcard/Music which has two test tones.
#
# AUDIO:  Routes through OrbStack PulseAudio → macOS audio output (speakers/headphones).

cd /Users/ege/Desktop/Projects/EgePod

pkill -f egepod_audiod 2>/dev/null
pkill -f egepod_pwrd   2>/dev/null
pkill -f egepod_uid    2>/dev/null
sleep 0.4

# ── configuration ────────────────────────────────────────────────────────────
# Change MUSIC_DIR to your music folder (accessible from the VM at same path):
#   export MUSIC_DIR=/Users/ege/Music
#   export MUSIC_DIR=/Users/ege/Desktop/mymusic
export MUSIC_DIR="${MUSIC_DIR:-/sdcard/Music}"
export EGEPOD_FB_FILE=/Users/ege/Desktop/Projects/EgePod/out/sim/egepod_fb.raw
export EGEPOD_FONT_PATH=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
export PULSE_SERVER=unix:/run/user/501/pulse/native
# ─────────────────────────────────────────────────────────────────────────────

rm -f /tmp/egepod_audiod_ready /tmp/egepod_pwrd_ready
rm -f /tmp/egepod_audiod.sock /tmp/egepod_pwrd.sock

nohup ./out/sim/egepod_pwrd > /tmp/pwrd.log 2>&1 &
echo "pwrd started (pid $!)"
sleep 1

MUSIC_DIR=$MUSIC_DIR \
PULSE_SERVER=$PULSE_SERVER \
nohup ./out/sim/egepod_audiod > /tmp/audiod.log 2>&1 &
echo "audiod started (pid $!), music=$MUSIC_DIR"
sleep 3

EGEPOD_FB_FILE=$EGEPOD_FB_FILE \
EGEPOD_FONT_PATH=$EGEPOD_FONT_PATH \
nohup ./out/sim/egepod_uid > /tmp/uid.log 2>&1 &
echo "uid started (pid $!)"
sleep 2

echo ""
echo "=== Running daemons ==="
ps aux | grep -E 'egepod_(audiod|pwrd|uid)' | grep -v grep

echo ""
echo "=== System log (egepod) ==="
journalctl -n 15 --no-pager 2>/dev/null | grep egepod | tail -12
