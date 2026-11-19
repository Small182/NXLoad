ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := NXLoad
APP_TITLE := NXLoad
APP_AUTHOR := Small182
APP_VERSION := 1.0.40
NXLOAD_BUILD_SHA ?= $(shell git -C "$(TOPDIR)" rev-parse HEAD 2>/dev/null || printf '%s' "$${GITHUB_SHA:-unknown}")
NXLOAD_BUILD_RUN ?= $(if $(filter push,$(GITHUB_EVENT_NAME)),$(GITHUB_RUN_NUMBER))
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__ -DCURL_STATICLIB -DNXLOAD_VERSION=\"$(APP_VERSION)\" -DNXLOAD_BUILD_SHA=\"$(NXLOAD_BUILD_SHA)\" -DNXLOAD_BUILD_RUN=\"$(NXLOAD_BUILD_RUN)\"
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH)
PKG_CONFIG := $(PORTLIBS)/bin/aarch64-none-elf-pkg-config
PORT_PACKAGES := sdl2 SDL2_ttf SDL2_image libxml-2.0 libarchive
PORT_LIBS := $(shell $(PKG_CONFIG) --static --libs $(PORT_PACKAGES))
TITLEDB := $(TOPDIR)/romfs/titledb.bin
CATALOG_META := $(TOPDIR)/romfs/catalog-meta.tsv
ROMSHQ_SEED := $(TOPDIR)/romfs/romshq.tsv.gz
GALLERIES := $(TOPDIR)/romfs/gallery/.ready
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
all: $(TITLEDB) $(CATALOG_META) $(GALLERIES)
	@mkdir -p build
	@$(MAKE) --no-print-directory -C build -f $(TOPDIR)/Makefile

$(TITLEDB) $(CATALOG_META) &: $(TOPDIR)/tools/build-metadata.py $(TOPDIR)/tools/title_db.py $(TOPDIR)/tools/catalog_metadata.py $(TOPDIR)/tools/nintendo-life.json.gz $(TOPDIR)/tools/nintendo-life-ratings.json
	python3 "$<" "$(TITLEDB)" --catalog-output "$(CATALOG_META)" --nintendo-life "$(TOPDIR)/tools/nintendo-life.json.gz" --nintendo-life-ratings "$(TOPDIR)/tools/nintendo-life-ratings.json"

$(GALLERIES): $(TOPDIR)/tools/build-galleries.py
	python3 $< $@

clean:
	rm -rf build $(TARGET).elf $(TARGET).nacp $(TARGET).nro
else
OFILES := main.o catalog.o game_info.o cover_cache.o fichier.o unpack.o install.o updater.o settings.o language.o range.o tls_platform.o
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(TITLEDB) $(CATALOG_META) $(GALLERIES) $(ROMSHQ_SEED)
$(OUTPUT).elf: $(OFILES)
-include $(addprefix $(DEPSDIR)/,$(OFILES:.o=.d))
endif
