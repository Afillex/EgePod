# Cross-compile for AArch64 Android using NDK r26+.
# Run: export ANDROID_NDK_HOME=/path/to/ndk-r26
#      make

NDK    ?= $(ANDROID_NDK_HOME)
HOST   := $(shell uname -s)-$(shell uname -m)

ifeq ($(HOST),Darwin-arm64)
    TC_DIR := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64
else
    TC_DIR := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
endif

API    ?= 34   # Android 14 matches the rosemary vendor tree

CC     := $(TC_DIR)/bin/aarch64-linux-android$(API)-clang
CXX    := $(TC_DIR)/bin/aarch64-linux-android$(API)-clang++
AR     := $(TC_DIR)/bin/llvm-ar
STRIP  := $(TC_DIR)/bin/llvm-strip

# ── compiler flags ──────────────────────────────────────────────────────────
# Tuned for Cortex-A55 (the cores we keep online).
CFLAGS  := -O2 \
           -march=armv8.2-a \
           -mtune=cortex-a55 \
           -flto=thin \
           -ffunction-sections \
           -fdata-sections \
           -std=c11 \
           -Wall -Wextra -Wno-unused-parameter \
           -D_GNU_SOURCE \
           -I$(CURDIR)/src \
           -I$(CURDIR)/vendor \
           -I$(CURDIR)/vendor/tinyalsa/include

LDFLAGS := -flto=thin \
           -Wl,--gc-sections \
           -Wl,-s \
           -L$(CURDIR)/vendor/tinyalsa \
           -ltinyalsa \
           -lpthread
