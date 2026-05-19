# EgePod Simulation Guide

## Prerequisites
- OrbStack installed (already present on your Mac)
- An Ubuntu 24.04 AArch64 VM in OrbStack

## Step 1 — Create the VM (one-time)

```bash
# On macOS terminal
orb create ubuntu:24.04 egepod-sim
```

## Step 2 — Enter the VM and run setup (one-time)

```bash
orb shell egepod-sim
sudo bash /mnt/mac/Users/ege/Desktop/Projects/EgePod/sim/setup_sim.sh
```

This installs: clang, libasound2-dev, libsdl2-dev, creates fake sysfs, loads `vfb`, generates a test WAV tone.

## Step 3 — Build the simulation binaries (inside VM)

```bash
cd /mnt/mac/Users/ege/Desktop/Projects/EgePod

# Fetch single-header vendor libs if not done yet
make vendor-fetch

# Build simulation binaries (native AArch64, system ALSA)
make -f sim/Makefile.sim
```

Output: `out/sim/{egepod_audiod, egepod_pwrd, egepod_uid, fb_viewer}`

## Step 4 — Run the simulation

```bash
# Inside the VM
sim/run_sim.sh
```

This starts all three daemons in order and opens an SDL2 window showing the
framebuffer at 40% scale (432×960 px).

## What you'll see

```
macOS screen
└── SDL2 window (432×960)  ← fb_viewer rendering /dev/fb0
    ┌─────────────────────┐
    │ EgePod    BAT 85%   │
    ├─────────────────────┤
    │   [ album art ]     │
    │   test_440hz        │
    │   Unknown — Unknown │
    │ [██████░░░] 0:03/0:30│
    │   |<   ||   >|      │
    └─────────────────────┘
```

Audio plays through OrbStack's audio passthrough to macOS speakers/headphones.

## Interaction

The simulation includes a **fake input generator** for testing gestures:

```bash
# Inside VM — in a second terminal
python3 sim/fake_input.py tap 540 950          # tap play/pause button
python3 sim/fake_input.py swipe_left           # next track
python3 sim/fake_input.py swipe_right          # prev track
python3 sim/fake_input.py power                # screen off/on
```

## Logs

```bash
# All daemon logs go to syslog
sudo journalctl -t egepod_audiod -t egepod_pwrd -t egepod_uid -f
```

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `/dev/fb0` not found | `sudo modprobe vfb video_mode=1080x2400-32` |
| No audio | `aplay -l` — if empty, enable OrbStack audio in Settings → Machines |
| SDL2 window blank | Check `DISPLAY` is set; try `export DISPLAY=:0` |
| `cannot open /dev/socket/egepod_audiod` | `sudo mkdir -p /dev/socket && sudo chmod 777 /dev/socket` |
| `mlock failed` | Normal in VM — audio will work but PCM buffer may be swapped under pressure |
