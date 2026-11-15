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
    build/video.o (.text._ZL9order_obj*)
    /* The blending path's sprite renderers (32-bit line buffer, the indexed
     * and stacked-colour instances) and the per-line window/effects driver:
     * the hottest drawing code of a blended screen, which ran from the flash
     * (Tony's blended menu, PC sampler 2026-11-15). Room for them: the
     * load-time and savestate code went to the flash (GBA_COLD, common.h). */
    build/video.o (.text._Z20render_scanline_objsIjL8rendtype1EEvjjjPvPKt*)
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
    ) *(.text .text*)

    /* cpu.o / m4a_gpsp.o run from ITCM. Their *rodata* must stay in RAM
     * (ITCM is code-only). Absolute veneers into XIP that the linker emits
     * in ITCM are patched at runtime (see gba_cache_xip_to_flash). */
    build/cpu.o (.rodata .rodata*)
    build/m4a_hle.o (.rodata .rodata*)
    build/m4a_gpsp.o (.rodata .rodata*)

    *(.glue_7)
    *(.glue_7t)

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
    build/video.o (.text.update_scanline)
    *(.gba_itcm_text*)       /* the scaler (gba_scale.c), PROF's PC sampler (gba_prof.c) */
    /* The drawing code below at its measured place mod 32 (24: runs 20-27,
     * with the PC sampler above it), with or without the sampler. */
    . = ALIGN(. - 24, 32) + 24;
    /* Hot code that ran from the QSPI flash (gba.xip), by the PC sampler
     * (Kirby, 2026-11-14): the plain sprite renderer (the 16-bit, no-blend
     * instance; the other four stay in flash), the tile-layer drivers, and
     * the M4A HLE's guest-memory helpers, called per sample. */
    build/video.o (.text._Z20render_scanline_objsItL8rendtype0EEvjjjPvPKt*)
    build/video.o (.text._Z18tile_render_layers*)
    build/m4a_hle.o (.text.rd_* .text.wr_* .text.win_hold)
    /* The text-layer renderer's hot pair: their unrolled tile loops
     * (video.cc TILE_LOOP) do not fit RAM_EMU. */
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

  __CORE_ITCM_BSS_END__ = __CORE_ITCM_CODE_END__;

  ASSERT(__CORE_ITCM_BSS_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
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
    build/m4a_hle.o (.text .text*)
    build/cheats.o (.text .text*)
    build/gba_idle_loop.o (.text .text*)
    build/bilinear.o (.text .text*)
    build/gba_rec.o (.text .text*)
    build/gba_prof.o (.text .text*)
    build/gba_timing.o (.text .text*)
    build/gba_idle.o (.text .text*)
    build/gba_scale.o (.text .text*)
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
    *(.rodata .rodata*)
    . = ALIGN(4);
    __rodata_gba_end__ = .;
    __xip_gba_end__ = .;
  } >GBA_CODE

  ASSERT(__xip_gba_end__ <= ORIGIN(GBA_CODE) + LENGTH(GBA_CODE),
         "Error: GBA XIP (.xip_gba + .rodata_gba) overflows 512K GBA_CODE")

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
