/*
 * Game Boy Advance (gpSP) core linker script.
 *
 *   RAM_EMU  — entry + main_gba + hot video (scanline_text) + remaining
 *              hot text + .data/.bss. No bios/sound/FB here: those are
 *              dtc_*'d at runtime. Cheats use the AHB heap (cold).
 *   ITCM     — hot CODE only (no .data/.bss/.rodata):
 *                cpu.o interpreter, m4a_gpsp.o (called from cpu),
 *                video update_scanline (every scanline from the interpreter).
 *              Remaining ITCM is too tight for render_scanline_text (~21 KiB)
 *              or the sprite/effects helpers; those stay in RAM_EMU.
 *              ITCM may still hold absolute veneers into XIP — main_gba
 *              patches those after caching gba.xip (same pass as RAM_EMU).
 *
 * Plus a non-packaged XIP sidecar (.xip_gba + .rodata_gba @ GBA_CODE
 * 0xDEC00000) shipped as gba.xip and cached into QSPI at runtime
 * (see src/main_gba.c gba_cache_xip_to_flash()).
 *
 * Objects land in a flat build/ dir (basenames only).
 */

INCLUDE gnw_ram_emu.ld
INCLUDE gnw_itcm_core.ld

MEMORY
{
  RAM_EMU  (xrw) : ORIGIN = __RAM_EMU_START__,  LENGTH = __RAM_EMU_LENGTH__
  ITCM     (xrw) : ORIGIN = __ITCM_CORE_START__, LENGTH = __ITCM_CORE_LENGTH__
  /* Sentinel / XIP link address only — payload ships as gba.xip, not in the CORE bin. */
  GBA_CODE (x)   : ORIGIN = 0xDEC00000,         LENGTH = 512K
}

