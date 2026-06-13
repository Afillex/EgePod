#!/bin/bash
# EgePod simulation launcher — run this once from macOS to start everything.
#
# Usage:
#   ./simulate.sh              — build if needed, start VM daemons, open viewer
#   ./simulate.sh --rebuild    — force a full recompile first
#   MUSIC_DIR=/path simulate.sh — use your own music folder
#
# Requirements: OrbStack (VM named "egepod-sim"), SDL2 (brew install sdl2)

set -euo pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
OUT="$PROJ/out/sim"
VIEWER="$OUT/fb_viewer_macos"
FB_RAW="$OUT/egepod_fb.raw"
VM="egepod-sim"
FORCE_REBUILD="${1:-}"
AUDIO_PORT=12321          # TCP port: macOS ffplay listens, VM audiod connects

# ── colours ──────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; YLW='\033[0;33m'; RED='\033[0;31m'; RST='\033[0m'
step()  { echo -e "${GRN}▶ $*${RST}"; }
warn()  { echo -e "${YLW}⚠ $*${RST}"; }
fatal() { echo -e "${RED}✗ $*${RST}"; exit 1; }

# ── 1. check orb is available ─────────────────────────────────────────────────
command -v orb >/dev/null 2>&1 || fatal "orb not found. Install OrbStack."

# ── 2. rebuild VM binaries if sources are newer (or --rebuild) ────────────────
NEWEST_SRC=$(find "$PROJ/src" "$PROJ/sim/Makefile.sim" "$PROJ/sim/fb_viewer.c" \
             -type f 2>/dev/null | xargs ls -t 2>/dev/null | head -1)
OLDEST_BIN="$OUT/egepod_uid"   # uid is the last thing built

need_vm_build=0
[[ "$FORCE_REBUILD" == "--rebuild" ]]         && need_vm_build=1
[[ ! -f "$OLDEST_BIN" ]]                      && need_vm_build=1
[[ -n "$NEWEST_SRC" && "$NEWEST_SRC" -nt "$OLDEST_BIN" ]] && need_vm_build=1

if [[ $need_vm_build -eq 1 ]]; then
    step "Building VM binaries (aarch64-linux)..."
    # Install Nimbus Sans Bold (Helvetica-compatible) if not present
    orb run -m "$VM" sudo bash -c "
        dpkg -l fonts-urw-base35 2>/dev/null | grep -q '^ii' || \
            (apt-get update -qq && apt-get install -y -q fonts-urw-base35)
    " 2>/dev/null || warn "fonts-urw-base35 install skipped — will use DejaVu fallback"
    orb run -m "$VM" bash -c "
        cd '$PROJ'
        [[ '$FORCE_REBUILD' == '--rebuild' ]] && make -f sim/Makefile.sim clean
        make -f sim/Makefile.sim
    " || fatal "VM build failed. Check the OrbStack VM is running: orb start $VM"
else
    step "VM binaries are up-to-date (use --rebuild to force)"
fi

# ── 3. rebuild macOS fb_viewer if needed ─────────────────────────────────────
if [[ ! -f "$VIEWER" || "$PROJ/sim/fb_viewer.c" -nt "$VIEWER" ]]; then
    step "Building macOS fb_viewer..."
    command -v sdl2-config >/dev/null 2>&1 || fatal "SDL2 not found. Run: brew install sdl2"
    mkdir -p "$OUT"
    clang "$PROJ/sim/fb_viewer.c" \
        $(sdl2-config --cflags --libs) \
        -O2 -o "$VIEWER" || fatal "fb_viewer build failed"
fi

# ── 4. kill stale viewer, audio bridge and VM daemons ────────────────────────
step "Stopping previous daemons..."
pkill -f fb_viewer_macos 2>/dev/null || true
pkill -9 ffplay 2>/dev/null || true
# Delete stale sidecar files; also delete the raw FB so no old-UI pixels survive
# until uid renders its first frame (fb_open_sim zero-inits on create).
rm -f "$FB_RAW" "$FB_RAW.tap" "$FB_RAW.page" "$FB_RAW.shutdown" "$FB_RAW.reboot" 2>/dev/null || true
# Force-kill everything in the VM and poll until the processes are gone.
# Two-wave kill: SIGTERM first so threads can unwind, then SIGKILL after 1 s.
orb run -m "$VM" bash -c "
    pkill -15 -f 'egepod_(audiod|pwrd|uid)' 2>/dev/null || true
    sleep 1
    pkill -9  -f 'egepod_(audiod|pwrd|uid)' 2>/dev/null || true
    # Poll up to 6 s for all egepod processes to disappear
    for i in \$(seq 1 60); do
        pgrep -f 'egepod_(audiod|pwrd|uid)' > /dev/null 2>&1 || break
        sleep 0.1
    done
    # Final check — warn if any survived
    if pgrep -f 'egepod_(audiod|pwrd|uid)' > /dev/null 2>&1; then
        echo 'WARNING: some egepod processes still alive after kill' >&2
        pgrep -a -f 'egepod_' >&2
    fi
    rm -f /tmp/egepod_audiod.sock /tmp/egepod_pwrd.sock \
          /tmp/egepod_audiod_ready /tmp/egepod_pwrd_ready
" 2>/dev/null || true

