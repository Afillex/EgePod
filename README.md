# EgePod — a bare-metal Digital Audio Player OS

EgePod turns a **Redmi Note 10S** (MediaTek Helio G95, *rosemary*) into a dedicated,
distraction-free **Digital Audio Player**. Instead of booting Android, the phone runs a small set
of native C/C++ daemons that drive the screen, the buttons, and a pure **ALSA** audio pipeline
straight to the 3.5 mm jack — no AudioFlinger, no resampling, no radios.

It started as a simple question: *how little software does it take to just play music well?*

> ⚠️ Hobby/research project for hardware I own. It bypasses the stock OS and disables radios at the
> kernel level — flashing it is destructive and device-specific.

## Why it's interesting

- **Ruthless power budget.** The design target is **20–30 mA** during screen-off playback. Big
  Cortex-A76 cores are hot-plugged offline, Wi-Fi/Bluetooth/modem are disabled in the kernel, and
  the UI thread blocks on hardware interrupts (`/dev/input/eventX`) — never polling — so it draws
  ~0 % CPU the moment the screen is off.
- **Pure audio path.** Decoded PCM is pushed directly through ALSA to the codec, skipping Android's
  mixer stack entirely.
- **Clean multi-process design.** Three small daemons with a single responsibility each, talking
  over Unix-domain-socket IPC.

## Architecture

```
        ┌──────────┐   IPC (unix socket)   ┌──────────┐
        │   uid    │◀─────────────────────▶│  audiod  │
        │ (UI/FB)  │                        │ (ALSA)   │
        └────┬─────┘                        └────┬─────┘
             │                                   │
   /dev/input/eventX                       ALSA / codec ──▶ 🎧 3.5mm
             │                                   │
        ┌────┴───────────────────────────────────┴────┐
        │                    pwrd                       │
        │  CPU governor · hotplug · rfkill · suspend    │
        └──────────────────────────────────────────────┘
```

| Daemon | Responsibility | Key files |
|--------|----------------|-----------|
| **`audiod`** | Music index, decoding, ALSA output, headset (h2w) detect, playback state persistence | `src/audiod/{player,decoder,alsa_out,index,persist,h2w}.c` |
| **`pwrd`**   | CPU governor + core hotplug, radio kill (rfkill), deep-sleep management | `src/pwrd/{cpu,rfkill,main}.c` |
| **`uid`**    | Framebuffer rendering, bitmap font, interrupt-driven input, IPC client | `src/uid/{fb,render,input,main}.c` |
| `common/`    | Shared IPC protocol, track model, logging | `src/common/{ipc,track,log}.h` |

## Try it without the hardware (simulator)

You don't need the phone to see it run. The `sim/` harness builds the daemons for a Linux VM and
streams the framebuffer + audio back to a macOS viewer.

```bash
./simulate.sh            # build if needed, start daemons, open the framebuffer viewer
./simulate.sh --rebuild  # force a clean recompile first
MUSIC_DIR=/path ./simulate.sh
```

Requirements: OrbStack (Linux VM) and SDL2 (`brew install sdl2`). See [`sim/README.md`](sim/README.md).

## Tech

C · C++ · ALSA (tinyalsa) · Linux kernel `defconfig` · framebuffer · POSIX IPC (`poll`/`epoll`) ·
AArch64 cross-compilation · QEMU/OrbStack simulation

## Status

Actively built and simulated. Real-device bring-up (kernel patch in `kernel/`, flashing in
`build/`) is partial and specific to *rosemary*. The simulator path is the easiest way to explore
the code.
