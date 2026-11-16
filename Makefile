ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := Switch-Download-Probe
APP_TITLE := Switch Download Probe
APP_AUTHOR := Homebrew
APP_VERSION := 0.2.0
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__ -DCURL_STATICLIB
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH)
LIBS := -lcurl -lz -lnx

ifeq ($(TOPDIR),$(CURDIR))
export TOPDIR
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/build
export INCLUDE := -I$(PORTLIBS)/include -I$(LIBNX)/include
export LIBPATHS := -L$(PORTLIBS)/lib -L$(LIBNX)/lib
export NROFLAGS := --icon=$(LIBNX)/default_icon.jpg --nacp=$(OUTPUT).nacp
export LD := $(CC)

.PHONY: all clean
all:
	@mkdir -p build
	@$(MAKE) --no-print-directory -C build -f $(TOPDIR)/Makefile

clean:
	rm -rf build $(TARGET).elf $(TARGET).nacp $(TARGET).nro
else
OFILES := main.o
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(DEPSDIR)/main.d
endif