SECTIONS
{
  /* ------------------------------------------------------------------ */
  /* Segment 0: RAM_EMU — entry, main_gba (whole), hot video text that  */
  /* does not fit ITCM, cpu/m4a rodata, then .bss.                      */
  /* ------------------------------------------------------------------ */
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))
    . = ALIGN(4);

    /* main_gba.o whole — holds GBA_CODE_BASE and the sentinel patch pass;
     * the pass must not walk itself. Symbols bound the patch range start. */
    _GBA_MAIN_CODE_START = .;
    build/main_gba.o (.data .data* .text .text* .rodata .rodata*)
    . = ALIGN(4);
    _GBA_MAIN_CODE_END = .;

    /* Hottest remaining video that does not fit leftover ITCM. The 16-bit
     * full-colour text layers (the hot pair) run from ITCM (.core_itcm). */
    build/video.o (.text._Z20render_scanline_textItL8rendtype1*)
    build/video.o (.text._Z20render_scanline_textIjL8rendtype1*)
    build/video.o (.text._Z20render_scanline_textIjL8rendtype2*)
    /* The hot RAM code from here on (sprites, the window/effects passes,
     * then everything below) keeps one place mod 8 KB, the I-cache's set
     * span, whatever the code in front of it: placement moved device cost by
     * 5-10% in the measurements of 2026-11-15. render_object at 0x1d24 mod
     * 8 KB, its measured placement (run 20); "." is section-relative here and
     * the section starts at 0x2404b000 (0x1000 mod 8 KB), hence 0xd24. */
    . = ALIGN(. - 0xd24, 0x2000) + 0xd24;
    build/video.o (.text._Z13render_object*)
    build/video.o (.text._ZL16render_w_effects*)
    /* order_obj at its measured place mod 8 KB (0x240560b4: Super Puzzle
     * Fighter II runs 5-7), whatever render_w_effects' size (it grew 924
     * bytes with the effect table's loops); the section starts 0x1000 mod
     * 8 KB, hence 0x10b4. The blended sprite renderers after it end at
     * 0x240585b4, where the code behind them was measured: the pins below
     * hold their sizes of 2026-11-20 (order_obj up to 0x260 bytes, the
     * renderers 0x107c and 0x1224); one that grows past its pin moves
     * everything behind it 8 KB (and .bss with it). */
    . = ALIGN(. - 0x10b4, 0x2000) + 0x10b4;
    build/video.o (.text._ZL9order_obj*)
    . = ALIGN(. - 0x1314, 0x2000) + 0x1314;
    /* The blending path's sprite renderers (32-bit line buffer, the indexed
     * and stacked-colour instances) and the per-line window/effects driver:
     * the hottest drawing code of a blended screen, which ran from the flash
     * (Tony's blended menu, PC sampler 2026-11-15). Room for them: the
     * load-time and savestate code went to the flash (GBA_COLD, common.h). */
    build/video.o (.text._Z20render_scanline_objsIjL8rendtype1EEvjjjPvPKt*)
    . = ALIGN(. - 0x390, 0x2000) + 0x390;
    build/video.o (.text._Z20render_scanline_objsIjL8rendtype2EEvjjjPvPKt*)
    build/video.o (.text._ZL27render_scanline_conditionaljjPtj)

    /* Remaining .data from every object (main_gba already claimed). */
    *(.data .data*)

    /* Hot .text: everything except cpu/m4a_gpsp (→ITCM) and cold XIP files.
     * savestate.o stays in RAM_EMU: its bson parse path calls strlen/memcmp
     * per key and must not run from XIP through ABI veneers. */
    EXCLUDE_FILE(
      build/cpu.o
      build/m4a_gpsp.o
      build/video.o
      build/serial.o
      build/serial_proto.o
      build/gbp.o
      build/rfu.o
      build/input.o
      build/gba_frontend.o
      build/m4a_hle.o
      build/cheats.o
      build/gba_idle_loop.o
      build/bilinear.o
      build/gba_rec.o
      build/gba_prof.o
      build/gba_timing.o
      build/gba_idle.o
      build/gba_scale.o
      build/hle_vrally3.o
      build/hle_spf2.o
      build/hle_rzw.o
      build/hle_mgolf.o
      build/hle_wcp.o
      build/gba_backup_bank.o
    ) *(.text .text*)

    /* cpu.o / m4a_gpsp.o run from ITCM. Their *rodata* must stay in RAM
     * (ITCM is code-only). Absolute veneers into XIP that the linker emits
     * in ITCM are patched at runtime (see gba_cache_xip_to_flash). */
    build/cpu.o (.rodata .rodata*)
    build/m4a_hle.o (.rodata .rodata*)
    build/m4a_gpsp.o (.rodata .rodata*)

    *(.glue_7)
    *(.glue_7t)

    /* The per-line window code (video.cc render_window_one_pass, run on
     * every line of a screen with windows; from the flash it was ~2% of the
     * device's time, Super Puzzle Fighter II): last, so adding it moved none
     * of the hot code above. */
    build/video.o (.text._ZL22render_window_one_passPtj .text._ZL15window_segments*)
    build/video.o (.text._ZL14fx_table_build*)   /* once per line with an effect */
    /* a game HLE's code too big for the ITCM window (src/hle): in RAM, first
     * -- Super Puzzle Fighter II's line-interrupt paths run ~180 times a
     * frame, at their measured place mod 8 KB (I-cache sets) beside the
     * drawing code above; behind them the rest (the idle detector's search:
     * linked in front, it moved them onto order_obj's sets and the early
     * match's drawing cost +13-20%, 2026-11-16 run 11) */
    *(.gba_ram_text.spf2*)
    *(.gba_ram_text*)
    /* The 16-bit sprite renderers for indexed colour (a dimmed or faded
     * screen's lines without semi-transparent sprites) and for copy (sprite
     * windows): from the flash their cost moved with gba.xip's place at every
     * update (Super Puzzle Fighter II's early match: 0% -> 1.5% of the
     * device's time from one push to the next, 2026-11-16) */
    build/video.o (.text._Z20render_scanline_objsItL8rendtype1EEvjjjPvPKt*)
    /* ...the second ending at 0x24067388, where what follows was measured
     * (its size of 2026-11-20, 0xf68 bytes; 0x1420 section-relative) */
    . = ALIGN(. - 0x1420, 0x2000) + 0x1420;
    build/video.o (.text._Z20render_scanline_objsItL8rendtype3EEvjjjPvPKt*)

    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;

    /* Align inside this section (not between sections): an empty trailing
     * .data would otherwise leave a VMA gap that objcopy does not emit. */
    . = ALIGN(8);
  } >RAM_EMU

  .data :
  {
    *(.data*)
    . = ALIGN(4);
    /* The rotated/scaled background renderers (modes 1 and 2), out of the
     * flash, at the end of the RAM code so nothing in front of them moves:
     * the plain-colour set (the 16-bit full-colour instances, no mosaic,
     * and their dispatchers) and the blended track layers' instances
     * (indexed / stacked colour, wrapping, rotated). 29% of the core's work
     * in F-Zero: Maximum Velocity's race, all from the flash (2026-11-15).
     * With their rotated walks (2026-11-21) the code ends past .bss's pin
     * at 0x2406bbe0, so .bss starts 4 KB later (0x2406cbe0), at the same
     * place mod 4 KB. Not in
     * .core_entry: the linker puts a section's veneers at its end, and after
     * them there they moved cpu.o's tables. In .data, which is otherwise
     * empty: the SDK packs only .core_entry and .data (sdk/Makefile). */
    build/video.o (.text._Z22render_scanline_affineItL8rendtype0E*)
    build/video.o (.text._Z24render_affine_backgroundItL8rendtype0E*Lb0EEv*)
    build/video.o (.text._Z24render_affine_backgroundItL8rendtype1ELb?ELb1ELb1ELb0EEv*)
    build/video.o (.text._Z24render_affine_backgroundIjL8rendtype2ELb?ELb1ELb1ELb0EEv*)
    /* their rotated walks (video.cc affine_rot_run, one per instance above)
     * and the line span the non-wrapping ones share */
    build/video.o (.text._Z14affine_rot_runItL8rendtype0E*)
    build/video.o (.text._Z14affine_rot_runItL8rendtype1ELb?ELb1E*)
    build/video.o (.text._Z14affine_rot_runIjL8rendtype2ELb?ELb1E*)
    build/video.o (.text._ZL16affine_line_span*)
    /* ...and the bitmap-mode renderers (modes 3-5, every colour path) and
     * the window drivers: from the flash their cost moved with gba.xip's
     * place at every update -- V-Rally 3's bitmap-mode race +24% drawing
     * from one push to the next, no renderer change (2026-11-17). Last, so
     * nothing in front of them moves. The rest of video.o (12.9 KB of
     * rarer rotated/scaled variants: mosaic, wrap, colour paths) stays in
     * the flash: RAM_EMU has no room for it (7 KB short with .bss's pin). */
    build/video.o (.text.*bitmap* .text.*window*)
    /* code added to an older RAM function's job after the renderers were
     * placed: here, behind them, so they do not move (the idle search's
     * calling loops, gba_idle.c IDLE_LATE) */
    __gba_late_text_start = .;
    *(.gba_late_text*)
    /* a fixed slot, so what follows keeps its address whatever the late
     * code's size */
    ASSERT(. <= __gba_late_text_start + 0x250, "Error: .gba_late_text outgrew its 0x250-byte slot");
    . = __gba_late_text_start + 0x250;
    /* The blended screens' unrotated rotate/scale layer (the 32-bit indexed
     * instances, wrapping, no mosaic) and their dispatchers, out of the
     * flash: Wing Commander: Prophecy's cockpit draws its 3D layer through
     * them, the largest of its drawing (2026-11-22). Behind everything, so
     * nothing moves. */
    build/video.o (.text._Z22render_scanline_affineIjL8rendtype1E*)
    build/video.o (.text._Z24render_affine_backgroundIjL8rendtype1ELb?ELb1ELb0ELb0EEv*)
    . = ALIGN(8);
  } >RAM_EMU

  __CORE_CODE_END__ = .;
  _OVERLAY_GBA_BSS_START = __CORE_CODE_END__;
  _GBA_SENTINEL_PATCH_END = __CORE_CODE_END__;

  /* .bss (the GBA memories: VRAM, the work RAMs, palette, OAM, I/O, reg[],
   * the page map) keeps one position mod 4 KB, the D-cache's set span,
   * whatever the code size: 0xbe0 is the measured placement (runs 13-20).
   * The firmware zeroes from the code's end to __CORE_BSS_END__, the gap
   * included. */
  .bss ALIGN(_OVERLAY_GBA_BSS_START - 0xbe0, 4096) + 0xbe0 (NOLOAD) :
  {
    . = ALIGN(8);
    *(.bss*)
    *(COMMON)
    *(.gba_tail_bss)         /* new variables, behind the GBA memories */
  } >RAM_EMU

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: GBA core RAM_EMU segment (code+data+bss) overflows RAM_EMU")

  /* ------------------------------------------------------------------ */
  /* Segment ITCM: hot CODE only. No .data / .bss / .rodata.            */
  /* ------------------------------------------------------------------ */
  .core_itcm __ITCM_CORE_START__ :
  {
    . = ALIGN(4);
    __CORE_ITCM_CODE_START__ = .;
    build/cpu.o (.text*)
    /* The code after cpu.o keeps one place mod 32 whatever execute_arm's
     * size: update_scanline at 20 mod 32 and the drawing code after it. Where
     * it fell with each interpreter edit moved the drawing's device cost by
     * up to +10/14% (2026-11-15: run 14, same code with this block 4 bytes off
     * mod 8 relative to the fetch). The value is the measured-good placement
     * (build 13), not derived: re-measure if this block's contents change. */
    . = ALIGN(32) + 28;
    build/m4a_gpsp.o (.text*)
    . = ALIGN(. - 20, 32) + 20;   /* whatever m4a_gpsp.o's size */
    build/video.o (.text.update_scanline)
    /* PROF's PC sampler (gba_prof.c), the idle-skip hooks (gba_idle.c), the
     * scaler (gba_scale.c). The pass-head hook and the scaler keep their
     * measured places mod 32 (run 37) whatever the event hook's size. */
    . = ALIGN(. - 4, 32) + 4;  /* the event hook at its run-37 place mod 32 */
    build/gba_prof.o (.gba_itcm_text*)
    build/gba_idle.o (.gba_itcm_text)
    . = ALIGN(. - 12, 32) + 12;
    build/gba_idle.o (.gba_itcm_text.idle_head)
    . = ALIGN(. - 16, 32) + 16;
    *(.gba_itcm_text*)
    /* The drawing code below at its measured place mod 32 (24: runs 20-27,
     * with the PC sampler above it), with or without the sampler. */
    . = ALIGN(. - 24, 32) + 24;
    /* Hot code that ran from the QSPI flash (gba.xip), by the PC sampler
     * (Kirby, 2026-11-14): the plain sprite renderer (the 16-bit, no-blend
     * instance; the other four stay in flash), the tile-layer drivers, and
     * the M4A HLE's guest-memory helpers, called per sample. */
    build/video.o (.text._Z20render_scanline_objsItL8rendtype0EEvjjjPvPKt*)
    . = ALIGN(. - 24, 32) + 24;  /* the code behind at its place mod 32 */
    build/video.o (.text._Z18tile_render_layers*)
    build/m4a_hle.o (.text.rd_* .text.wr_* .text.win_hold)
    /* The text-layer renderer's hot pair: their unrolled tile loops
     * (video.cc TILE_LOOP) do not fit RAM_EMU. */
    . = ALIGN(. - 4, 32) + 4;  /* at their measured place mod 32 (runs 20-27) */
    build/video.o (.text._Z20render_scanline_textItL8rendtype0ELb1EEvjjjPvPKt)
    build/video.o (.text._Z20render_scanline_textItL8rendtype0ELb0EEvjjjPvPKt)
    /* Read-only tables the interpreter reads every instruction: the
     * threaded Thumb dispatch's label table (ITCM serves data reads too,
     * and RAM_EMU has no room). */
    . = ALIGN(32) + 16;      /* thumb_tbl at its measured place mod 32 */
    *(.gba_itcm_rodata*)
    . = ALIGN(4);
    __CORE_ITCM_CODE_END__ = .;
  } >ITCM

  ASSERT(__CORE_ITCM_CODE_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
         "Error: GBA core ITCM segment (hot code) overflows the 64K ITCM region")

  /* ------------------------------------------------------------------ */
  /* XIP sidecar (NOT a pack segment) — cold text then remaining rodata */
  /* as one contiguous GBA_CODE image → gba.xip.                        */
  /* ------------------------------------------------------------------ */
  .xip_gba :
  {
    . = ALIGN(8);
    __xip_gba_start__ = .;
    build/video.o (.text .text*)
    build/serial.o (.text .text*)
    build/serial_proto.o (.text .text*)
    build/gbp.o (.text .text*)
    build/rfu.o (.text .text*)
    build/input.o (.text .text*)
    build/gba_frontend.o (.text .text*)
    /* The M4A mixers, each bounded: the game's one is copied to RAM at
     * start-up (m4a_gpsp.c m4a_mix_to_ram); the copy must hold all of it. */
    . = ALIGN(4); __m4a_mix_v1_start = .; build/m4a_hle.o (.text.m4a_run_v1_mono)       __m4a_mix_v1_end = .;
    . = ALIGN(4); __m4a_mix_v2_start = .; build/m4a_hle.o (.text.m4a_run_v2_stereo)     __m4a_mix_v2_end = .;
    . = ALIGN(4); __m4a_mix_v3_start = .; build/m4a_hle.o (.text.m4a_run_v3_stereo2)    __m4a_mix_v3_end = .;
    . = ALIGN(4); __m4a_mix_v4_start = .; build/m4a_hle.o (.text.m4a_run_v4_stereo3)    __m4a_mix_v4_end = .;
    . = ALIGN(4); __m4a_mix_v5_start = .; build/m4a_hle.o (.text.m4a_run_v5_bytes)      __m4a_mix_v5_end = .;
    . = ALIGN(4); __m4a_mix_v6_start = .; build/m4a_hle.o (.text.m4a_run_v6_bytes_mono) __m4a_mix_v6_end = .;
    build/m4a_hle.o (.text .text*)
    build/cheats.o (.text .text*)
    build/gba_idle_loop.o (.text .text*)
    build/bilinear.o (.text .text*)
    build/gba_rec.o (.text .text*)
    build/gba_prof.o (.text .text*)
    build/gba_timing.o (.text .text*)
    build/gba_idle.o (.text .text*)
    build/gba_scale.o (.text .text*)
    build/gba_backup_bank.o (.text .text*)
    *(.gba_cold_text*)       /* cold cpu.o code: power-on, savestates */
  } >GBA_CODE

  .rodata_gba :
  {
    /* The flash read-only data keeps one position mod 4 KB (the D-cache's set
     * span) whatever the cold code in front of it: a 160-byte shift moved the
     * drawing's device cost by 5-8% (2026-11-15: runs 10 vs 13). 0x5e0 is the
     * measured-good offset, not derived; costs up to 4 KB of the 512 KB. */
    . = ALIGN(4096) + 0x5e0;
    __rodata_gba_start__ = .;
    EXCLUDE_FILE(build/hle_*.o build/gba_hle_table.o) *(.rodata .rodata*)
    /* the game HLE units' data: in their overlays; the HLE table last, so
     * adding a game moves none of the renderer's tables above */
    build/gba_hle_table.o (.rodata .rodata*)
    . = ALIGN(4);
    __rodata_gba_end__ = .;
    __xip_gba_end__ = .;
  } >GBA_CODE

  ASSERT(__xip_gba_end__ <= ORIGIN(GBA_CODE) + LENGTH(GBA_CODE),
         "Error: GBA XIP (.xip_gba + .rodata_gba) overflows 512K GBA_CODE")

  /* ------------------------------------------------------------------ */
  /* Game HLE overlays (src/hle, gba_hle.h): each game's native routine   */
  /* is linked to run at one ITCM address, behind the core's ITCM code,   */
  /* and stored in gba.xip behind the flash data (the Makefile's          */
  /* GBA_HLE_OVERLAYS); gba_hle_select copies the loaded game's there.    */
  /* ------------------------------------------------------------------ */
  __gba_hle_window = ALIGN(__CORE_ITCM_CODE_END__, 32);
  OVERLAY __gba_hle_window : NOCROSSREFS AT (ALIGN(__xip_gba_end__, 4))
  {
    gba_hle_vrally3 { build/hle_vrally3.o (.text .text* .rodata .rodata*) }
    gba_hle_spf2    { build/hle_spf2.o (.text .text* .rodata .rodata*) }
    gba_hle_rzw     { build/hle_rzw.o (.text .text* .rodata .rodata*) }
    gba_hle_mgolf   { build/hle_mgolf.o (.text .text* .rodata .rodata*) }
    gba_hle_wcp     { build/hle_wcp.o (.text .text* .rodata .rodata*) }
  }
  /* the window holds the largest */
  __gba_hle_window_end = __gba_hle_window + MAX(MAX(MAX(SIZEOF(gba_hle_vrally3), SIZEOF(gba_hle_spf2)), SIZEOF(gba_hle_rzw)), MAX(SIZEOF(gba_hle_mgolf), SIZEOF(gba_hle_wcp)));
  /* each overlay's size, for gba_hle_table.c (not its end address: see there) */
  __gba_hle_size_vrally3 = SIZEOF(gba_hle_vrally3);
  __gba_hle_size_spf2 = SIZEOF(gba_hle_spf2);
  __gba_hle_size_rzw = SIZEOF(gba_hle_rzw);
  __gba_hle_size_mgolf = SIZEOF(gba_hle_mgolf);
  __gba_hle_size_wcp = SIZEOF(gba_hle_wcp);
  /* The window is the ITCM segment's "BSS": the firmware loader reserves a
   * core's ITCM code + BSS (rg_emulators.c, itc_malloc) and hands out the
   * rest of ITCM later (rg_logos.c loads /bios/logo.bin there on first use,
   * 608 B today, behind assert()s): unreserved, it landed on the HLE copied
   * into the window (V-Rally 3 hard fault, 2026-11-15). Keep 1 KB for it. */
  __CORE_ITCM_BSS_END__ = __gba_hle_window_end;
  ASSERT(__CORE_ITCM_BSS_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__ - 1024,
         "Error: the game HLE window leaves the firmware under 1 KB of ITCM (its logos)")
  ASSERT(__load_stop_gba_hle_wcp <= ORIGIN(GBA_CODE) + LENGTH(GBA_CODE),
         "Error: GBA XIP with the game HLE overlays overflows 512K GBA_CODE")

  __exidx_start = .;
  __exidx_end = .;

  /DISCARD/ :
  {
    *(.fini_array*)
    *(.preinit_array*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.ARM.attributes)
  }
}
