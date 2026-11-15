# Changelog

This file is a template for the single project created from this repo.
At project setup time you choose exactly one kind by setting `PROJECT_KIND`
to `core` or `homebrew` (you will only build/release that chosen kind).

Update the content for your project and keep the section heading matching
the pushed release tag (CI requirement).

This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Release tags must
match a section heading exactly (for example `v1.0.0`).

When you cut a release:

1. Move items from `[Unreleased]` into a new `## [vX.Y.Z] - YYYY-MM-DD` section.
2. Commit the changelog update.
3. Push the tag: `git tag vX.Y.Z && git push origin vX.Y.Z`

CI reads the matching section and uses it as the GitHub Release notes. Assets
attached to the release:

- `<binary>-<tag>.zip` — SD layout only (`cores/` + packed `.bin` and sidecar `.xip`)
- `<binary>-<tag>-debug.zip` — ELF + linker map (use `arm-none-eabi-addr2line` for crash PC/LR → function/line)

## [Unreleased]

### Added

- **Record and replay input** from the pause menu ("Record input" /
  "Replay input"): the game restarts from power-on and the keys of every
  frame are saved next to the ROM (`<game>.rec`, with the cart save as it
  was at the start); a replay plays them back exactly, for reproducing a
  problem or comparing performance. A replay uses the recording's cart
  save, so the player's save file is not written for the rest of that
  session. Every 120 frames both log `REC/hash` lines (RAM, video, CPU and
  audio hashes): two runs of one recording must print the same lines. A
  replay or recording also ends with a `REC/hash` line for its last frame
  (unless that frame was just hashed), so the very end can be compared.
- **Generic poll-loop skip, for every game**: loops that only read and
  compare (waiting on DISPSTAT, VCOUNT or a RAM flag an interrupt sets) are
  found while the game runs, and their passes are skipped up to the next
  hardware event once two passes prove they change nothing (no store,
  identical registers and flags). Exact: host replays with and without it
  log the same `REC/hash` lines in all 149 games of a 150-game set that run
  on the host. Kirby & The Amazing Mirror: 12-17% fewer instructions in its
  heaviest stage-2 scenes.

- Host tools: `GBA_PCHIST=1` (PC histogram, `tools/gba_pchist.py`),
  `tools/gba_idle_census.sh` (poll loops across a ROM set),
  `tools/gba_idle_ab.sh` (the skip against none, over a ROM set).
- `make PROF=1`: a profiling build that prints `PROF/` lines every 120 frames
  (frame budget, the ARM7 / renderer / M4A mixer split, scaling, the LCD and
  audio waits); with `PCS=1` it also samples the device CPU's PC at 1 kHz
  while a replay runs (the firmware's SysTick through a handler in ITCM): `<game>.pcs` next
  to the ROM, one histogram per 1800 replay frames, read with
  `tools/gba_pcs_report.py` and the build's ELF.
- Host: `GBA_FAST=1`, `GBA_EXIT_FRAME=N`, `GBA_FRAMESKIP=N`, `GBA_RECORD=1` /
  `GBA_REPLAY=1` (`GBA_REC_AT=N`), `GBA_HASH_EVERY=N`, `GBA_INPUT_RAW=<file>`.

### Changed

- **The GBA's real frame rate, 59.7275 Hz** (280,896 cycles of 16.78 MHz).
  Every sample a frame produces (803.65 on average at 48 kHz) goes into the
  firmware's interrupt-fed audio ring, and the loop paces on that ring, so
  it runs at the audio's rate divided by the samples per frame: the GBA's.
  The LCD refresh is set to the same rate (milli-Hz, read back from the
  real audio clock; firmwares without milli-Hz rates keep 60 Hz), and the
  firmware's frameskip gets the exact frame time.
- Every frame that is on time, or late with at least 2 frames of audio
  still queued, draws at a fixed point of the LCD's scan (after the last
  swap has shown, between scan lines 32 and 200), so its swap never
  straddles the vertical blank; a late frame with the audio low draws at
  once, or drops its picture if the last swap has not shown yet.
- **Smoother FULL / FIT scaling** (the "hard" filter): the arcade core's
  scaler. Every 3 GBA pixels make 4 on the panel, (s0, s1, avg(s1,s2), s2),
  and every panel row is a 2-tap blend of the two GBA rows around it, done
  by DMA2D from a small ring of pre-scaled rows while the CPU scales the
  next one. Scaling OFF and the "soft" filter are unchanged.
- Faster interpreter: the ARM7's flags are written into the CPSR only where
  something reads them (MSR, MRS, SWI, the M4A HLE, events, interrupts)
  instead of before every instruction, and the cheat hook shares the idle
  skip's one PC compare (with cheats on, the idle skip is off). Exact: the
  same `REC/hash` lines as before in all 149 games that run on the host.