# ── 4.5. start macOS audio sink ──────────────────────────────────────────────
# ffplay listens for a raw s16le PCM TCP connection.  audiod in the VM connects
# to host.orb.internal:AUDIO_PORT.  This avoids the orb stdio bridge bottleneck
# and the OrbStack VM's PulseAudio null sink (VM has no real audio hardware).
#
# If ffplay lacks TCP-listen support (rare), use:
#   nc -l $AUDIO_PORT | ffplay -f s16le -ar 44100 -ac 2 -nodisp -
step "Starting macOS audio sink (ffplay TCP :$AUDIO_PORT)..."
# ffplay 8.x dropped -ac; use -ch_layout stereo instead.
# Wrapped in a restart loop: ffplay's ?listen=1 mode exits after one connection,
# so CMD_STOP closes the TCP fd and ffplay exits.  The loop re-binds within ~50 ms
# so the next CMD_PLAY can reconnect without waiting 5 s.
(while true; do
    ffplay -f s16le -ar 44100 -ch_layout stereo -nodisp -loglevel warning \
        "tcp://0.0.0.0:${AUDIO_PORT}?listen=1" \
        </dev/null >>/tmp/ffplay_audio.log 2>&1
    sleep 0.05
done) &
FFPLAY_PID=$!
# Wait up to 3 s for ffplay to bind the port
_bound=0
for _i in $(seq 1 30); do
    lsof -iTCP:${AUDIO_PORT} -sTCP:LISTEN 2>/dev/null | grep -q . && { _bound=1; break; }
    sleep 0.1
done
if [[ $_bound -eq 0 ]]; then
    warn "ffplay did not bind :${AUDIO_PORT} — audio may be silent"
    warn "Check /tmp/ffplay_audio.log for errors"
else
    step "ffplay listening on :${AUDIO_PORT} (pid $FFPLAY_PID)"
fi

# ── 5. start VM daemons ───────────────────────────────────────────────────────
step "Starting daemons in OrbStack VM ($VM)..."
orb run -m "$VM" bash -c "
    cd '$PROJ'
    export EGEPOD_FB_FILE='$FB_RAW'
    export MUSIC_DIR=\${MUSIC_DIR:-/sdcard/Music}
    # PulseAudio ALSA plugin needs XDG_RUNTIME_DIR to find its socket.
    # Set it explicitly so nohup'd daemons find PA even without a login env.
    export XDG_RUNTIME_DIR=/run/user/\$(id -u)
    export PULSE_SERVER=unix:\$XDG_RUNTIME_DIR/pulse/native
    # TCP audio out: audiod connects to macOS ffplay listener (bypasses null sink)
    export EGEPOD_AUDIO_OUT='tcp://host.orb.internal:$AUDIO_PORT'
    # Let render.c pick the best available font (Nimbus Sans Bold → DejaVu)
    unset EGEPOD_FONT_PATH

    nohup ./out/sim/egepod_pwrd > /tmp/pwrd.log 2>&1 &
    sleep 1
    MUSIC_DIR=\$MUSIC_DIR \
    EGEPOD_AUDIO_OUT=\$EGEPOD_AUDIO_OUT \
    nohup ./out/sim/egepod_audiod > /tmp/audiod.log 2>&1 &
    sleep 3
    EGEPOD_FB_FILE=\$EGEPOD_FB_FILE \
    nohup ./out/sim/egepod_uid > /tmp/uid.log 2>&1 &
    sleep 2
    # Quick sanity check
    running=\$(pgrep -c -f 'out/sim/egepod_(audiod|pwrd|uid)' 2>/dev/null || echo 0)
    echo \"Daemons running: \$running/3\"
    journalctl -n 20 --no-pager 2>/dev/null | grep egepod | grep -v 'DEBUG\|WARN.*sysfs' | tail -6 || true
" || fatal "Failed to start daemons in VM"

# ── 6. wait for framebuffer file (uid writes it on first render) ──────────────
step "Waiting for first frame..."
FB_SIZE=7372800    # 720 × 1280 × 4 bytes × 2 pages (same count as 1280×720)
deadline=$((SECONDS + 15))
while [[ $SECONDS -lt $deadline ]]; do
    sz=$(stat -f%z "$FB_RAW" 2>/dev/null || echo 0)
    [[ "$sz" -eq $FB_SIZE ]] && break
    printf '.'
    sleep 0.5
done
echo ""
sz=$(stat -f%z "$FB_RAW" 2>/dev/null || echo 0)
[[ "$sz" -eq $FB_SIZE ]] || warn "Framebuffer not ready yet — viewer will wait up to 30 s"

# ── 7. open the viewer ────────────────────────────────────────────────────────
step "Opening EgePod framebuffer viewer  (Q or Esc to close)"
echo ""
"$VIEWER" "$FB_RAW"

# ── 8. handle reboot sidecar (pwrd writes this on CMD_REBOOT in sim) ─────────
# pwrd writes the sidecar before sending EVT_SHUTDOWN_PENDING, so it should
# already exist when the viewer exits.  Poll briefly as a safety net for slow
# VirtioFS flushes (the sidecar file is small; 20 × 100ms = 2 s max wait).
for _ri in $(seq 1 20); do
    [[ -f "$FB_RAW.reboot" ]] && break
    sleep 0.1
done
if [[ -f "$FB_RAW.reboot" ]]; then
    rm -f "$FB_RAW.reboot"
    step "EgePod rebooting — restarting simulation..."
    sleep 0.5
    exec "$0" "$@"
fi
