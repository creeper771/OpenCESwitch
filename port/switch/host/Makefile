# Builds the Switch host, build/switch/opence.nro, with devkitA64 and libnx.
# Its RomFS holds the Android port's guest image: build that first with
#     ninja build/android/halo_guest.elf
# (refer to port/switch/README.md).

ifeq ($(strip $(DEVKITPRO)),)
$(error set DEVKITPRO, for example: export DEVKITPRO=/opt/devkitpro)
endif

ROOT    := ../../..
OUT     := $(ROOT)/build/switch
GUEST   := $(ROOT)/build/android/halo_guest.elf

LIBNX   ?= $(DEVKITPRO)/libnx
PORTLIBS ?= $(DEVKITPRO)/portlibs/switch
TOOLS   := $(DEVKITPRO)/tools/bin
CC      := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
# (the link is C++'s: Mesa's shader compiler needs its runtime)
CXX     := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -O2 -std=gnu11 -Wall -Wextra -ffunction-sections $(ARCH) -D__SWITCH__ \
	-I. -I$(LIBNX)/include -I$(PORTLIBS)/include -I$(ROOT)/port/android/include

LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH) -L$(PORTLIBS)/lib -L$(LIBNX)/lib
# switch-mesa: EGL and OpenGL ES on the nouveau driver
LIBS    := -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx

SOURCES := host_main.c host_memory.c host_loader.c host_thread.c host_syscall.c host_imports.c host_audio.c host_gl.c host_profile.c host_network.c host_input.c \
	host_watch_hash.c posix_files.c
ASSEMBLY := host_stack.s host_stubs.s
OBJECTS := $(addprefix $(OUT)/obj/,$(SOURCES:.c=.o) $(ASSEMBLY:.s=.o))

vpath %.c . $(ROOT)/port/android/host $(ROOT)/port/linux/src

# posix_files.c's file functions are the guest's hostposix_* imports
$(OUT)/obj/posix_files.o: CFLAGS += -include host_posix.h -I$(ROOT)/port/linux/src

.PHONY: all clean

all: $(OUT)/opence.nro

$(OUT)/obj $(OUT)/romfs:
	mkdir -p $@

$(OUT)/obj/%.o: %.c host.h host_posix.h | $(OUT)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/%.o: %.s | $(OUT)/obj
	$(CC) $(ARCH) -c $< -o $@

$(OUT)/opence.elf: $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $@

$(GUEST):
	@echo "build the guest image first: ninja build/android/halo_guest.elf" && false

$(OUT)/romfs/halo_guest.elf: $(GUEST) | $(OUT)/romfs
	cp $< $@

$(OUT)/opence.nacp:
	$(TOOLS)/nacptool --create "OpenCE" "OpenCE" "0.1.0" $@

$(OUT)/opence.nro: $(OUT)/opence.elf $(OUT)/opence.nacp $(OUT)/romfs/halo_guest.elf
	$(TOOLS)/elf2nro $< $@ --nacp=$(OUT)/opence.nacp --icon=$(LIBNX)/default_icon.jpg --romfsdir=$(OUT)/romfs

clean:
	rm -rf $(OUT)
