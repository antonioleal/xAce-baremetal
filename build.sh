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
# sdcard/ with the model's boot firmware (from firmware/piN), its Wi-Fi firmware
# (from firmware/wlan, into sdcard/firmware), the kernel image and config.txt, and
# zip sdcard/ into dist/xAce-baremetal-<version>-pi<model>.zip (the version from
# src/version.h): the release zip of the model, which the system update page (F11)
# installs. dist/ is emptied first, so it has the zips of this run only.
#
# With --all, every model is built (the Pi 4 last, so sdcard/ is left with it) and
# dist/ gets the four zips. With --publish (and --all), the GitHub release
# v<version> is also created on the current commit, which must be committed and
# pushed, with these zips (GitHub adds the source code zip and tar.gz itself) and the
# <version> section of CHANGELOG.md as its notes.
#
# Usage: ./build.sh [model]
#        ./build.sh --all [--publish]
#   1    Raspberry Pi 1 / Zero / Zero W        (kernel.img)
#   2    Raspberry Pi 2                        (kernel7.img)
#   3    Raspberry Pi 3 / Zero 2 W             (kernel8-32.img)
#   4    Raspberry Pi 4 / 400 (the default)    (kernel7l.img)

set -e

usage() {
    sed -n '/^# Usage:/,/^set -e/s/^# \{0,1\}//p' "$0"
}

MODELS=""
ALL=0
PUBLISH=0
for ARG in "$@"; do
    case "$ARG" in
        1|2|3|4)    MODELS="$ARG" ;;
        400)        MODELS=4 ;;
        5)
            echo "The Raspberry Pi 5 is not supported yet (it needs a 64-bit build and code changes)" >&2
            exit 1
            ;;
        --all)      ALL=1 ;;
        --publish)  PUBLISH=1 ;;
        -h|--help)  usage; exit 0 ;;
        *)
            echo "Unknown option or Raspberry Pi model: $ARG" >&2
            usage >&2
            exit 1
            ;;
    esac
done
if [ $ALL = 1 ] && [ -n "$MODELS" ]; then
    echo "Give a model or --all, not both" >&2
    exit 1
fi
if [ $PUBLISH = 1 ] && [ $ALL = 0 ]; then
    echo "--publish needs --all: a release has the zips of every model" >&2
    exit 1
fi
if [ $ALL = 1 ]; then
    MODELS="1 2 3 4"                    # the Pi 4 last, so sdcard/ is left with it
fi
MODELS="${MODELS:-4}"

cd "$(dirname "$0")"

VERSION="$(sed -n 's/^#define XACE_VERSION *"\(.*\)".*/\1/p' src/version.h)"
if [ -z "$VERSION" ]; then
    echo "No XACE_VERSION in src/version.h" >&2
    exit 1
fi
NAME="xAce-baremetal-$VERSION"

if [ $PUBLISH = 1 ]; then
    if [ -n "$(git status --porcelain)" ]; then
        echo "The working tree has uncommitted changes: commit them first" >&2
        exit 1
    fi
    git fetch -q origin
    if [ "$(git rev-parse HEAD)" != "$(git rev-parse @{u})" ]; then
        echo "HEAD is not the pushed commit: push first" >&2
        exit 1
    fi
    if gh release view "v$VERSION" >/dev/null 2>&1; then
        echo "The release v$VERSION exists already: change XACE_VERSION in src/version.h" >&2
        exit 1
    fi
fi

# dist/ has the zips of this run only
mkdir -p dist
find dist -mindepth 1 -delete

build_model() {
    MODEL="$1"

    # Overrides RASPPI in circle/Config.mk
    export RASPPI="$MODEL"

    # Circle's Config.mk is not part of the Circle repository; restore the project's copy
    if [ ! -f circle/Config.mk ]; then
        cp circle-config/Config.mk circle/Config.mk
    fi

    # Circle and the kernel are built for one model at a time: start again when it changes
    LAST_MODEL="$(cat .build-model 2>/dev/null || true)"
    if [ "$LAST_MODEL" != "$MODEL" ]; then
        echo "Building for Raspberry Pi $MODEL (previous build: ${LAST_MODEL:-unknown}), cleaning first"
        (cd circle && ./makeall clean && make -C addon/SDCard clean && make -C addon/fatfs clean \
            && make -C addon/wlan clean && make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle clean)
        make -C src clean
        echo "$MODEL" > .build-model
    fi

    # Circle libraries (only rebuilds what changed)
    cd circle
    ./makeall
    make -C addon/SDCard
    make -C addon/fatfs
    make -C addon/wlan
    make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle
    cd ..

    make -C src

    # sdcard/ holds the files for one model only: start again from an empty folder
    if [ ! -d "firmware/pi$MODEL" ]; then
        echo "Missing firmware/pi$MODEL: the boot firmware for the Raspberry Pi $MODEL" >&2
        exit 1
    fi
    find sdcard -mindepth 1 ! -name .gitkeep -delete
    cp firmware/pi$MODEL/* sdcard/

    # Wi-Fi firmware for the WLAN chips of the model (F11): Zero W 43430; Pi 3 43430,
    # Pi 3 B+ / A+ 43455, Zero 2 W 43436; Pi 4 43455, Pi 400 43456. The Pi 2 has none.
    case "$MODEL" in
        1) WLAN_CHIPS="43430" ;;
        3) WLAN_CHIPS="43430 43436 43455" ;;
        4) WLAN_CHIPS="43455 43456" ;;
        *) WLAN_CHIPS="" ;;
    esac
    if [ -n "$WLAN_CHIPS" ]; then
        mkdir -p sdcard/firmware
        for CHIP in $WLAN_CHIPS; do
            cp firmware/wlan/brcmfmac$CHIP-sdio.* sdcard/firmware/
        done
        cp firmware/wlan/LICENCE.wlan sdcard/firmware/
    fi
    make -C src sdcard

    # The release zip of the model
    (cd sdcard && zip -q -X -r "../dist/$NAME-pi$MODEL.zip" $(ls))
    echo "dist/$NAME-pi$MODEL.zip"
}

for MODEL in $MODELS; do
    build_model "$MODEL"
done

if [ $PUBLISH = 1 ]; then
    # The <version> section of CHANGELOG.md, without its heading
    NOTES="$(mktemp)"
    trap 'rm -f "$NOTES"' EXIT
    {
        echo "Jupiter Ace emulator running baremetal on the Raspberry Pi."
        echo
        echo "## SD card"
        echo
        echo "Unzip the file for your Raspberry Pi into the root of a FAT32 micro-SD card, add"
        echo "your .tap, .tzx and .ace files and boot. pi4 is also for the Pi 400 (add a"
        echo "cmdline.txt containing sounddev=sndhdmi for sound), pi3 for the Zero 2 W, pi1 for"
        echo "the Zero and Zero W."
        echo
        echo "## Changes"
        echo
        awk -v v="## $VERSION " 'index($0, v) == 1 { on = 1; next } /^## / { on = 0 } on' CHANGELOG.md
    } > "$NOTES"

    gh release create "v$VERSION" dist/"$NAME"-pi*.zip --target "$(git rev-parse HEAD)" \
        --title "xAce $VERSION" --notes-file "$NOTES"
fi
