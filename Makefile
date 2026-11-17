ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := NXLoad
APP_TITLE := NXLoad
APP_AUTHOR := Small182
APP_VERSION := 1.0.4
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__ -DCURL_STATICLIB
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH)
PKG_CONFIG := $(PORTLIBS)/bin/aarch64-none-elf-pkg-config
PORT_PACKAGES := sdl2 SDL2_ttf SDL2_image libxml-2.0
PORT_LIBS := $(shell $(PKG_CONFIG) --static --libs $(PORT_PACKAGES))
NXLOAD_NETWORK := $(TOPDIR)/.nxdeps/install
NETWORK_LIBS := $(addprefix $(NXLOAD_NETWORK)/lib/,libcurl.a libmbedtls.a libmbedx509.a libmbedcrypto.a)
LIBS := $(NETWORK_LIBS) $(PORT_LIBS) -lz -lnx -lm

ifeq ($(TOPDIR),$(CURDIR))
export TOPDIR
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/build
export INCLUDE := -I$(NXLOAD_NETWORK)/include -I$(PORTLIBS)/include -I$(LIBNX)/include -I$(PORTLIBS)/include/libxml2
export LIBPATHS := -L$(PORTLIBS)/lib -L$(LIBNX)/lib
export NROFLAGS := --icon=$(TOPDIR)/icon.jpg --nacp=$(OUTPUT).nacp --romfsdir=$(TOPDIR)/romfs
export LD := $(CC)

.PHONY: all clean
all:
	@mkdir -p build
	@$(MAKE) --no-print-directory -C build -f $(TOPDIR)/Makefile

clean:
	rm -rf build $(TARGET).elf $(TARGET).nacp $(TARGET).nro
else
OFILES := main.o catalog.o fichier.o range.o tls_platform.o
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(addprefix $(DEPSDIR)/,$(OFILES:.o=.d))
endif
