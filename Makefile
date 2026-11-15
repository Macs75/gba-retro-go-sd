# Game Boy Advance (gpSP) — standalone Retro-Go SD core.
#
#   make                  — build + pack → gba.bin + gba.xip
#   make host             — desktop SDL build → ./gba_host [rom.gba]
#   make docker           — same build inside Docker (no host toolchain)
#   make docker_shell     — interactive shell in the builder image
#
# BUILD_DIR must stay `build`: ld/gba_core.ld names objects as build/*.o.

#######################################
# Project identity
#######################################
PROJECT_KIND ?= core

CORE_NAME  := gba
CORE_ENTRY := app_main_gba

CORE_C_SOURCES := \
src/gpsp/gba_memory.c \
src/gpsp/sound.c \
src/gpsp/main.c \
src/gpsp/savestate.c \
src/gpsp/input.c \
src/gpsp/cheats.c \
src/gpsp/serial.c \
src/gpsp/serial_proto.c \
src/gpsp/gbp.c \
src/gpsp/rfu.c \
src/porting/gba_frontend.c \
src/porting/gba_idle_loop.c \
src/porting/gba_audio_filter.c \
src/porting/gba_rec.c \
src/porting/gba_prof.c \
src/porting/gba_timing.c \
src/porting/gba_pchist.c \
src/porting/gba_idle.c \
src/porting/gba_scale.c \
src/main_gba.c \
src/porting/gba_bios_hle.c \
src/m4a/m4a_hle.c \
src/m4a/m4a_gpsp.c \
src/porting/bilinear.c

CORE_CXX_SOURCES := \
src/gpsp/cpu.cc \
src/gpsp/video.cc

CORE_ASM_SOURCES := \
src/porting/gba_bios.S

CORE_C_INCLUDES := \
-Isrc/gpsp \
-Isrc/gpsp/libretro/libretro-common/include \
-Isrc/m4a \
-Isrc/porting

# Relative path so Docker bind-mounts work (do NOT use $(abspath)).
GNW_CORE_SDK ?= sdk
# Must match EXCLUDE_FILE / .core_itcm paths in ld/gba_core.ld.
BUILD_DIR ?= build

#######################################
# Kind-specific compile defs + packing
#######################################
ifeq ($(PROJECT_KIND),core)
# COVERFLOW=1 CHEAT_CODES=1 MAX_CHEAT_CODES=13: match release firmware
# retro_emulator_file_t layout. gpSP's own MAX_CHEATS (20 engine slots) is
# a different constant in src/gpsp/cheats.h.
#
# ROM_BUFFER_SIZE=0    cart XIP from QSPI — not buffered in RAM.
# OBJ_PER_LINE_MAX=32  caps sprite-line BSS (~120 KB saved vs gpSP default).
# BUFFER_SIZE=4096     audio ring (gpSP default is 128 KB).
# GBA_DTCM_BUFFERS     bios / sound / cheats allocated at runtime (DTCM/AHB).
CORE_C_DEFS := \
-DPROJECT_KIND_CORE=1 \
-DCOVERFLOW=1 \
-DCHEAT_CODES=1 \
-DMAX_CHEAT_CODES=13 \
-DROM_BUFFER_SIZE=0 \
-DOBJ_PER_LINE_MAX=32 \
-DBUFFER_SIZE=4096 \
-DGBA_SOUND_FREQUENCY=48000 \
-DGBA_M4A_HLE \
-DGBA_BIOS_HLE \
-DGBA_DTCM_BUFFERS

PACKED_BIN  := $(CORE_NAME).bin
XIP_BIN     := $(CORE_NAME).xip
PAD_LOGO    := src/assets/pad.bmp
HEADER_LOGO := src/assets/header.bmp

CORE_LDSCRIPT := ld/gba_core.ld
CORE_EXTRA_SEGMENTS := itcm:core_itcm
CORE_ASFLAGS := -Isrc/gpsp/bios

else
$(error PROJECT_KIND must be 'core' (got '$(PROJECT_KIND)'))
endif

