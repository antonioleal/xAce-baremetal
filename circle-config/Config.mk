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

