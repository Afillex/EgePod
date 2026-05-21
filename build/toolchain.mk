# Cross-compile for AArch64 Android using NDK r26+.
# Run: export ANDROID_NDK_HOME=/path/to/ndk-r26
#      make

NDK    ?= $(ANDROID_NDK_HOME)
OS     := $(shell uname -s)

# NDK r23c+ ships native darwin-arm64 host tools; older NDK ships only x86_64.
# Both work on Apple Silicon — darwin-arm64 is faster (no Rosetta overhead).
ifeq ($(OS),Darwin)
    ifneq ($(wildcard $(NDK)/toolchains/llvm/prebuilt/darwin-arm64),)
        TC_DIR := $(NDK)/toolchains/llvm/prebuilt/darwin-arm64
    else
        TC_DIR := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64
    endif
else
    TC_DIR := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
endif

API    ?= 34   # Android 14 matches the rosemary vendor tree

CC     := $(TC_DIR)/bin/aarch64-linux-android$(API)-clang
CXX    := $(TC_DIR)/bin/aarch64-linux-android$(API)-clang++
AR     := $(TC_DIR)/bin/llvm-ar
STRIP  := $(TC_DIR)/bin/llvm-strip

# ── compiler flags ─────────────────────────────────────────────────────────────
# Tuned for Cortex-A55 (the only cores kept online in DAP mode).
CFLAGS  := -O2 \
           -march=armv8.2-a \
           -mtune=cortex-a55 \
           -flto=thin \
           -ffunction-sections \
           -fdata-sections \
           -std=c11 \
           -Wall -Wextra -Wno-unused-parameter \
           -D_GNU_SOURCE \
           -DMUSIC_DIR=\"/sdcard/Music\" \
           -DEGEPOD_READY_DIR=\"/dev\" \
           -I$(CURDIR)/src \
           -I$(CURDIR)/vendor \
           -I$(CURDIR)/vendor/tinyalsa/include

# tinyalsa is linked as a static archive built in $(OUT)/tinyalsa/;
# each daemon target adds it explicitly — no -ltinyalsa in shared LDFLAGS.
LDFLAGS := -flto=thin \
           -Wl,--gc-sections \
           -Wl,-s \
           -lpthread