# PROF=1: the device profiler (src/porting/gba_prof.c): PROF/ lines every
# 120 frames in the log. The objects do not depend on the defines and
# BUILD_DIR must stay build (the linker script's paths), so a switch of
# PROF needs a clean build first: the stamp below stops the build if it was
# built the other way.
# GBA_IDLE_TABLES=1: the per-game idle tables (gpSP's gba_over.h,
# src/porting/gba_idle_loop.c) instead of the generic skip (gba_idle.c).
ifeq ($(GBA_IDLE_TABLES),1)
CORE_C_DEFS += -DGBA_IDLE_TABLES
endif

# AUTOREPLAY=1: replay "<rom>.rec" from the first frame (tests on gwemu).
ifeq ($(AUTOREPLAY),1)
CORE_C_DEFS += -DGBA_AUTOREPLAY
endif

# CPI=1 (with PROF=1): also count the interpreted instructions (PROF/cpi).
# Not by default: the count changes the interpreter's codegen and cost
# (+7-9% in Kirby), so PROF=1 alone measures the interpreter as shipped.
PROF ?= 0
CPI ?= 0
# PCS=1 (with PROF=1): the device PC sampler during replays (<rom>.pcs,
# tools/gba_pcs_report.py). Off by default: its SD writes stall the loop
# every 1800 frames and it swaps the interrupt table. PCS_BLOCK=120: one
# histogram per PROF window instead (frames per block).
PCS ?= 0
PCS_BLOCK ?= 1800
ifeq ($(PROF),1)
CORE_C_DEFS += -DGBA_PROF
ifeq ($(CPI),1)
CORE_C_DEFS += -DGBA_ICOUNT
endif
ifeq ($(PCS),1)
CORE_C_DEFS += -DGBA_PCS -DGBA_PCS_BLOCK=$(PCS_BLOCK)u
endif
endif
GBA_PROF_STAMP := $(BUILD_DIR)/.gba_prof
ifeq ($(filter clean host host_clean docker docker_pull docker_shell,$(MAKECMDGOALS)),)
GBA_PROF_PREV := $(shell cat $(GBA_PROF_STAMP) 2>/dev/null)
ifneq ($(GBA_PROF_PREV),)
ifneq ($(GBA_PROF_PREV),$(PROF)$(CPI)$(PCS)$(PCS_BLOCK))
$(error $(BUILD_DIR) was built with PROF/CPI=$(GBA_PROF_PREV): make clean first)
endif
endif
$(shell mkdir -p $(BUILD_DIR) && echo $(PROF)$(CPI)$(PCS)$(PCS_BLOCK) > $(GBA_PROF_STAMP))
endif

include $(GNW_CORE_SDK)/Makefile
include host/Makefile.host

PACK_CORE := $(GNW_CORE_SDK)/tools/pack_core.py

#######################################
# Packed header version
#######################################
# gnw_core_meta_t only stores major.minor.patch (0..255).
# CORE_VERSION is the full git describe string passed to the packer; it
# extracts the leading vX.Y.Z (NOTAG / missing tags → 0.0.0).
# Override: make CORE_VERSION=v1.2.3
CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

# gpSP's main.c shares a basename with nothing else here, but pin it so a
# future vpath collision cannot pick the wrong main.c (firmware Makefile
# had this exact trap against Core/Src/main.c).
$(BUILD_DIR)/main.o: src/gpsp/main.c $(REDEFINE_SYMS_FILE) | $(BUILD_DIR)
	$(V)$(ECHO) [ CC ] $(notdir $<)
	$(V)$(CC) -c $(CFLAGS) $< -o $@
	$(V)$(CP) --redefine-syms=$(REDEFINE_SYMS_FILE) $@
	$(V)$(call CORE_APPLY_EXTRA_REDEFINE,$@)

GBA_WARN_OFF := -Wno-parentheses -Wno-unknown-pragmas -Wno-strict-aliasing -Wno-comment \
	-Wno-unused-function -Wno-stack-usage -Wno-unused-variable \
	-Wno-unused-but-set-variable -Wno-sign-compare -Wno-format

CFLAGS   += $(GBA_WARN_OFF)
CXXFLAGS += $(GBA_WARN_OFF)

