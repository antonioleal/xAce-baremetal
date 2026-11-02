#
# Makefile for xAce Baremetal on Raspberry Pi 4 / 400 using Circle
#

CIRCLEHOME = ../circle

OBJS = main.o kernel.o keyboard_circle.o keyboard_layouts.o ram_ace.o sound_ace.o spool_ace.o save_dictionary.o line_history.o tape_sd.o web_files.o wifi_settings.o zip_sd.o puff.o update_sd.o z80.o

LIBS = $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
       $(CIRCLEHOME)/addon/wlan/libwlan.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/sound/libsound.a \
       $(CIRCLEHOME)/lib/net/libnet.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

z80.o: CFLAGS += -Wno-parentheses

# Refresh the ready-to-copy SD card folder at the project root
# Circle gives the kernel (code, data and variables) KERNEL_MAX_SIZE (2 MB) from
# 0x8000, with the startup stack right after: a bigger kernel crashes before the
# screen comes up (rainbow screen), so refuse to copy one
KERNEL_END_MAX = 0x208000

sdcard: $(TARGET).img
	@END=$$($(PREFIX)nm $(TARGET).elf | awk '$$3 == "_end" { print $$1 }'); \
	if [ -z "$$END" ] || [ $$((0x$$END)) -gt $$(($(KERNEL_END_MAX))) ]; then \
		echo "The kernel ends at 0x$$END, past $(KERNEL_END_MAX): it would not boot" >&2; \
		exit 1; \
	fi; \
	echo "  SIZE  kernel ends at 0x$$END (limit $(KERNEL_END_MAX))"
	cp $(TARGET).img config.txt ../sdcard/

.PHONY: sdcard

# Dependency files record toolchain headers by absolute path. If the project
# folder is renamed or moved, treat those vanished headers as changed so the
# objects (and their .d files) are rebuilt instead of make stopping.
%.h:
	@:

-include $(DEPS)

# update_sd.cpp holds the build marker (__DATE__ __TIME__): build it every time
update_sd.o: FORCE
FORCE:
.PHONY: FORCE
