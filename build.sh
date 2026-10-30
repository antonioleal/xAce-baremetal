#!/bin/bash

# Build script for xAce-baremetal

# Copyright 2026 Antonio Leal, Porto Salvo, Oeiras, Portugal
# All rights reserved.
#
# Redistribution and use of this script, with or without modification, is
# permitted provided that the following conditions are met:
#
# 1. Redistributions of this script must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
#  THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
#  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
#  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
#  EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
#  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
#  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
#  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
#  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
#  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Build the Circle libraries and the kernel for a Raspberry Pi model, then fill
# sdcard/ with the model's boot firmware (from firmware/piN), the kernel image and
# config.txt
#
# Usage: ./build.sh [model]
#   1    Raspberry Pi 1 / Zero / Zero W        (kernel.img)
#   2    Raspberry Pi 2                        (kernel7.img)
#   3    Raspberry Pi 3 / Zero 2 W             (kernel8-32.img)
#   4    Raspberry Pi 4 / 400 (the default)    (kernel7l.img)

set -e

usage() {
    sed -n '/^# Usage:/,/^set -e/s/^# \{0,1\}//p' "$0"
}

MODEL="${1:-4}"
case "$MODEL" in
    1|2|3|4)
        ;;
    400)
        MODEL=4
        ;;
    5)
        echo "The Raspberry Pi 5 is not supported yet (it needs a 64-bit build and code changes)" >&2
        exit 1
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown Raspberry Pi model: $1" >&2
        usage >&2
        exit 1
        ;;
esac

# Overrides RASPPI in circle/Config.mk
export RASPPI="$MODEL"

cd "$(dirname "$0")"

# Circle's Config.mk is not part of the Circle repository; restore the project's copy
if [ ! -f circle/Config.mk ]; then
    cp circle-config/Config.mk circle/Config.mk
fi

# Circle and the kernel are built for one model at a time: start again when it changes
LAST_MODEL="$(cat .build-model 2>/dev/null || true)"
if [ "$LAST_MODEL" != "$MODEL" ]; then
    echo "Building for Raspberry Pi $MODEL (previous build: ${LAST_MODEL:-unknown}), cleaning first"
    (cd circle && ./makeall clean && make -C addon/SDCard clean && make -C addon/fatfs clean)
    make -C src clean
    echo "$MODEL" > .build-model
fi

# Circle libraries (only rebuilds what changed)
cd circle
./makeall
make -C addon/SDCard
make -C addon/fatfs
cd ..

make -C src

# sdcard/ holds the files for one model only: start again from an empty folder
if [ ! -d "firmware/pi$MODEL" ]; then
    echo "Missing firmware/pi$MODEL: the boot firmware for the Raspberry Pi $MODEL" >&2
    exit 1
fi
find sdcard -mindepth 1 ! -name .gitkeep -delete
cp firmware/pi$MODEL/* sdcard/
make -C src sdcard
