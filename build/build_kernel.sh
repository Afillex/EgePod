#!/usr/bin/env bash
# Build the rosemary kernel with EgePod defconfig patches.
# Designed to run on macOS Apple Silicon M4 using a Linux AArch64 QEMU VM
# for the actual kernel build step (the kernel Makefile requires a Linux host).
#
# Usage:
#   ./build/build_kernel.sh          — full kernel build
#   ./build/build_kernel.sh qemu     — boot in QEMU for smoke-testing

set -euo pipefail

KERNEL_REPO="https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git"
KERNEL_BRANCH="rosemary-r-oss"
KERNEL_DIR="out/kernel/rosemary"
PATCH="kernel/rosemary_dap.defconfig.patch"

NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
    echo "ERROR: ANDROID_NDK_HOME not set"
    exit 1
fi
CROSS_COMPILE="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android-"

# ── Clone kernel ─────────────────────────────────────────────────────────────
if [ ! -d "$KERNEL_DIR" ]; then
    echo "Cloning kernel…"
    git clone --depth=1 --branch "$KERNEL_BRANCH" "$KERNEL_REPO" "$KERNEL_DIR"
fi

# ── Apply defconfig patch ─────────────────────────────────────────────────────
cd "$KERNEL_DIR"
echo "Applying EgePod defconfig patch…"
patch -p1 --forward --batch < "../../$PATCH" || true

# ── Configure ─────────────────────────────────────────────────────────────────
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" rosemary_defconfig
scripts/config --enable  NO_HZ_FULL
scripts/config --enable  HZ_100
scripts/config --disable BT
scripts/config --disable WLAN
scripts/config --disable MTK_MODEM_CCCI_SUPPORT
scripts/config --enable  RFKILL
scripts/config --enable  CPU_FREQ_GOV_POWERSAVE
scripts/config --enable  MLOCK_MLOCKALL
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig

# ── Build ──────────────────────────────────────────────────────────────────────
echo "Building kernel ($(nproc) jobs)…"
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)" Image.gz-dtb

echo "Kernel image: $KERNEL_DIR/arch/arm64/boot/Image.gz-dtb"

# ── Optional QEMU smoke test ───────────────────────────────────────────────────
if [ "${1:-}" = "qemu" ]; then
    echo "Booting in QEMU (AArch64)…"
    # macOS: install via brew install qemu
    qemu-system-aarch64 \
        -M virt \
        -cpu cortex-a55 \
        -m 512M \
        -kernel arch/arm64/boot/Image.gz \
        -append "console=ttyAMA0 earlycon panic=5" \
        -nographic \
        -no-reboot
fi
