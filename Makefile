# beken-prj - top level Makefile
#
# Multi-board wrapper around the vendor (Beken) build system.
#
#   make build BOARD=migan-bk7258 PROJECT=blink_hello
#   make build BOARD=migan-bk7258 PROJECT=beken_genie   (vendor project, lives in the vendor tree)
#   make patches                                        # apply local patches to the vendor submodule
#   make flash BOARD=migan-bk7258 PROJECT=blink_hello COM=42
#   make clean  BOARD=migan-bk7258 PROJECT=blink_hello
#
# Machine-specific overrides (flash tool paths, COM port) go into config.local.mk
# (see config.local.mk.example), never into committed files.

BOARD   ?= migan-bk7258
PROJECT ?= blink_hello
SOC     ?= bk7258

VENDOR_DIR  := vendor/bk_aidk
BOARD_DIR   := boards/$(BOARD)
PATCH_STAMP := build/.vendor-patched

# NOTE: PROJECT_DIR/BUILD_DIR must stay RELATIVE to vendor/bk_aidk/bk_avdk/bk_idk
# (the vendor build system string-concatenates them with its own root).
REL_ROOT := ../../../../
PROJECT_DIR_OURS := $(REL_ROOT)boards/$(BOARD)/projects/$(PROJECT)
BUILD_DIR_REL    := $(REL_ROOT)build/$(BOARD)/$(PROJECT)

-include config.local.mk
-include $(BOARD_DIR)/board.mk

export GEN_IMAGE_WIN_EXE
export ENCRYPT_CRC_WIN_EXE
export BK_LOADER

# Projects that live in this repo (anything else falls back to the vendor tree)
OUR_PROJECTS = blink_hello pwm_test

.PHONY: build patches flash clean help

help:
	@echo "Boards : $(notdir $(wildcard boards/*))"
	@echo "Usage  : make build   BOARD=<board> PROJECT=<project>"
	@echo "         make flash   BOARD=<board> PROJECT=<project> COM=<n>"
	@echo "         make patches / make clean"

patches: $(PATCH_STAMP)

$(PATCH_STAMP): $(wildcard patches/*.patch) tools/apply-vendor-patches.sh
	bash tools/apply-vendor-patches.sh
	@mkdir -p $(dir $@) && touch $@

build: $(PATCH_STAMP)
	$(MAKE) -C $(VENDOR_DIR) $(SOC) PROJECT=$(PROJECT) BUILD_DIR=$(BUILD_DIR_REL) \
	$(if $(filter $(PROJECT),$(OUR_PROJECTS)),PROJECT_DIR=$(PROJECT_DIR_OURS),)

flash: build
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/flash.ps1 \
		-Bin "$(BUILD_DIR_REL)/$(SOC)/all-app.bin" -Com $(COM)

clean:
	-$(MAKE) -C $(VENDOR_DIR) $(SOC) PROJECT=$(PROJECT) BUILD_DIR=$(BUILD_DIR_REL) \
	$(if $(filter $(PROJECT),$(OUR_PROJECTS)),PROJECT_DIR=$(PROJECT_DIR_OURS),) clean
	@rm -rf build/$(BOARD)/$(PROJECT)
