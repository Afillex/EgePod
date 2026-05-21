#!/usr/bin/env bash
# Build the rosemary kernel with EgePod DAP patches.
#
# The kernel build MUST run on Linux (kernel Makefiles use Linux-specific tools).
# This script delegates the build to the OrbStack "egepod-sim" AArch64 VM.
#
# Usage (from macOS):
#   ./build/build_kernel.sh          — clone, patch, configure, build
#   ./build/build_kernel.sh --qemu   — also smoke-test in QEMU after build
#
# Output: out/kernel/rosemary/arch/arm64/boot/Image.gz-dtb
# Flash:  ./build/flash.sh --kernel

set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_REPO="https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git"
KERNEL_BRANCH="rosemary-r-oss"
KERNEL_DIR="$PROJ/out/kernel/rosemary"
PATCH="$PROJ/kernel/rosemary_dap.defconfig.patch"
VM="egepod-sim"

RED='\033[0;31m'; GRN='\033[0;32m'; RST='\033[0m'
ok()  { echo -e "${GRN}✓ $*${RST}"; }
die() { echo -e "${RED}✗ $*${RST}"; exit 1; }

command -v orb >/dev/null 2>&1 || die "orb not found — install OrbStack"
orb list 2>/dev/null | grep -q "$VM" || die "OrbStack VM '$VM' not found"

# ── Ensure Linux build tools are present in the VM ───────────────────────────
echo "Installing kernel build dependencies in VM (one-time)…"
orb run -m "$VM" sudo bash -c "
    export DEBIAN_FRONTEND=noninteractive
    dpkg -l clang lld llvm binutils-aarch64-linux-gnu bc flex bison libssl-dev 2>/dev/null \
        | grep -c '^ii' | grep -q '^10$' || \
    apt-get install -y -q \
        clang lld llvm \
        binutils-aarch64-linux-gnu \
        bc flex bison libssl-dev \
        make python3
" 2>/dev/null || true

# ── Clone kernel inside the VM ────────────────────────────────────────────────
if [ ! -d "$KERNEL_DIR/.git" ]; then
    echo "Cloning kernel (this takes ~5 min for a shallow clone)…"
    orb run -m "$VM" bash -c "
        git clone --depth=1 --branch '$KERNEL_BRANCH' '$KERNEL_REPO' '$KERNEL_DIR'
    "
    ok "Kernel cloned → $KERNEL_DIR"
else
    ok "Kernel already cloned at $KERNEL_DIR"
fi

# ── Apply EgePod defconfig patch ─────────────────────────────────────────────
echo "Applying DAP defconfig patch…"
orb run -m "$VM" bash -c "
    cd '$KERNEL_DIR'
    patch -p1 --forward --batch < '$PATCH' 2>/dev/null || true
" || true

# ── Configure ─────────────────────────────────────────────────────────────────
echo "Configuring kernel…"
orb run -m "$VM" bash -c "
    cd '$KERNEL_DIR'
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
         CC=clang LD=ld.lld LLVM=1 LLVM_IAS=1 \
         rosemary_defconfig
    scripts/config --enable  NO_HZ_FULL
    scripts/config --enable  HZ_100
    scripts/config --disable BT
    scripts/config --disable WLAN
    scripts/config --disable MTK_MODEM_CCCI_SUPPORT
    scripts/config --enable  RFKILL
    scripts/config --enable  CPU_FREQ_GOV_POWERSAVE
    scripts/config --enable  MLOCK_MLOCKALL
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
         CC=clang LD=ld.lld LLVM=1 LLVM_IAS=1 olddefconfig
"
ok "Kernel configured"

# ── Build ─────────────────────────────────────────────────────────────────────
NCPUS=$(orb run -m "$VM" nproc 2>/dev/null || echo 4)
echo "Building kernel (${NCPUS} jobs — expect 20-40 min)…"
orb run -m "$VM" bash -c "
    cd '$KERNEL_DIR'
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
         CC=clang LD=ld.lld LLVM=1 LLVM_IAS=1 \
         -j${NCPUS} Image.gz-dtb
"
KIMAGE="$KERNEL_DIR/arch/arm64/boot/Image.gz-dtb"
[ -f "$KIMAGE" ] || die "Build failed — Image.gz-dtb not found"
ok "Kernel image: $KIMAGE ($(du -sh "$KIMAGE" | cut -f1))"

# ── Optional QEMU smoke test ──────────────────────────────────────────────────
if [ "${1:-}" = "--qemu" ]; then
    command -v qemu-system-aarch64 >/dev/null \
        || { echo "Install: brew install qemu"; exit 1; }
    echo "Booting kernel in QEMU (Ctrl-A X to quit)…"
    qemu-system-aarch64 \
        -M virt \
        -cpu cortex-a55 \
        -m 512M \
        -kernel "$KERNEL_DIR/arch/arm64/boot/Image.gz" \
        -append "console=ttyAMA0 earlycon panic=5" \
        -nographic \
        -no-reboot
fi

echo ""
echo "Flash the kernel:"
echo "   ./build/flash.sh --kernel"
