#!/bin/bash

# Release script for xAce-baremetal

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
# Build the SD card files for every Raspberry Pi model and zip them, one zip per
# model, into dist/: xAce-baremetal-<version>-pi1.zip ... -pi4.zip. With
# --publish, also create the GitHub release v<version> on the current commit
# with these zips (GitHub adds the source code zip and tar.gz itself) and the
# <version> section of CHANGELOG.md as its notes.
#
# Usage: ./release.sh <version> [--publish]
#   e.g. ./release.sh 0.6
set -e

usage() {
    sed -n '/^# Usage:/,/^set -e/s/^# \{0,1\}//p' "$0"
}

VERSION="$1"
PUBLISH=0
if [ -z "$VERSION" ] || [[ "$VERSION" == -* ]]; then
    usage >&2
    exit 1
fi
case "$2" in
    "")         ;;
    --publish)  PUBLISH=1 ;;
    *)          usage >&2; exit 1 ;;
esac

cd "$(dirname "$0")"

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
fi

NAME="xAce-baremetal-$VERSION"
mkdir -p dist
rm -f dist/"$NAME"-pi*.zip

# Pi 4 last, so sdcard/ is left as the default build
for MODEL in 1 2 3 4; do
    ./build.sh "$MODEL"
    (cd sdcard && zip -q -X "../dist/$NAME-pi$MODEL.zip" $(ls))
    echo "dist/$NAME-pi$MODEL.zip"
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
