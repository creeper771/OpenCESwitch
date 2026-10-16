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
# Mesa built with its GL thread (port/switch/mesa/build_mesa.sh), when there,
# before the system's switch-mesa
MESA    := $(OUT)/mesa/install$(DEVKITPRO)/portlibs/switch
MESA_FLAGS := $(if $(wildcard $(MESA)/lib/libEGL.a),-I$(MESA)/include,)
MESA_LIBS := $(if $(wildcard $(MESA)/lib/libEGL.a),-L$(MESA)/lib,)
TOOLS   := $(DEVKITPRO)/tools/bin
CC      := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
# (the link is C++'s: Mesa's shader compiler needs its runtime)
CXX     := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -O2 -std=gnu11 -Wall -Wextra -ffunction-sections $(ARCH) -D__SWITCH__ \
	-I. -I$(LIBNX)/include $(MESA_FLAGS) -I$(PORTLIBS)/include -I$(ROOT)/port/android/include

LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH) $(MESA_LIBS) -L$(PORTLIBS)/lib -L$(LIBNX)/lib
# switch-mesa: EGL and OpenGL ES on the nouveau driver (zlib for its shader
# cache)
LIBS    := -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lz -lnx

# internet play's UPnP and DTLS, as the Android host has them
# (tools/android_build.py): port/third_party's miniupnpc and mbedtls
MINIUPNPC := $(ROOT)/port/third_party/miniupnpc
MBEDTLS := $(ROOT)/port/third_party/mbedtls
MINIUPNPC_SOURCES := $(notdir $(wildcard $(MINIUPNPC)/src/*.c))
MBEDTLS_SOURCES := $(notdir $(wildcard $(MBEDTLS)/library/*.c))

SOURCES := host_main.c host_memory.c host_loader.c host_thread.c host_syscall.c host_imports.c host_audio.c host_gl.c host_profile.c host_network.c host_input.c host_clock.c \
	host_watch_hash.c posix_files.c posix_net.c posix_upnp.c posix_dtls.c \
	$(MINIUPNPC_SOURCES) $(MBEDTLS_SOURCES)
ASSEMBLY := host_stack.s host_stubs.s
OBJECTS := $(addprefix $(OUT)/obj/,$(SOURCES:.c=.o) $(ASSEMBLY:.s=.o))

vpath %.c . $(ROOT)/port/android/host $(ROOT)/port/linux/src $(MINIUPNPC)/src $(MBEDTLS)/library

# posix_*.c's functions are the guest's hostposix_* imports
$(OUT)/obj/posix_files.o: CFLAGS += -include host_posix.h -I$(ROOT)/port/linux/src
$(OUT)/obj/posix_net.o: CFLAGS += -I$(ROOT)/port/linux/src
# (compat/: what libnx's headers do not have that they need)
MINIUPNPC_FLAGS := -I$(MINIUPNPC)/include -I$(MINIUPNPC)/src -DMINIUPNP_STATICLIB -DMINIUPNPC_SET_SOCKET_TIMEOUT \
	-DMINIUPNPC_GET_SRC_ADDR -D_BSD_SOURCE -D_DEFAULT_SOURCE -Icompat -include compat/miniupnpc_switch.h
MBEDTLS_FLAGS := -I$(MBEDTLS)/include -I$(MBEDTLS)/library -DMBEDTLS_USER_CONFIG_FILE='"compat/mbedtls_switch_config.h"' 
$(OUT)/obj/posix_upnp.o: CFLAGS += -I$(ROOT)/port/linux/src $(MINIUPNPC_FLAGS)
$(addprefix $(OUT)/obj/,$(MINIUPNPC_SOURCES:.c=.o)): CFLAGS += $(MINIUPNPC_FLAGS) -w
$(OUT)/obj/posix_dtls.o: CFLAGS += -I$(ROOT)/port/linux/src $(MBEDTLS_FLAGS)
$(addprefix $(OUT)/obj/,$(MBEDTLS_SOURCES:.c=.o)): CFLAGS += $(MBEDTLS_FLAGS) -w

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

# internet play's MQTT brokers (network.brokers_file), which the host writes
# into the data folder at each start, as the Android app does
$(OUT)/romfs/brokers.txt: $(ROOT)/port/assets/network/brokers.txt | $(OUT)/romfs
	cp $< $@

$(OUT)/opence.nacp:
	$(TOOLS)/nacptool --create "OpenCE" "OpenCE" "0.1.0" $@

$(OUT)/opence.nro: $(OUT)/opence.elf $(OUT)/opence.nacp $(OUT)/romfs/halo_guest.elf $(OUT)/romfs/brokers.txt
	$(TOOLS)/elf2nro $< $@ --nacp=$(OUT)/opence.nacp --icon=$(LIBNX)/default_icon.jpg --romfsdir=$(OUT)/romfs

clean:
	rm -rf $(OUT)
