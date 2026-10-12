# Builds opence_memory_probe.nro with devkitA64 and libnx (refer to README.md).

ifeq ($(strip $(DEVKITPRO)),)
$(error set DEVKITPRO, for example: export DEVKITPRO=/opt/devkitpro)
endif

TARGET  := opence_memory_probe
LIBNX   ?= $(DEVKITPRO)/libnx
TOOLS   := $(DEVKITPRO)/tools/bin
CC      := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -O2 -Wall -Wextra -ffunction-sections $(ARCH) -D__SWITCH__ -I$(LIBNX)/include -I../../../android/include
LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH) -L$(LIBNX)/lib
LIBS    := -lnx

.PHONY: all clean

all: $(TARGET).nro

$(TARGET).elf: memory_probe.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

$(TARGET).nacp:
	$(TOOLS)/nacptool --create "OpenCE memory probe" "OpenCE" "1.0.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	$(TOOLS)/elf2nro $< $@ --nacp=$(TARGET).nacp --icon=$(LIBNX)/default_icon.jpg

clean:
	rm -f $(TARGET).elf $(TARGET).nacp $(TARGET).nro