- Interpreter: the special PCs (the idle skip's loop head, the M4A
  mixer's entry, the cheat hook) are tested only after an ARM jump or on
  entering the ARM loop, not before every ARM instruction, and the PC is
  masked only when it is misaligned.
- Interpreter: the PC is kept in a local copy. The handlers' PC offsets
  are stores from it instead of load-add-store sequences on `reg[15]`, and
  the fetch, region check and alignment read it instead of memory. Every write
  of the PC also updates the copy, so the dispatch never reads `reg[15]`
  (host `HOST_EXTRA_DEFS=-DGBA_PC_CHECK` checks it after every
  instruction). Device, Tony Hawk's Underground: -3.2% interpreter cycles,
  4,617 -> 4,926 pictures shown of 5,640.
- Interpreter: ARM conditions by a 16-entry table with an AL fast path (no
  16-way jump per instruction); stores to work RAM done by small functions
  in ITCM (no call into `write_memory*` in AXI RAM); LDM/STM visit only the
  listed registers; Thumb's idle-target compare uses a local copy. Device,
  Tony Hawk's Underground: -8.4% interpreter cycles, 4,926 -> 5,200
  pictures shown of 5,640, windows at a full 59.7 fps 15 -> 29 of 47.
- Interpreter code size: power-on and savestate code moved from ITCM to the
  XIP flash with the rest of the cold code. The register-operand barrel shifter and
  the shifted register offset of LDR/STR are one shared copy each instead
  of being expanded in every ALU and load/store case; the shifts by an
  immediate, nearly all real code, stay inline. LDM/STM share one body: the
  forms compilers emit (push/pop, LDMIA/STMIA, LDMDB, all Thumb forms)
  inline it, the 26 other ARM forms call one copy. ITCM free: 840 B ->
  12.6 KB. Device, Tony Hawk's Underground: interpreter cost unchanged,
  5,200 -> 5,264 pictures shown of 5,640, frames over budget 2,359 -> 1,981.
- Interpreter: the cycle count and the four flags stay in registers (no
  local's address escapes any more), and Thumb uses threaded dispatch:
  every handler ends with its own fetch and jump through a label table in
  ITCM; an alert is folded into the cycle count instead of being tested
  after every Thumb instruction. Device, Tony Hawk's Underground: -3%
  interpreter cycles.
- Hot code out of the flash: the plain sprite renderer, the tile-layer
  drivers, the M4A HLE's memory helpers and the idle skip's hooks run from
  ITCM (the device PC sampler found them running from gba.xip); the serial
  port is no longer polled at every event while no link device is
  emulated.
- Renderer: the text layers' tile loops are unrolled (8 pixels a tile as
  fixed bit-field extracts, no loop counter or variable shift) for the
  16-bit full-colour instances, the plain layers of nearly every game, and
  the 32-bit indexed/stacked ones, which every screen with blending or a
  brightness fade draws through (31% of the text pixels in the first
  minute of a 150-game set). Same pictures, frame by frame, in 10 games.
  Device: Tony Hawk's Underground drawing -20%, Kirby & The Amazing Mirror
  -13%; Tony's blended menu a further -12% per drawn frame.
- Code and data placement is pinned where the device measured it best:
  the code after the interpreter in ITCM (mod 32), the hot RAM drawing code
  (mod 8 KB, the I-cache's set span), the flash read-only data and the GBA
  memories (mod 4 KB, the D-cache's set span). Unpinned, unrelated edits
  moved the drawing's cost by 5-14%.
- `PROF/` lines: `fps`, dropped pictures (`vskip`), late frames, the audio
  ring's lowest fill, `PROF/idle` (the generic skip), and with `CPI=1`
  `PROF/cpi` (device cycles per GBA instruction; the count itself costs the
  interpreter 7-9%, so it is opt-in).

- The per-game idle tables (gpSP's `gba_over.h`, `gba_idle_loop.c` and the
  two hand-listed VCOUNT polls) are off: on 37 of the 47 games of the set
  they covered, the generic skip removed 39-88% more instructions (and as
  many on the other 10), exactly, where the tables' skip was not.
  `make GBA_IDLE_TABLES=1` brings them back instead.

### Fixed

- The generic poll-loop skip lost real poll loops that shared a 64-byte
  block with rejected code: rejected loops are now remembered by their head,
  and a rejected block is decoded again at every 16th event pair. 18 of the
  150 host games had lost most of their skip (Broken Sword ran 73K
  interpreted instructions a frame instead of 12K).
- Pacing: the loop locked onto the LCD (60 Hz set, ~59.64 Hz real) because
  drawing slept until the last swap showed; it then never waited for the
  audio, which slowly underran, and the frameskip's rounded frame time
  (16.74 ms against 16.77 real) skipped 2 frames every ~10 s on frames with
  10-16% to spare (Kirby & The Amazing Mirror).
- Build: the -O2/-O3 objects (CPU, memory, renderer, sound, BIOS and M4A
  HLE) got no dependency files, so a header change did not rebuild them,
  and a stray `-DUSE_HAL_DRIVER` file appeared in the tree.
- Host build: a missing `odroid_system_switch_app` and a `frame_counter`
  clash with gpSP's own.

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
