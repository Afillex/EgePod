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

# ── 4. kill stale viewer and VM daemons ──────────────────────────────────────
step "Stopping previous daemons..."
pkill -f fb_viewer_macos 2>/dev/null || true
orb run -m "$VM" bash -c "
    pkill -f egepod_audiod 2>/dev/null; \
    pkill -f egepod_pwrd   2>/dev/null; \
    pkill -f egepod_uid    2>/dev/null; \
    sleep 0.3; \
    rm -f /tmp/egepod_audiod.sock /tmp/egepod_pwrd.sock \
          /tmp/egepod_audiod_ready /tmp/egepod_pwrd_ready
" 2>/dev/null || true

# ── 5. start VM daemons ───────────────────────────────────────────────────────
step "Starting daemons in OrbStack VM ($VM)..."
orb run -m "$VM" bash -c "
    cd '$PROJ'
    export EGEPOD_FB_FILE='$FB_RAW'
    export EGEPOD_FONT_PATH=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
    export PULSE_SERVER=unix:/run/user/501/pulse/native
    export MUSIC_DIR=\${MUSIC_DIR:-/sdcard/Music}

    nohup ./out/sim/egepod_pwrd > /tmp/pwrd.log 2>&1 &
    sleep 1
    MUSIC_DIR=\$MUSIC_DIR PULSE_SERVER=\$PULSE_SERVER \
    nohup ./out/sim/egepod_audiod > /tmp/audiod.log 2>&1 &
    sleep 3
    EGEPOD_FB_FILE=\$EGEPOD_FB_FILE EGEPOD_FONT_PATH=\$EGEPOD_FONT_PATH \
    nohup ./out/sim/egepod_uid > /tmp/uid.log 2>&1 &
    sleep 2
    # Quick sanity check
    running=\$(ps aux | grep -cE 'egepod_(audiod|pwrd|uid)' || true)
    echo \"Daemons running: \$running/3\"
    journalctl -n 6 --no-pager 2>/dev/null | grep egepod | grep -v 'WARN\|sysfs' | tail -5 || true
" || fatal "Failed to start daemons in VM"

# ── 6. wait for framebuffer file (uid writes it on first render) ──────────────
step "Waiting for first frame..."
FB_SIZE=20736000   # 1080 × 2400 × 4 bytes × 2 pages
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
exec "$VIEWER" "$FB_RAW"
