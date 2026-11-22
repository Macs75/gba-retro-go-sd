# Changelog

## [v0.1.0] - 2026-11-22

The first release since v0.0.1: mostly speed, measured on the device
against recordings, with every change exact (same game state, frame for
frame). The full list, with mechanisms and numbers, is in
[docs/changes-v0.1.0.md](docs/changes-v0.1.0.md).

### Speed

- A faster interpreter: flags and cycles kept in registers, ARM conditions
  by table, cheaper Thumb fetches, and its hot code in ITCM.
- A generic wait-loop skip for every game, including loops that call
  functions, in place of gpSP's per-game tables.
- The M4A (Sappy) sound driver: its native mixer runs from RAM, resumes an
  interrupted block natively, and its reverb loop runs natively.
- A faster renderer: unrolled tile loops, one-table fades, window regions
  planned per line, sprite lists rebuilt in bands for sprite multiplexers,
  rotated layers drawn a cell at a time, and the hot renderers out of the
  flash.
- Native versions of the hottest routines of five games, generated from
  their own code: V-Rally 3, Super Puzzle Fighter II, WarioWare: Twisted!,
  Mario Golf: Advance Tour and Wing Commander: Prophecy.

### Added

- Record and replay input from the pause menu.
- Tilt controls for WarioWare: Twisted! and Yoshi's Universal Gravitation /
  Yoshi Topsy-Turvy.

### Changed

- The GBA's real frame rate (59.73 Hz), with pacing locked to the audio.
- Smoother FULL / FIT scaling.
- The cart save takes 64 KB of RAM instead of 128 KB.

### Fixed

- Sprites beyond 32 on one priority on one line were not drawn (Kingdom
  Hearts: Chain of Memories' dialog portraits, and 22 other games).
- WarioWare, Inc.: Mega Microgame$! froze at Jimmy's disco stage;
  WarioWare: Twisted! locked up in Jimmy's third stage.
- Sampled music played random notes in games that ask the BIOS for a note's
  frequency.
- Games without the M4A sound driver no longer spend a third of the frame
  on it.

## [v0.0.1] - 2026-08-20

### Added

- Ported the firmware Game Boy Advance core (gpSP + `porting/gba` + M4A HLE)
  into this standalone tree: `/cores/gba.bin` + `/cores/gba.xip`, `ggcodes`
  cheats, optional `/bios/gba/gba_bios.bin`.
- Tag releases ship install + debug zips only (no loose `.bin`): SD layout
  includes `gba.xip`; debug zip has ELF/map for `addr2line` / `resolve_addr.py`.

### Changed

- Packed output is `gba.bin` (`/cores/gba.bin`). ROMs under `/roms/gba/`.
- ITCM holds hot **code only** (interpreter, M4A gpsp glue, `update_scanline`).
  No ITCM data.
- 240×160 framebuffer is `dtc_malloc`'d (75 KiB; leftover AHB is too small).
  BIOS and sound ring prefer DTCM, then AHB. Cheats stay on the AHB heap.
- Logos from firmware `icons/c_gba.bmp` + `h_gba.bmp`.

### Added

- Freestanding Cortex-M7 skeleton (`src/main.c`) with LCD demo, square-wave
  audio, save/load/screenshot hooks, and watchdog-friendly frame loop.
- Vendored SDK, linker scripts, and ABI bridge for `gw_firmware_abi_t`.
- Packaging for both project kinds:
  - **core** → `pack_core.py`, SD path `/cores/<name>.bin`
  - **homebrew** → `pack_homebrew.py`, SD path `/homebrews/<name>.bin`
- Docker builder integration (`make docker`) using `sylverb/retro-go-sd-builder`.
- CI build on push/PR and automated GitHub Release on `v*` tags.

### Install

Only the section corresponding to your chosen `PROJECT_KIND` is relevant for
your derived project.

**Core (`PROJECT_KIND=core`, default)**

- Copy `example.bin` to `/cores/` on the SD card.
- Place test ROMs under `/roms/example/` (dirname matches `CORE_NAME` in the
  Makefile).
- Requires firmware whose ABI matches `SDK_VERSION` in this repository.

**Homebrew (`PROJECT_KIND=homebrew`)**

- Set `PROJECT_KIND=homebrew` in the Makefile, rebuild, then copy
  `ExampleHB.bin` to `/homebrews/`.
- Optional coverflow override: `/covers/homebrew/ExampleHB.img` (JPEG ≤186×100,
  ≤10 KiB).

The release archive contains the ready-to-copy SD layout for the active project
kind only (`cores/` or `homebrews/`).