# Size-first default (-Os) for cold files. Per-frame / per-sample paths
# match the firmware GBA core: gba_memory/cpu at -O3, video/sound/HLE at -O2.
# Only OPT is overridden: CFLAGS stays recursive, so its -MF"$(@:%.o=%.d)"
# is expanded per object (rebuilding CFLAGS with := expanded $@ empty and
# lost the dependency files and -DUSE_HAL_DRIVER of these objects).
$(BUILD_DIR)/gba_memory.o:       OPT := -O3
# GCC: cpu.cc's threaded Thumb dispatch ends every handler with its own
# fetch and computed goto (THUMB_NEXT). GCC factors computed gotos into one
# shared dispatch and copies it back only when it is at most
# max-goto-duplication-insns long (8): the tail is ~20, so raise the limit or
# every handler jumps to a single shared dispatch again.
# GCC: -fno-tree-pre -fira-region=one: execute_arm's register allocation
# flipped with unrelated edits (reg[]'s base to a high register: every reg[]
# access 32-bit, +1.4-2.4 KB, ITCM overflowed); with these two the function
# is ~4 KB smaller and reg[] stays in a low register. Re-check the size and
# the base of `str rX, [rY, #60]` (the PC store) if cpu.cc changes.
$(BUILD_DIR)/cpu.o:              OPT := -O3 --param max-goto-duplication-insns=40 -fno-crossjumping -fno-tree-tail-merge -fno-tree-pre -fira-region=one
$(BUILD_DIR)/video.o:            OPT := -O2
$(BUILD_DIR)/sound.o:            OPT := -O2
$(BUILD_DIR)/gba_bios_hle.o:     OPT := -O2
# GCC: -mlong-calls makes the M4A mixers copy-safe: their calls into the
# ITCM memory helpers load absolute addresses from their own literal pools
# instead of a PC-relative bl to a veneer that stays behind in the flash
# (m4a_gpsp.c copies the game's mixer to RAM).
$(BUILD_DIR)/m4a_hle.o:          OPT := -O2 -mlong-calls
$(BUILD_DIR)/m4a_gpsp.o:         OPT := -O2

#######################################
# Pack
#######################################
.PHONY: pack

$(XIP_BIN): $(TARGET_ELF)
	$(V)$(ECHO) [ XIP ] $(XIP_BIN)
	$(V)$(CP) -O binary --only-section=.xip_gba --only-section=.rodata_gba $< $@
	$(V)$(SZ) --target=binary $@

pack: $(TARGET_BIN) $(BUILD_DIR)/gba_core_itcm.bin $(XIP_BIN) $(PAD_LOGO) $(HEADER_LOGO)
	$(V)$(ECHO) [ PACK CORE ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_CORE) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--system name="Nintendo Gameboy Advance",dirname=gba,pad_logo=$(PAD_LOGO),header_logo=$(HEADER_LOGO),ext=gba,parse=rom,cheat_ext=ggcodes \
		--logo-invert \
		--segment itcm:__ITCM_CORE_START__:__CORE_ITCM_CODE_END__:__CORE_ITCM_BSS_END__:$(BUILD_DIR)/gba_core_itcm.bin \
		--core-name "gpSP" \
		--version "$(CORE_VERSION)" \
		--out $(PACKED_BIN)

all: pack

.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-XIP_BIN print-CORE_VERSION
print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)
print-TARGET_ELF:
	@echo $(TARGET_ELF)
print-TARGET_MAP:
	@echo $(BUILD_DIR)/$(CORE_NAME)_core.map
print-XIP_BIN:
	@echo $(XIP_BIN)
print-CORE_VERSION:
	@echo $(CORE_VERSION)

clean::
	$(V)rm -f $(PACKED_BIN) $(XIP_BIN)

#######################################
# Docker (same image as firmware repo)
#######################################
.PHONY: docker docker_pull docker_shell

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker:
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE) "PROJECT_KIND=$(PROJECT_KIND)"
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc) PROJECT_KIND=$(PROJECT_KIND) PROF=$(PROF) CPI=$(CPI) PCS=$(PCS) PCS_BLOCK=$(PCS_BLOCK) AUTOREPLAY=$(AUTOREPLAY)

docker_pull:
	$(V)$(ECHO) "[ PULL ]" $(DOCKER_IMAGE)
	$(V)docker pull $(DOCKER_IMAGE)

docker_shell:
	$(DOCKER_RUN) bash
