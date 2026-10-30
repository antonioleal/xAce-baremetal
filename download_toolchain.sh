#!/usr/bin/env bash

# Toolchain downloader script for xAce-baremetal

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

set -euo pipefail

# Script directory is the project root
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLCHAIN_DIR="${PROJECT_ROOT}/toolchain"
VERSION="14.2.rel1"
BASE_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${VERSION}/binrel"

# Detect OS and Architecture
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "${OS}" in
    linux)
        case "${ARCH}" in
            x86_64|amd64)
                TARBALL="arm-gnu-toolchain-${VERSION}-x86_64-arm-none-eabi.tar.xz"
                ;;
            aarch64|arm64)
                TARBALL="arm-gnu-toolchain-${VERSION}-aarch64-arm-none-eabi.tar.xz"
                ;;
            *)
                echo "Unsupported Linux architecture: ${ARCH}" >&2
                exit 1
                ;;
        esac
        ;;
    darwin)
        case "${ARCH}" in
            arm64|aarch64)
                TARBALL="arm-gnu-toolchain-${VERSION}-darwin-arm64-arm-none-eabi.tar.xz"
                ;;
            x86_64)
                TARBALL="arm-gnu-toolchain-${VERSION}-darwin-x86_64-arm-none-eabi.tar.xz"
                ;;
            *)
                echo "Unsupported macOS architecture: ${ARCH}" >&2
                exit 1
                ;;
        esac
        ;;
    *)
        echo "Unsupported operating system: ${OS}" >&2
        exit 1
        ;;
esac

DOWNLOAD_URL="${BASE_URL}/${TARBALL}"

echo "=========================================================="
echo " Arm GNU Toolchain Downloader"
echo " Target directory: ${TOOLCHAIN_DIR}"
echo " Package:          ${TARBALL}"
echo " Download URL:     ${DOWNLOAD_URL}"
echo "=========================================================="

if [ -x "${TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc" ] && [ "${1:-}" != "--force" ]; then
    echo "Toolchain already installed at ${TOOLCHAIN_DIR}."
    "${TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc" --version | head -n 1
    echo "Pass '--force' to force re-download and reinstallation."
    exit 0
fi

# Ensure prerequisites
for cmd in tar xz; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Error: required tool '$cmd' is not installed." >&2
        exit 1
    fi
done

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TEMP_DIR}"' EXIT

ARCHIVE_PATH="${TEMP_DIR}/${TARBALL}"

echo "Downloading ${TARBALL}..."
if command -v curl >/dev/null 2>&1; then
    curl -fL --progress-bar "${DOWNLOAD_URL}" -o "${ARCHIVE_PATH}"
elif command -v wget >/dev/null 2>&1; then
    wget --show-progress -O "${ARCHIVE_PATH}" "${DOWNLOAD_URL}"
else
    echo "Error: neither curl nor wget was found." >&2
    exit 1
fi

echo "Extracting into ${TOOLCHAIN_DIR}..."
rm -rf "${TOOLCHAIN_DIR}"
mkdir -p "${TOOLCHAIN_DIR}"
tar -xf "${ARCHIVE_PATH}" -C "${TOOLCHAIN_DIR}" --strip-components=1

# Verify installation
if [ -x "${TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc" ]; then
    echo "Successfully installed toolchain:"
    "${TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc" --version | head -n 2
else
    echo "Error: installation verification failed." >&2
    exit 1
fi

# Ensure circle/Config.mk exists and uses dynamic toolchain detection
CIRCLE_CONFIG="${PROJECT_ROOT}/circle/Config.mk"
if [ ! -f "${CIRCLE_CONFIG}" ]; then
    cat << 'EOF' > "${CIRCLE_CONFIG}"
# Determine paths relative to this config file (independent of folder name/location)
_CONFIG_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
_LOCAL_TOOLCHAIN := $(abspath $(_CONFIG_DIR)../toolchain/bin)/arm-none-eabi-

# Prefer local project toolchain if present, otherwise fall back to system toolchain
ifeq ($(wildcard $(abspath $(_CONFIG_DIR)../toolchain/bin/arm-none-eabi-gcc)),)
  PREFIX ?= arm-none-eabi-
else
  PREFIX ?= $(_LOCAL_TOOLCHAIN)
endif

AARCH ?= 32
RASPPI ?= 4
EOF
    echo "Created dynamic ${CIRCLE_CONFIG}."
fi

echo "Done!"
