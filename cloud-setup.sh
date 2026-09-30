#!/usr/bin/env bash
# Cloud environment setup for building/testing picolog (and test/uart_source).
# Reproduces what was installed by hand in the session. Idempotent; run as root.
set -euo pipefail

PICO_SDK_VERSION=2.3.1                 # README: pico-sdk 2.3.1 with TinyUSB submodule
PICO_SDK_PATH=${PICO_SDK_PATH:-/root/pico-sdk}

# Arm cross toolchain (+ newlib, libstdc++ for the SDK) and ninja
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
    ninja-build cmake build-essential

# pico-sdk at the pinned tag, with only the TinyUSB submodule
if [ ! -d "$PICO_SDK_PATH/.git" ]; then
    git clone --depth 1 --branch "$PICO_SDK_VERSION" \
        https://github.com/raspberrypi/pico-sdk.git "$PICO_SDK_PATH"
fi
git -C "$PICO_SDK_PATH" submodule update --init --depth 1 lib/tinyusb

# Make PICO_SDK_PATH available to later shells (best effort)
echo "export PICO_SDK_PATH=$PICO_SDK_PATH" > /etc/profile.d/pico-sdk.sh

# Optional: Python deps for the hardware/simulator pytest suite (test/hw)
# pip install -r test/hw/requirements.txt    # pyserial, pytest
