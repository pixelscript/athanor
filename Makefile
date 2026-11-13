# =============================================================================
# Athanor — performance end-of-chain effect for Hermetic Modular Alchemy Lab
#
# Built on alchemy-template (standard Daisy workflow, libDaisy core Makefile):
#   git submodule update --init --recursive   — fetch the SDK + libDaisy
#   make libdaisy       — build lib/libDaisy once after cloning
#   make                — build firmware → build/athanor.bin
#   make program-dfu    — flash over USB (module in DFU mode: hold B3 at boot)
#   make program-live   — reboot the running module over USB and flash it
#   make clean          — remove the build tree
# =============================================================================

TARGET = athanor

# Athanor uses V2-only hardware (J3 as a calibrated CV input).
BOARD = v2

ALCHEMY_DIR  = lib/alchemy-sdk
LIBDAISY_DIR = lib/libDaisy

# ── App sources ─────────────────────────────────────────────────────────────
CPP_SOURCES = \
    src/athanor.cpp \
    src/stretch_engine.cpp \
    src/fx.cpp \
    src/age.cpp \
    src/drive.cpp \
    src/modfx.cpp \
    src/recorder.cpp \
    src/shifter.cpp \
    src/crashlog.cpp

# ── Alchemy SDK, compiled straight from the submodule ───────────────────────
CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))

C_INCLUDES += \
    -Isrc \
    -Idsp \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include

C_DEFS += -DALCHEMY_BOARD_V2

# ── Version stamp (HostLink HELLO + descriptor) ─────────────────────────────
# version.txt is maintained by release-please; the short git hash marks
# exactly which commit a binary came from (+ "-dirty" for local changes).
ATHANOR_VERSION  := $(shell cat version.txt 2>/dev/null || echo 0.0.0)
ATHANOR_GIT_HASH := $(shell git describe --always --dirty --abbrev=7 --exclude '*' 2>/dev/null || echo dev)
C_DEFS += -DATHANOR_VERSION='"$(ATHANOR_VERSION)"' -DATHANOR_GIT_HASH='"$(ATHANOR_GIT_HASH)"'

# Capicola's engine needs real optimisation to fit the audio budget.
OPT = -O2

# ── Daisy bootloader build (BOOT_SRAM) ──────────────────────────────────────
APP_TYPE = BOOT_SRAM
# Local copy of the SDK script + a .d2_bss section (reverb in internal SRAM).
LDSCRIPT = athanor.lds

# SD recording: FatFS long-file-name tables (ccsbcs.c) from libDaisy.
USE_FATFS = 1

# The Alchemy SDK requires C++17 (libDaisy's default is gnu++14).
CPP_STANDARD = -std=gnu++17

# ── libDaisy core Makefile does the rest ────────────────────────────────────
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

.PHONY: libdaisy
libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR)

# ── Flash without touching the module ───────────────────────────────────────
USBPID ?= df11

.PHONY: program-live
program-live: all
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)

# ── Host-side DSP tests (no hardware, native compiler) ──────────────────────
HOST_CXX ?= g++
.PHONY: test
test:
	$(HOST_CXX) -O2 -std=c++17 -Itests/stub -Isrc -Idsp tests/host_test.cpp \
	    src/fx.cpp src/drive.cpp src/age.cpp src/modfx.cpp src/stretch_engine.cpp \
	    -o build/host_test
	./build/host_test
	$(HOST_CXX) -O2 -std=c++17 -Isrc tests/shift_probe.cpp src/shifter.cpp -o build/shift_probe
	./build/shift_probe
	$(HOST_CXX) -O2 -std=c++17 -Isrc tests/drive_probe.cpp src/drive.cpp src/fx.cpp -o build/drive_probe
	./build/drive_probe
	$(HOST_CXX) -O2 -std=c++17 -Isrc tests/div_probe.cpp src/fx.cpp -o build/div_probe
	./build/div_probe
	$(HOST_CXX) -O2 -std=c++17 -Isrc tests/clock_probe.cpp -o build/clock_probe
	./build/clock_probe
	$(HOST_CXX) -O2 -std=c++17 -Isrc tests/midi_clock_probe.cpp -o build/midi_clock_probe
	./build/midi_clock_probe
	@# SD recorder on the real FatFS + the SDK's RAM disk (asserts aligned DMA).
	mkdir -p build/ffshim/util
	echo '#include "$(abspath $(LIBDAISY_DIR))/src/sys/ffconf.h"' > build/ffshim/ffconf.h
	cp $(ALCHEMY_DIR)/tests/host/fatfs_conf/util/bsp_sd_diskio.h build/ffshim/util/
	$(HOST_CXX) -O1 -std=c++17 -Itests/stub -Isrc -I$(ALCHEMY_DIR)/framework/include \
	    -I$(ALCHEMY_DIR)/tests/host -Ibuild/ffshim -I$(FATFS_DIR) \
	    tests/rec_probe.cpp src/recorder.cpp $(ALCHEMY_DIR)/framework/src/storage/sd_card.cpp \
	    $(ALCHEMY_DIR)/tests/host/ram_diskio.cpp -x c $(FATFS_DIR)/ff.c $(FATFS_DIR)/option/ccsbcs.c \
	    -o build/rec_probe
	./build/rec_probe
