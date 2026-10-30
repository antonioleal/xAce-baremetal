#
# Makefile for xAce Baremetal on Raspberry Pi 4 / 400 using Circle
#

CIRCLEHOME = ../circle

OBJS = main.o kernel.o keyboard_circle.o keyboard_layouts.o ram_ace.o sound_ace.o spool_ace.o save_dictionary.o line_history.o tape_sd.o z80.o

LIBS = $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/sound/libsound.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

z80.o: CFLAGS += -Wno-parentheses

# Refresh the ready-to-copy SD card folder at the project root
sdcard: $(TARGET).img
	cp $(TARGET).img config.txt ../sdcard/

.PHONY: sdcard

# Dependency files record toolchain headers by absolute path. If the project
# folder is renamed or moved, treat those vanished headers as changed so the
# objects (and their .d files) are rebuilt instead of make stopping.
%.h:
	@:

-include $(DEPS)
