#include <odroid_system.h>

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include "gw_lcd.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "odroid_overlay.h"
#include "appid.h"
#include "bilinear.h"
#include "error_screens.h"

#ifndef HOST_BUILD
#include "stm32h7xx.h"
#endif

/* Everything above is a normal firmware header (see gw_core_bridge.h's file
 * comment: include AFTER those so macro-substitution rewrites later *uses*,
 * not the extern declarations this file relies on for type checking). */
#ifdef HOST_BUILD
#include <stdlib.h>
#include "host_compat.h"
void gba_pchist_frame(void);   /* gba_pchist.c, GBA_PCHIST */
#else
#include "gw_core_bridge.h"
#endif

uint8_t *odroid_overlay_cache_file_in_flash_relocate(const char *file_path,
                                                     uint32_t *file_size_p, bool byte_swap,
                                                     gw_flash_relocate_cb_t relocate_cb);

#ifndef HOST_BUILD
/* Linker symbols from ld/gba_core.ld (sentinel patch ranges). */
extern uint8_t _GBA_MAIN_CODE_END[];
extern uint8_t _OVERLAY_GBA_BSS_START[];
extern uint8_t __CORE_ITCM_CODE_START__[];
extern uint8_t __CORE_ITCM_CODE_END__[];
#endif

/* ABI: host CPU clock after overclock (see gw_firmware_abi_t). */
extern uint32_t get_SystemCoreClock(void);

/* gpsp. The core's own headers pull in libretro types and register-name macros
 * that collide with CMSIS, so we declare the handful of entry points we use. */
#include "gba_savestate_abi.h"
#include "gba_idle_loop.h"
#include "gba_audio_filter.h"
#include "gba_prof.h"
#include "gba_rec.h"
#include "gba_timing.h"
#include "gba_idle.h"
#include "gba_scale.h"

extern uint32_t  idle_loop_target_pc; /* gpSP's; gba_frontend.c owns the storage */
extern uint32_t  idle_loop_cond;      /* 0 = always burn; 1 = only while the branch loops */
extern uint16_t *gba_screen_pixels; /* the core renders straight into this, RGB565 */
extern uint32_t  execute_cycles;    /* cycles the core wants to run before the next event */
extern uint32_t  skip_next_frame;   /* set and the PPU evaluates but does not draw */
#ifdef GBA_DTCM_BUFFERS
extern uint8_t  *bios_rom;
#define GBA_BIOS_ROM_SIZE (16 * 1024)
bool bios_alloc_dtcm(void *(*alloc)(size_t, size_t));
bool cheats_alloc_dtcm(void *(*alloc)(size_t, size_t));
bool sound_alloc_dtcm(void *(*alloc)(size_t, size_t));
#else
extern uint8_t   bios_rom[16 * 1024];
#define GBA_BIOS_ROM_SIZE sizeof(bios_rom)
#endif
extern uint8_t   gamepak_backup[64 * 1024];   /* a 1 Mbit flash: one bank (gba_backup_bank.c) */
#include "porting/gba_backup_bank.h"
extern const uint8_t open_gba_bios_rom[];

void     init_main(void);
void     init_memory(void);
void     init_sound(void);
void     init_gamepak_buffer(void);
void     reset_gba(void);
void     execute_arm(uint32_t cycles);
void     gba_set_xip_rom(uint8_t *base, uint32_t size);
void     gba_set_keys(uint32_t keys);
uint32_t load_gamepak(const void *info, const char *name, int rtc, int rumble, int serial);
/* load_gamepak's last three arguments. Spelled out here rather than included: gpSP's
 * headers collide with CMSIS (see the note at the top of this file).
 *
 * The two scales are NOT the same, which is the trap. rtc and rumble are a tri-state
 * where 0 means DISABLE and -1 means "ask the cart" (gba_memory.h:25-27). serial is a
 * MODE, where 0 means disabled and "auto" is 6 (serial.h:20-26) — so a -1 there is not
 * "no opinion", it is a serial mode that does not exist. */
#define FEAT_AUTODETECT       (-1)
#define FEAT_DISABLE            0
#define FEAT_ENABLE             1
#define SERIAL_MODE_DISABLED    0
#define SERIAL_MODE_AUTO        6
uint32_t sound_read_samples(int16_t *out, uint32_t frames);
uint32_t sound_fifo_rate_hz(void);
#if CHEAT_CODES == 1
/* gpsp's cheat engine: GameShark / CodeBreaker / Action Replay, 20 slots. */
int  cheat_parse(unsigned index, const char *code);
void cheat_clear(void);
extern uint32_t cheat_master_hook;
#define IDLE_COND_CHEAT 3u        /* cpu.h */
#define GBA_MAX_CHEAT_SLOTS 20
#endif

#define GBA_WIDTH   240
#define GBA_HEIGHT  160
/* The GBA's real frame: 280,896 cycles of a 16.777216 MHz clock. That is 59.7275 fps,
 * and every rate below is derived from it rather than from a rounded 60 — a 0.45%
 * error is a sample buffer lapping itself every nine seconds. */
#define GBA_FRAME_CYCLES  280896.0f
#define GBA_CPU_HZ        16777216.0f
#define GBA_FPS     60          /* only for anything that must name a whole number */
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

/* The mixer runs at the rate the SAI is already set to, so nothing has to be
 * resampled on a budget that has no room for it (gpsp's own default is 65536Hz).
 * The device has one speaker, so the core's stereo pair is folded to mono on the
 * way out — half the samples to touch, and nothing is lost that could be heard. */
#define GBA_SAMPLE_RATE          48000

/* Samples per frame — and NOT 48000/60.
 *
 * A GBA frame is 280,896 cycles of a 16.777216 MHz clock: 16.7427 ms, which is
 * 59.7275 fps, not 60. At 48 kHz that is 803.65 samples a frame, and gpSP produces
 * exactly that many (803 or 804). Every one of them is read each frame
 * (gba_pcm_submit, in chunks of this size): a reader that leaves some behind lets
 * gpSP's 2048-pair ring fill and lap itself, after which sound_read_samples()
 * returns a nonsense count -- a click every nine seconds once. */
#define GBA_AUDIO_FRAMES         804
static int16_t gba_audio_stereo[GBA_AUDIO_FRAMES * 2];

/* The GBA framebuffer. The LCD's own buffers live outside RAM_EMU, but the core
 * renders a 240x160 image that then has to be scaled, so it needs a source.
 *
 * 75 KB does not fit the leftover AHB heap on this firmware (~56 KB), so it
 * lives in DTCM (zero-wait, ~104 KB core pool). The scaler reads it once per
 * frame and the DMA never touches it.
 *
 * One line more than the picture: the OBJ window renders its inside into the
 * line below the one it draws, as scratch (video.cc render_scanline_objs,
 * PIXCOPY), so line 159 needs a line 160. Without it that scratch landed on
 * the BIOS copy allocated next (its IRQ return at 0x34): WarioWare, Inc.'s
 * disco stage then froze in the BIOS reset path, on the device only, and only
 * when that frame was drawn. */
#define GBA_FRAMEBUFFER_BYTES  (GBA_WIDTH * (GBA_HEIGHT + 1) * sizeof(uint16_t))
static uint16_t *gba_framebuffer;

#ifdef GBA_DTCM_BUFFERS
/* Prefer DTCM for hot buffers; fall back to the freeable AHB heap. Do not
 * call dtc_init() — the launcher already bump-allocated emulator tables. */
static void *gba_calloc_dtcm_then_ahb(size_t count, size_t size)
{
    void *p = dtc_calloc(count, size);
    if (p)
        return p;
    return ahb_calloc(count, size);
}
#endif

static odroid_video_frame_t video_frame = {GBA_WIDTH, GBA_HEIGHT, GBA_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

static void blit_emulator(void);

/* ------------------------------------------------------------------- diag ---
 * Where the frame actually goes.
 *
 * The overlay says FPS 40 and CPU 69% — enough to know we are waiting on the audio
 * tick, not enough to know WHAT is late. Emulating the ARM7 and drawing the screen
 * are the only two candidates and they want opposite fixes: the interpreter wants
 * clock and ITCM, the renderer wants to be out of flash. Guessing which cost two
 * builds already. So measure both, and put the answer in the pause menu.
 *
 * Averaged over a second so a single heavy frame does not read as a trend. */
/* The DWT cycle counter, read without clearing it: the PROF build's phase
 * marks (gba_prof.c) measure from the same free-running counter, and
 * clearing it mid-frame would corrupt them. Differences wrap correctly. */
#ifdef HOST_BUILD
static inline uint32_t gba_cyc(void) { return 0; }
#else
static inline uint32_t gba_cyc(void) { return *(volatile uint32_t *)0xE0001004u; }
#endif

typedef struct {
    uint32_t cycles;   /* accumulated since the last publish */
    uint32_t frames;
    uint32_t us;       /* published: microseconds per frame */
} gba_diag_t;

/* execute_arm() is TWO things. gpSP renders the picture from inside it — the PPU runs
 * per scanline, as the ARM7 executes — so "Emulate" is the interpreter AND the
 * renderer added together, and the two want opposite fixes.
 *
 * A skipped frame is what tells them apart: skip_next_frame makes the PPU evaluate the
 * scanline but not draw it. So time execute_arm() separately on frames that drew and
 * frames that did not, and the difference IS the renderer.
 *
 *   Emu+ppu   execute_arm() on a frame that rendered
 *   Emu only  execute_arm() on a frame that skipped rendering
 *   PPU       the difference — what drawing the picture actually costs
 */
static gba_diag_t diag_emu_draw;
static gba_diag_t diag_emu_skip;
static gba_diag_t diag_scale;
static gba_diag_t diag_overlay;
static gba_diag_t diag_wait;
static uint32_t   diag_last_tick;
static char       diag_emu_draw_str[16] = "-";
static char       diag_emu_skip_str[16] = "-";
static char       diag_ppu_str[16]      = "-";
static char       diag_scale_str[16]    = "-";
static char       diag_overlay_str[16]  = "-";
static char       diag_wait_str[16]     = "-";
/* Which of blit_emulator()'s branches actually ran. Scaling and filtering are user
 * settings, and SOFT does not go anywhere near the nearest-neighbour scaler — it
 * clears the whole 153 KB buffer and then runs a bilinear resample. */
static char       diag_path_str[16]    = "-";

static inline void gba_diag_add(gba_diag_t *d, uint32_t cycles)
{
    d->cycles += cycles;
    d->frames++;
}

static void gba_diag_format(gba_diag_t *d, char *out, size_t out_len)
{
    if (d->frames == 0) {
        snprintf(out, out_len, "-");
    } else {
        /* get_SystemCoreClock() is whatever the overclock left us at, so this stays true
         * across the OC levels rather than assuming 280MHz. */
        uint32_t per_frame = d->cycles / d->frames;
        d->us = (uint32_t)((uint64_t)per_frame * 1000000u / get_SystemCoreClock());
        snprintf(out, out_len, "%lu.%02lu ms",
                 (unsigned long)(d->us / 1000), (unsigned long)((d->us % 1000) / 10));
    }
    d->cycles = 0;
    d->frames = 0;
}

static void gba_diag_publish(void)
{
    uint32_t now = HAL_GetTick();
    if (now - diag_last_tick < 1000)
        return;
    diag_last_tick = now;

    gba_diag_format(&diag_emu_draw, diag_emu_draw_str, sizeof(diag_emu_draw_str));
    gba_diag_format(&diag_emu_skip, diag_emu_skip_str, sizeof(diag_emu_skip_str));
    gba_diag_format(&diag_scale, diag_scale_str, sizeof(diag_scale_str));
    gba_diag_format(&diag_overlay, diag_overlay_str, sizeof(diag_overlay_str));
    gba_diag_format(&diag_wait, diag_wait_str, sizeof(diag_wait_str));

    /* What the picture costs: the same emulation, once with the PPU drawing and once
     * without. Only meaningful when frameskip is actually skipping something. */
    if (diag_emu_draw.us > diag_emu_skip.us && diag_emu_skip.us > 0) {
        uint32_t ppu = diag_emu_draw.us - diag_emu_skip.us;
        snprintf(diag_ppu_str, sizeof(diag_ppu_str), "%lu.%02lu ms",
                 (unsigned long)(ppu / 1000), (unsigned long)((ppu % 1000) / 10));
    } else {
        snprintf(diag_ppu_str, sizeof(diag_ppu_str), "no skips");
    }
}

/* ------------------------------------------------------------------ fatal --- */
/* Say which failure it was, and stay on screen while it is read.
 *
 * Not an alert-and-return-to-the-launcher: the two ways loading fails here (no
 * room left in the flash cache for the cart, a cart the core will not take) are
 * different problems with different fixes, and a single "load failed" toast that
 * vanishes tells the player neither. Not a sleep either — deep sleep comes back
 * through gw_sleep.c, whose sdcard_init() then fails, and the last thing left on
 * screen is "No SD CARD found": a message about a card that was never the problem
 * (this cost half a day once, on Super Metroid). */
static void __attribute__((noreturn)) gba_fatal(const char *line_1, const char *line_2)
{
    odroid_dialog_choice_t choices[5];
    int n = 0;
    int ok_sel;

    printf("gba: FATAL %s / %s\n", line_1, line_2 ? line_2 : "");

    choices[n++] = (odroid_dialog_choice_t){0, line_1, "", -1, NULL};
    if (line_2 && line_2[0])
        choices[n++] = (odroid_dialog_choice_t){0, line_2, "", -1, NULL};
    choices[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_SEPARATOR;
    ok_sel = n;
    choices[n++] = (odroid_dialog_choice_t){1, "OK", "", 1, NULL};
    choices[n] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;

    odroid_overlay_dialog("Game Boy Advance", choices, ok_sel, NULL, 0);
    odroid_system_switch_app(0);
    while (1) {
    }
}

/* -------------------------------------------------------------------- XIP ---
 * gpSP is 853 KB of core against a 724 KB pool. The scanline renderer (video.o)
 * and the 16 KB BIOS image are linked at a sentinel address instead, shipped as
 * one file — /cores/gba.xip — cached into QSPI flash, and executed and
 * read straight out of it. Same trick as Super Metroid's sm.xip; the linker
 * script says which object goes where and why.
 *
 * Code and BIOS share the region, and therefore one cache entry, on purpose: the
 * renderer's own rodata sits between them, and a pointer from one cache entry
 * into another goes stale the moment the circular cache evicts one and not the
 * other. As a single blob every such pointer is a sentinel into the blob itself,
 * so one relocation against one base fixes all of them at once.
 *
 * The relocation happens on the way IN — the cache hands each buffer to
 * gba_relocate_xip() before programming it — rather than by rewriting flash that
 * has already been written. A rewrite would have to erase first, and an erase
 * interrupted by a flat battery leaves a blank hole indistinguishable from a
 * finished job. On a cache hit nothing is written and nothing needs to be: the
 * copy in flash was relocated to that same address when it was first stored.
 */
#define GBA_CODE_BASE  0xDEC00000u
#define GBA_XIP_PATH   "/cores/gba.xip"
#define GBA_BIOS_PATH  "/bios/gba/gba_bios.bin"

static uint8_t *g_xip_addr;
static uint32_t g_xip_size;
static int32_t  g_xip_offset;

/* Rewrite every sentinel-range word in [start, end) to where the blob really
 * landed. Thumb bit included, hence the & ~1. */
static int patch_gba_sentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size)
{
    int patched = 0;
    for (uint32_t *p = start; p < end; p++) {
        uint32_t v = *p;
        if ((v & ~1u) >= GBA_CODE_BASE && (v & ~1u) < GBA_CODE_BASE + size) {
            *p = (uint32_t)(v + offset);
            patched++;
        }
    }
    return patched;
}

/* Relocation hook: runs on each buffer of gba.xip on its way into the flash. */
static void gba_relocate_xip(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                             uint8_t *file_address, uint32_t file_size)
{
    (void)offset_in_file;
    int32_t offset = (int32_t)((uint32_t)file_address - GBA_CODE_BASE);
    patch_gba_sentinels((uint32_t *)buffer, (uint32_t *)(buffer + (length & ~3u)), offset, file_size);
    wdog_refresh();
}

/* Where a thing linked into the blob actually ended up. main_gba.o is the one
 * object the sentinel pass below does not walk (it holds GBA_CODE_BASE itself, and
 * a scan that could not tell the constant from a reference would rewrite the very
 * constant it is built on), so the single blob pointer this file holds — the BIOS
 * image — is relocated by hand, here. */
static const void *gba_xip_ptr(const void *sentinel)
{
#ifdef HOST_BUILD
    (void)g_xip_offset;
    return sentinel;
#else
    return (const void *)((uint32_t)sentinel + g_xip_offset);
#endif
}

static bool gba_cache_xip_to_flash(void)
{
#ifdef HOST_BUILD
    /* Desktop build links cold text/rodata into the same binary — no QSPI
     * sidecar and no sentinel patch. gba_xip_ptr() is then an identity. */
    g_xip_addr = NULL;
    g_xip_size = 0;
    g_xip_offset = 0;
    printf("host: XIP skipped (cold code linked into binary)\n");
    return true;
#else
    g_xip_size = 0;
    g_xip_addr = odroid_overlay_cache_file_in_flash_relocate(GBA_XIP_PATH, &g_xip_size, false,
                                                             &gba_relocate_xip);
    if (g_xip_addr == NULL || g_xip_size == 0) {
        printf("gba: %s missing\n", GBA_XIP_PATH);
        return false;
    }
    g_xip_offset = (int32_t)((uint32_t)g_xip_addr - GBA_CODE_BASE);
    printf("gba: xip blob at %p, %lu bytes, offset 0x%08lX\n",
           g_xip_addr, (unsigned long)g_xip_size, (unsigned long)g_xip_offset);

    /* Overlay (RAM_EMU after main_gba): long-call veneers into XIP, plus any
     * literal-pool sentinels. main_gba.o itself is excluded — it owns
     * GBA_CODE_BASE and must not rewrite that constant. */
    int n = patch_gba_sentinels((uint32_t *)_GBA_MAIN_CODE_END,
                                (uint32_t *)_OVERLAY_GBA_BSS_START,
                                g_xip_offset, g_xip_size);
    printf("gba: patched %d sentinel refs in the overlay\n", n);

    /* ITCM holds cpu.o (+ hot helpers). The linker places absolute veneers
     * there for XIP callees (process_cheats, m4a_scan, some video helpers).
     * Those must be relocated too — ITCM is outside the overlay scan. */
    int n_itcm = patch_gba_sentinels((uint32_t *)__CORE_ITCM_CODE_START__,
                                     (uint32_t *)__CORE_ITCM_CODE_END__,
                                     g_xip_offset, g_xip_size);
    printf("gba: patched %d sentinel refs in ITCM\n", n_itcm);

    /* Patch wrote code as data: push D-cache lines out and drop stale I-cache. */
    {
        uint32_t ram_sz = (uint32_t)(_OVERLAY_GBA_BSS_START - _GBA_MAIN_CODE_END);
        uint32_t itcm_sz = (uint32_t)(__CORE_ITCM_CODE_END__ - __CORE_ITCM_CODE_START__);
        if (ram_sz > 0) {
            SCB_CleanDCache_by_Addr((void *)_GBA_MAIN_CODE_END, (int32_t)ram_sz);
            SCB_InvalidateICache_by_Addr((void *)_GBA_MAIN_CODE_END, (int32_t)ram_sz);
        }
        if (itcm_sz > 0) {
            /* ITCM is not D-cached; still invalidate any I-cache fill of it. */
            SCB_InvalidateICache_by_Addr((void *)__CORE_ITCM_CODE_START__, (int32_t)itcm_sz);
        }
        __DSB();
        __ISB();
    }
    return true;
#endif
}

/* Prefer the official BIOS from SD when present, otherwise fall back to the
 * bundled open BIOS in gba.xip. A partial read is treated as invalid and we
 * keep the open BIOS to avoid booting with garbage content. */
static void gba_load_bios(void)
{
    FILE *f = fopen(GBA_BIOS_PATH, "rb");
    if (f != NULL) {
        size_t n = fread(bios_rom, 1, GBA_BIOS_ROM_SIZE, f);
        int extra = fgetc(f);
        fclose(f);
        if (n == GBA_BIOS_ROM_SIZE && extra == EOF) {
            printf("gba: using official BIOS from %s\n", GBA_BIOS_PATH);
            return;
        }
        printf("gba: ignoring %s (expected exactly %u bytes)\n",
               GBA_BIOS_PATH, (unsigned)GBA_BIOS_ROM_SIZE);
    }

    memcpy(bios_rom, gba_xip_ptr(open_gba_bios_rom), GBA_BIOS_ROM_SIZE);
    printf("gba: using bundled open BIOS\n");
}

/* ------------------------------------------------------------------- SRAM --- */
/* The cart's own save — the one the game writes when you save in-game. This is
 * what a Pokemon player actually cares about; a savestate is a convenience on
 * top of it. */
static void gba_SramSave(void)
{
#if defined(GBA_PROF) && !defined(HOST_BUILD)
    gba_prof_quit();   /* the firmware's interrupt table back (the PC sampler) */
#endif
    gba_rec_close();   /* quit / sleep: the recording gets its frame count */
    if (!gba_rec_backup_is_players()) {
        printf("gba: cart save not written: a replay ran this session, the save in RAM is the recording's\n");
        return;
    }
    char *path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        /* the 128 KB image: the bank in RAM and the one on file */
        if (!gba_backup_export(f))
            printf("gba: the cart save could not be written completely to %s\n", path);
        fclose(f);
    }
    free(path);
}

static void gba_SramLoad(void)
{
    char *path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
    gba_backup_reset(path);   /* the other bank's file sits next to the save */
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        gba_backup_import(f, GBA_BACKUP_IMAGE_BYTES);
        fclose(f);
    }
    free(path);
}

/* -------------------------------------------------------------- savestate --- */
/* gpsp builds its savestate as one contiguous bson document, and that document is
 * 416KB because six buffers — iwram, ewram, vram, oam, palette, ioregs — are
 * ~390KB of it. There is no 416KB block free here; the largest is the 300KB LCD
 * pool. So the six go straight to the file and only the rest, 4,333 bytes of it,
 * is ever held in RAM (gba_bulk_regions() / gba_save_state_slim()).
 *
 * Order matters on the way back in: the bulk buffers have to be in place before
 * the slim document is applied, because applying it rebuilds the palette cache
 * from palette_ram — and a load restores state, not the caches derived from it. */
#define GBA_STATE_MAGIC   0x41425347u  /* 'GBAS' */
#define GBA_STATE_VER     1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t slim_len;   /* bytes of the bson document that follow */
    uint32_t bulk_len;   /* bytes of raw buffers after that */
} gba_state_header_t;

static bool gba_SaveState(const char *savePathName)
{
    unsigned nreg = 0;
    const gba_bulk_region_t *rg = gba_bulk_regions(&nreg);
    uint8_t *slim = (uint8_t *)lcd_get_active_buffer();   /* borrow the off-screen buffer */

    lcd_wait_for_vblank();
    gba_save_state_slim(slim);

    uint32_t slim_len = *(uint32_t *)slim;
    uint32_t bulk_len = 0;
    for (unsigned i = 0; i < nreg; i++)
        bulk_len += rg[i].len;

    FILE *f = fopen(savePathName, "wb");
    if (f == NULL)
        return false;

    gba_state_header_t h = {GBA_STATE_MAGIC, GBA_STATE_VER, slim_len, bulk_len};
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
              fwrite(slim, 1, slim_len, f) == slim_len;
    for (unsigned i = 0; ok && i < nreg; i++)
        ok = fwrite(rg[i].ptr, 1, rg[i].len, f) == rg[i].len;

    fclose(f);
    lcd_clear_active_buffer();
    return ok;
}

static bool gba_LoadState(const char *savePathName)
{
    gba_rec_close();   /* a loaded state ends a record/replay */
    unsigned nreg = 0;
    const gba_bulk_region_t *rg = gba_bulk_regions(&nreg);

    /* The main loop is blocked for the whole read below, so the audio DMA would
     * otherwise loop its last buffer — a latched tone — until playback resumes. */
    audio_clear_buffers();

    FILE *f = fopen(savePathName, "rb");
    if (f == NULL)
        return false;

    gba_state_header_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != GBA_STATE_MAGIC ||
        h.version != GBA_STATE_VER || h.slim_len == 0 ||
        h.slim_len > GBA_STATE_SLIM_SIZE) {
        fclose(f);
        return false;
    }

    uint32_t bulk_len = 0;
    for (unsigned i = 0; i < nreg; i++)
        bulk_len += rg[i].len;
    if (h.bulk_len != bulk_len) {   /* a state this build did not write */
        fclose(f);
        return false;
    }

    uint8_t *slim = (uint8_t *)lcd_get_active_buffer();
    bool ok = fread(slim, 1, h.slim_len, f) == h.slim_len;
    for (unsigned i = 0; ok && i < nreg; i++)
        ok = fread(rg[i].ptr, 1, rg[i].len, f) == rg[i].len;
    fclose(f);

    if (!ok)
        return false;

    /* Bulk first, then the document: applying it rebuilds the palette cache out
     * of the palette_ram we just restored. */
    if (!gba_load_state_slim(slim))
        return false;

    lcd_clear_active_buffer();
    return true;
}

static void *gba_Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit_emulator();
    return lcd_get_active_buffer();
}

/* ------------------------------------------------------------------ audio --- */
/* The firmware's SAI ISR plays from this ring (pcm_attach), a DMA half of
 * GBA_DMA_HALF samples at a time; the main loop pushes every sample each
 * frame produced and paces on the ring's fill level (gba_audio_pace).
 *
 * So the loop runs at the rate the samples are played: the SAI's rate /
 * 803.65, the GBA's own 59.7275 Hz (gba_timing.h). It used to fill one
 * fixed 804-sample DMA half per frame (59.70 Hz), with the LCD at 60 Hz
 * (really ~59.64) and blit() sleeping until the last swap showed: the loop
 * then locked onto the slower LCD, never waited for the audio again, and
 * the audio underran (Kirby, 2026-11-14).
 *
 * Short DMA halves: the wait ends at an ISR, so the halves' length is how
 * far the moment the loop draws can wander in the LCD's scan (4.2 ms).
 *
 * GBA_PACE_SPARE frames queue beyond the one playing: a frame that overruns
 * by up to that much plays from them, and lighter frames refill them. */
#define GBA_DMA_HALF     201u            /* samples: 804 / 4 */
#define GBA_RING         4096u
#define GBA_PACE_SPARE   2u
#define GBA_SPF_MAX      GBA_AUDIO_FRAMES
_Static_assert((GBA_RING & (GBA_RING - 1u)) == 0u, "the pcm ring must be a power of two");
static int16_t *gba_ring;                /* GBA_RING samples, AHB heap */
static volatile uint16_t gba_ring_head, gba_ring_tail;
/* 0 while the sound loop is muted (volume 0, start paused): the ISR then
 * plays zeros and leaves the ring alone, so nothing may wait for it to
 * drain (the SNES core's volume-0 hang, 2026-10-28): the loop paces on the
 * DMA's ticks instead */
static uint8_t gba_ring_live = 1;

static uint16_t gba_ring_queued(void)
{
    return (uint16_t)((gba_ring_head - gba_ring_tail) & (GBA_RING - 1u));
}

#if defined(GBA_PROF) && !defined(HOST_BUILD)
uint32_t gba_audio_queued(void) { return gba_ring_queued(); }   /* PROF/pace */
#endif

/* Empty the ring from the main loop. The ISR only ever advances the tail,
 * up to the head, so set head = tail and retry if it moved in between. */
static void gba_ring_drop(void)
{
    uint16_t t;
    do {
        t = gba_ring_tail;
        gba_ring_head = t;
    } while (gba_ring_tail != t);
}

static void gba_ring_push(const int16_t *src, uint32_t n)
{
    const uint16_t mask = (uint16_t)(GBA_RING - 1u);
    uint16_t head = gba_ring_head;
    uint32_t used = gba_ring_queued();
    uint32_t max = GBA_SPF_MAX * (GBA_PACE_SPARE + 2u);   /* only a resume or a speed-up gets here */
    uint32_t take = used >= max ? 0 : (max - used < n ? max - used : n);
    for (uint32_t i = 0; i < take; i++) {
        gba_ring[head] = src[i];
        head = (uint16_t)((head + 1u) & mask);
    }
    gba_ring_head = head;
}

/* (Re)start the audio DMA with the ring attached, at scan line
 * GBA_SYNC_LINE, where the frames draw (gba_lcd_window_wait): the audio wait
 * of a frame on time then already ends near it. The first waits let a
 * re-programmed PLL3 (gba_timing_apply) settle. */
#define GBA_SYNC_LINE 32u
static void gba_audio_start(void)
{
    audio_stop_playing();
    odroid_audio_init(GBA_SAMPLE_RATE);
    audio_clear_buffers();
    gba_ring_head = gba_ring_tail = 0;
    lcd_wait_for_vblank();
    lcd_wait_for_vblank();               /* the line 239 interrupt */
#ifndef HOST_BUILD
    for (uint32_t guard = 0; guard < 2000000u; guard++) {   /* to GBA_SYNC_LINE */
        uint32_t y = lcd_get_pixel_position() & 0xffffu;    /* LTDC CPSR.CYPOS */
        if (y >= GBA_SYNC_LINE && y < 239u)
            break;
    }
#endif
    /* start_playing clears the ring's ownership: attach and enable after */
    audio_start_playing(GBA_DMA_HALF);
    pcm_attach(gba_ring, (int)GBA_RING, &gba_ring_head, &gba_ring_tail);
    pcm_audio_set((int)common_emu_sound_get_volume(), !common_emu_sound_loop_is_muted());
    pcm_audio_enable(1);
}

/* Everything the frame produced into the ring: gpSP's mixer runs at 48 kHz
 * and leaves 803 or 804 stereo pairs a frame (its own carry), read until
 * it has no more. Folded to mono (one speaker), through the console's
 * analog rolloff (gba_audio_filter.h); the ISR applies the volume. */
static void gba_pcm_submit(void)
{
    int vol = (int)common_emu_sound_get_volume();
    int play = !common_emu_sound_loop_is_muted();
    pcm_audio_set(vol, play);
    if (!play && gba_ring_live)
        gba_ring_drop();        /* left queued, it would play stale at the unmute */
    gba_ring_live = (uint8_t)play;

    gba_lpf_configure(sound_fifo_rate_hz());
    for (int chunk = 0; chunk < 4; chunk++) {
        uint32_t got = sound_read_samples(gba_audio_stereo, GBA_AUDIO_FRAMES);
        gba_rec_audio(gba_audio_stereo, (int)got);   /* the machine's output */
        if (got == 0)
            break;
        /* One speaker: fold the pair rather than throw a channel away. Anything
         * panned hard to the side would otherwise vanish. In place: sample i
         * only reads pairs >= i. */
        for (uint32_t i = 0; i < got; i++)
            gba_audio_stereo[i] = (int16_t)(((int32_t)gba_audio_stereo[i * 2] +
                                             gba_audio_stereo[i * 2 + 1]) / 2);
        gba_lpf_apply(gba_audio_stereo, (uint16_t)got);
        if (play)
            gba_ring_push(gba_audio_stereo, got);
        if (got < GBA_AUDIO_FRAMES)
            break;
    }
}

/* The frame wait: while the ring holds the playing frame plus
 * GBA_PACE_SPARE more. A frame late by any fraction of a frame shows up
 * there, so every overrun is repaid by not waiting. Muted, nothing drains
 * the ring: wait one frame's worth of DMA ticks. A speed-up does not wait.
 * Returns 1 if it waited. */
static int gba_audio_pace(void)
{
    static uint32_t slot;
    const uint32_t ticks = (GBA_AUDIO_FRAMES + GBA_DMA_HALF / 2u) / GBA_DMA_HALF;
    const uint16_t target = (uint16_t)(GBA_AUDIO_FRAMES * (GBA_PACE_SPARE + 1u));
    int waited = 0;

    if (odroid_system_get_app()->speedupEnabled != SPEEDUP_1x) {
        slot = dma_counter;
        return 0;
    }
    if (gba_ring_live) {
        /* bounded (~100 ms of interrupts): a stopped DMA must not hang the
         * loop into a watchdog reset with no log (the SNES core's lesson) */
        for (uint32_t guard = 0; guard < 100000u && gba_ring_queued() >= target; guard++) {
            waited = 1;
            wdog_refresh();
            cpumon_sleep();      /* until the next interrupt: an ISR drains the ring */
        }
        slot = dma_counter;
        return waited;
    }
    if ((int32_t)(dma_counter - slot) > (int32_t)(ticks * GBA_PACE_SPARE) ||
        (int32_t)(slot - dma_counter) > (int32_t)ticks)
        slot = dma_counter;      /* a pause, or just muted: start afresh */
    slot += ticks;
    for (uint32_t guard = 0; guard < 100000u && (int32_t)(dma_counter - slot) < 0; guard++) {
        waited = 1;
        wdog_refresh();
        cpumon_sleep();
    }
    return waited;
}

/* ------------------------------------------------------------------ video --- */
/* The nearest-neighbour source column for every destination column. It is the same
 * for all 213 rows, so it is computed once per scaling mode instead of doing a
 * multiply and a shift for each of ~68,000 pixels, every frame. */
static uint16_t nn_xmap[LCD_WIDTH];
static int32_t  nn_xmap_width = -1;

__attribute__((optimize("unroll-loops")))
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    int w1 = video_frame.width;
    int h1 = video_frame.height;
    int w2 = dest_width;
    int h2 = dest_height;

    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (LCD_WIDTH - dest_width) / 2;
    int wpad = (LCD_HEIGHT - dest_height) / 2;

    if (nn_xmap_width != dest_width) {
        int x_ratio = (int)((w1 << 16) / w2) + 1;
        for (int j = 0; j < w2; j++)
            nn_xmap[j] = (uint16_t)((j * x_ratio) >> 16);
        nn_xmap_width = dest_width;
    }

    uint16_t *screen_buf = (uint16_t *)video_frame.buffer;
    uint16_t *dest = lcd_get_active_buffer();

    /* Write every pixel of the buffer, borders included. A separate clear is a
     * ~150KB memset that can overtake the beam and desync the vblank swap. */
    for (int i = 0; i < wpad; i++)
        memset(dest + i * LCD_WIDTH, 0, LCD_WIDTH * sizeof(uint16_t));
    for (int i = wpad + h2; i < LCD_HEIGHT; i++)
        memset(dest + i * LCD_WIDTH, 0, LCD_WIDTH * sizeof(uint16_t));

    /* Two pixels per store.
     *
     * The LCD pool is uncached AND unbuffered (see ._ram_uc in the linker script), so
     * every write to it is a bus round trip the CPU stalls on until it completes. FIT
     * writes 320x213 pixels a frame; one 16-bit store each is 68,160 round trips, and
     * that — not the scaling arithmetic — was 9.1 ms of a 16.67 ms frame. Packing two
     * pixels into one 32-bit store halves the trips.
     *
     * 32-bit, not 64: a Cortex-M7 traps an unaligned STRD, and Super Metroid already
     * died once on exactly that (see the root CLAUDE.md). Rows are 640 bytes, so a row
     * start is always 4-byte aligned, and hpad is even for every scaling mode here —
     * but a 64-bit store would need 8, which is not guaranteed. */
    for (int i = 0; i < h2; i++) {
        uint16_t *row = dest + (i + wpad) * LCD_WIDTH;
        int y2 = ((i * y_ratio) >> 16);
        const uint16_t *src_row = screen_buf + (y2 * w1);

        for (int j = 0; j < hpad; j++)
            row[j] = 0;

        uint32_t *out32 = (uint32_t *)(row + hpad);
        int pairs = w2 >> 1;
        for (int j = 0; j < pairs; j++) {
            uint32_t lo = src_row[nn_xmap[j * 2]];
            uint32_t hi = src_row[nn_xmap[j * 2 + 1]];
            out32[j] = lo | (hi << 16);
        }
        if (w2 & 1)
            row[hpad + w2 - 1] = src_row[nn_xmap[w2 - 1]];

        for (int j = hpad + w2; j < LCD_WIDTH; j++)
            row[j] = 0;
    }
}

static void screen_blit_bilinear(int32_t dest_width)
{
    int hpad = (LCD_WIDTH - dest_width) / 2;
    uint16_t *dest = lcd_get_active_buffer();

    image_t dst_img = {dest_width, LCD_HEIGHT, 2, ((uint8_t *)dest) + hpad * 2};
    image_t src_img = {video_frame.width, video_frame.height, 2, video_frame.buffer};

    if (hpad > 0)
        memset(dest, 0x00, hpad * 2);

    imlib_draw_image(&dst_img, &src_img, 0, 0, LCD_WIDTH,
                     ((float)dest_width) / ((float)video_frame.width),
                     ((float)LCD_HEIGHT) / ((float)video_frame.height),
                     NULL, -1, 255, NULL, NULL, IMAGE_HINT_BILINEAR, NULL, NULL);
}

/* 320 columns, FIT/FULL/CUSTOM with the hard filter: the arcade core's 3:4
 * lane + DMA2D row blend (gba_scale.c), the letterbox rows cleared here;
 * the nearest-neighbour scaler when the scaler's ring did not fit. */
static int gba_scale_ok;
static void screen_blit_320(int32_t dest_height)
{
    if (!gba_scale_ok) {
        screen_blit_nn(LCD_WIDTH, dest_height);
        return;
    }
    uint16_t *dest = lcd_get_active_buffer();
    int wpad = (LCD_HEIGHT - dest_height) / 2;
    if (wpad > 0) {
        memset(dest, 0, (size_t)wpad * LCD_WIDTH * sizeof(uint16_t));
        memset(dest + (wpad + dest_height) * LCD_WIDTH, 0,
               (size_t)(LCD_HEIGHT - wpad - dest_height) * LCD_WIDTH * sizeof(uint16_t));
    }
    gba_scale_320((const uint16_t *)video_frame.buffer, dest + wpad * LCD_WIDTH, dest_height);
}

static void blit_emulator(void)
{
    /* No wait for the LCD in here: a wait timed together with work reads as work
     * (it once made a fixed-size blit look like it cost 2 to 4.5 ms by scene). The
     * main loop drops a picture whose swap is still pending instead. */
    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    /* Which branch ran. SOFT does not touch the nearest-neighbour scaler at all: it
     * clears all 153 KB of the buffer and then bilinear-resamples into it. */
    snprintf(diag_path_str, sizeof(diag_path_str), "%s/%s",
             scaling == ODROID_DISPLAY_SCALING_OFF  ? "off"
           : scaling == ODROID_DISPLAY_SCALING_FIT  ? "fit"
           : scaling == ODROID_DISPLAY_SCALING_FULL ? "full" : "cust",
             filtering == ODROID_DISPLAY_FILTER_SOFT ? "soft" : "hard");

    static odroid_display_scaling_t last_scaling = -1;
    if (scaling != last_scaling) {
        lcd_clear_buffers();
        last_scaling = scaling;
    }

    /* 240x160 is 3:2. Filling the 240px height would need 360px of width, which
     * the 320px panel does not have — so FIT fills the width instead and letter-
     * boxes: 320x213. */
    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        screen_blit_nn(GBA_WIDTH, GBA_HEIGHT);   /* native, centred */
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT) {
            lcd_clear_active_buffer();           /* bilinear does not fill the borders */
            screen_blit_bilinear(LCD_WIDTH);
        } else {
            screen_blit_320(213);
        }
        break;
    case ODROID_DISPLAY_SCALING_FULL:
    case ODROID_DISPLAY_SCALING_CUSTOM:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT)
            screen_blit_bilinear(LCD_WIDTH);
        else
            screen_blit_320(LCD_HEIGHT);
        break;
    default:
        screen_blit_320(213);
        break;
    }
}

/* The pause menu's repaint: the picture the game left, under the menu. The
 * main loop presents its frames itself (app_main_gba), timed part by part. */
static void blit(void)
{
    lcd_sleep_while_swap_pending();
    blit_emulator();
    common_ingame_overlay();
}

/* ------------------------------------------------------------------ input --- */
/* KEYINPUT bit order, and what gpsp's gba_set_keys() expects: a set bit is held. */
#define GBA_KEY_A      0x0001
#define GBA_KEY_B      0x0002
#define GBA_KEY_SELECT 0x0004
#define GBA_KEY_START  0x0008
#define GBA_KEY_RIGHT  0x0010
#define GBA_KEY_LEFT   0x0020
#define GBA_KEY_UP     0x0040
#define GBA_KEY_DOWN   0x0080
#define GBA_KEY_R      0x0100
#define GBA_KEY_L      0x0200

static void gba_input_read(odroid_gamepad_state_t *joystick)
{
    uint32_t keys = 0;
    if (joystick->values[ODROID_INPUT_UP])     keys |= GBA_KEY_UP;
    if (joystick->values[ODROID_INPUT_DOWN])   keys |= GBA_KEY_DOWN;
    if (joystick->values[ODROID_INPUT_LEFT])   keys |= GBA_KEY_LEFT;
    if (joystick->values[ODROID_INPUT_RIGHT])  keys |= GBA_KEY_RIGHT;
    if (joystick->values[ODROID_INPUT_A])      keys |= GBA_KEY_A;
    if (joystick->values[ODROID_INPUT_B])      keys |= GBA_KEY_B;
    if (joystick->values[ODROID_INPUT_START])  keys |= GBA_KEY_START;
    if (joystick->values[ODROID_INPUT_SELECT]) keys |= GBA_KEY_SELECT;
    /* The unit has no shoulder buttons. Y/X stand in for L/R — every GBA game
     * that uses them uses them, and there is nowhere else to put them. */
    if (joystick->values[ODROID_INPUT_X])      keys |= GBA_KEY_R;
    if (joystick->values[ODROID_INPUT_Y])      keys |= GBA_KEY_L;

    /* recorded, or replaced by the replay (gba_rec.c) */
    gba_set_keys(gba_rec_input((uint16_t)keys));
}

/* gw_sleep() restores the *settings* OC level on wake, but GBA always forces
 * level 4 during gameplay. Without this the core keeps running at the (slower
 * settings clock after a sleep/wake cycle. Re-apply the boost and reinit
 * audio (SystemClock_Config also reprograms the audio PLL). */
static void gba_sleep_wake_up(void)
{
    SystemClock_Config(3);
    gba_timing_apply();     /* the resume may have reset the LCD rate */
    gba_audio_start();      /* and restarts the DMA: realign the phase */
}

/* ------------------------------------------------------- record / replay --- */
static const uint8_t *gba_rom;
static uint32_t gba_rom_size;

/* Only the codes the user ticked on for this ROM go in. After a reset: parsing
 * a code installs the hook the engine watches for, and a reset throws it away. */
static void gba_apply_cheats(void)
{
#if CHEAT_CODES == 1
    cheat_clear();
    unsigned slot = 0;
    for (int i = 0; i < ACTIVE_FILE->cheat_count && slot < GBA_MAX_CHEAT_SLOTS; i++) {
        if (odroid_settings_ActiveGameGenieCodes_is_enabled(ACTIVE_FILE->path, i))
            cheat_parse(slot++, ACTIVE_FILE->cheat_codes[i]);
    }
    /* A master hook takes the interpreter's one special-PC slot (cpu.cc,
     * IDLE_COND_CHEAT): the cheats run there, and the idle skip is off. */
    if (cheat_master_hook != 0xFFFFFFFFu) {
        idle_loop_target_pc = cheat_master_hook;
        idle_loop_cond = IDLE_COND_CHEAT;
        printf("gba: cheat hook at 0x%08lX (no idle skip while cheats are on)\n",
               (unsigned long)cheat_master_hook);
    }
#endif
}

/* what a recording is tied to besides its inputs (gba_rec.c): the cart, and
 * the BIOS and cheats it ran with */
static uint32_t gba_rom_hash(void)
{
    uint32_t n = gba_rom_size < 0x10000u ? gba_rom_size : 0x10000u;
    return gba_rec_fnv(gba_rom, n, GBA_REC_FNV0);
}

static uint32_t gba_setup_hash(void)
{
    uint32_t h = gba_rec_fnv(bios_rom, GBA_BIOS_ROM_SIZE, GBA_REC_FNV0);
#if CHEAT_CODES == 1
    for (int i = 0; i < ACTIVE_FILE->cheat_count; i++) {
        if (odroid_settings_ActiveGameGenieCodes_is_enabled(ACTIVE_FILE->path, i))
            h = gba_rec_fnv(ACTIVE_FILE->cheat_codes[i], (uint32_t)strlen(ACTIVE_FILE->cheat_codes[i]), h);
    }
#endif
    return h;
}

/* the record/replay's power-on: the machine as the start-up reset leaves it
 * (gpSP's reset, then the cheats, the output filter); the cart save and the
 * RTC base are the caller's (gba_rec.c) */
static void gba_power_on(void)
{
    reset_gba();
    gba_idle_reset();
    gba_apply_cheats();
    gba_lpf_reset();
}

static void gba_rec_toggle(int mode)
{
    if (gba_rec_mode() == mode)
        gba_rec_close();
    else
        gba_rec_start(mode, ACTIVE_FILE->path, gba_rom_hash(), gba_setup_hash(), gba_power_on);
}

/* Actions, not toggles: the label says what selecting the entry does. */
static bool rec_record_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                          uint32_t repeat)
{
    (void)option; (void)repeat;
    if (event == ODROID_DIALOG_ENTER)
        gba_rec_toggle(GBA_REC_RECORD);
    return event == ODROID_DIALOG_ENTER;
}

static bool rec_replay_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                          uint32_t repeat)
{
    (void)option; (void)repeat;
    if (event == ODROID_DIALOG_ENTER)
        gba_rec_toggle(GBA_REC_REPLAY);
    return event == ODROID_DIALOG_ENTER;
}

/* PHASE: where in the LCD's scan a frame draws and swaps. A swap only shows
 * at the start of vertical blanking (scan line 248 of 256). If the draw
 * straddled that line, every swap would miss it, still be pending at the
 * next frame's check, and every other picture would be dropped (30 fps --
 * the Neo Geo core's bug).
 *
 * The audio wait alone does not hold the phase still: it ends at one of the
 * four DMA interrupts of a frame (GBA_DMA_HALF), and after a run of late
 * frames the loop can settle on any of the four, 4.2 ms apart -- one of
 * them across the blanking (Kirby, 2026-11-14: 40-57 of 120 pictures dropped
 * until a watchdog restarted the audio). So a frame that was on time also
 * waits until the last swap has shown and the scan is between GBA_SYNC_LINE
 * and GBA_DRAW_LAST, then draws: the draw is pinned to the LCD every frame.
 * The LCD runs at the loop's rate (gba_timing.c), so this wait settles to a
 * constant and the audio ring absorbs it. A late frame does not wait (it
 * draws if the last swap has shown, else drops its picture). Bounded: a
 * stopped LCD must not hang the loop. */
#define GBA_DRAW_LAST 200u   /* scan line: the draw (~15 lines) swaps well before 248 */
static void gba_lcd_window_wait(void)
{
#ifndef HOST_BUILD
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < 40u) {
        if (!lcd_is_swap_pending()) {
            uint32_t y = lcd_get_pixel_position() & 0xffffu;   /* LTDC CPSR.CYPOS */
            if (y >= GBA_SYNC_LINE && y < GBA_DRAW_LAST)
                return;
        }
        wdog_refresh();
        cpumon_sleep();          /* until the next interrupt: SysTick, the audio DMA */
    }
#endif
}

/* ------------------------------------------------------------------- main --- */
void app_main_gba(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    odroid_gamepad_state_t joystick;

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    /* 1674, not 1667. A GBA frame is 16.7427 ms — 59.7275 fps, not 60 — and the pacing
     * loop's idea of a frame has to be the same one the audio DMA enforces, or the two
     * fight and the loser is a dropped frame. The LCD is told 60 because its refresh
     * rate is a hardware setting with no 59.7 to choose. */
    common_emu_state.frame_time_10us = (uint16_t)(100000.0f * GBA_FRAME_CYCLES / GBA_CPU_HZ + 0.5f);
    lcd_set_refresh_rate(60);

    /* Hot buffers: FB in DTCM (75 KB, will not fit leftover AHB). BIOS and
     * the sound ring prefer DTCM too; cheats are cold and stay on AHB. */
    gba_framebuffer = dtc_malloc(GBA_FRAMEBUFFER_BYTES);
    if (gba_framebuffer == NULL)
        gba_fatal("Out of DTCM", "The 75KB framebuffer could not be allocated");
    memset(gba_framebuffer, 0, GBA_FRAMEBUFFER_BYTES);

#ifdef GBA_DTCM_BUFFERS
    if (!bios_alloc_dtcm(gba_calloc_dtcm_then_ahb))
        gba_fatal("Out of memory", "The 16KB BIOS buffer could not be allocated");
    if (!sound_alloc_dtcm(gba_calloc_dtcm_then_ahb))
        gba_fatal("Out of memory", "The sound buffer could not be allocated");
#endif

    video_frame.buffer = gba_framebuffer;
    gba_screen_pixels = gba_framebuffer;

    odroid_system_init(APPID_GBA, GBA_SAMPLE_RATE);
    odroid_system_emu_init(&gba_LoadState, &gba_SaveState, &gba_Screenshot,
                           NULL, &gba_sleep_wake_up, &gba_SramSave, NULL);

    /* Native 240x160 is a small island on a 320x240 panel; FIT is the sane
     * first-run default. Any choice the user makes afterwards is theirs. */
    if (odroid_display_get_scaling_mode() == ODROID_DISPLAY_SCALING_OFF)
        odroid_display_set_scaling_mode(ODROID_DISPLAY_SCALING_FIT);

    /* Cache gba.xip BEFORE overclock / audio DMA. The progress UI waits on
     * lcd_sleep_while_swap_pending(); a PLL rewrite can leave LTDC SRCR stuck
     * and freeze the bar at 0%. Firmware GBA uses OC3; level ≥4 falls through
     * to "no OC" in SystemClock_Config. */
    if (!gba_cache_xip_to_flash())
        gba_fatal("Missing " GBA_XIP_PATH, "Re-run the retro-go_update.bin update");

#if defined(GBA_DTCM_BUFFERS) && (CHEAT_CODES == 1)
    if (!cheats_alloc_dtcm(ahb_calloc))
        gba_fatal("Out of AHB SRAM", "The cheat table could not be allocated");
#endif

    /* The scaler's row ring (gba_scale.c): 3.2 KB of AHB, DMA2D reads it. */
    gba_scale_ok = gba_scale_init();
#ifdef HOST_BUILD
    if (getenv("GBA_NN"))
        gba_scale_ok = 0;              /* host: the nearest-neighbour scaler, for comparisons */
#endif

    /* The ISR's sample ring (gba_audio_start): 8 KB, cold enough for AHB. */
    gba_ring = (int16_t *)ahb_calloc(GBA_RING, sizeof(int16_t));
    if (gba_ring == NULL)
        gba_fatal("Out of AHB SRAM", "The 8KB audio ring could not be allocated");

    /* Level 3 (~353 MHz): same as the in-tree firmware GBA core. */
    SystemClock_Config(3);
    audio_start_playing(GBA_DMA_HALF);
    gba_lpf_reset();

    init_main();
    init_memory();
    init_sound();

    gba_load_bios();
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    {
        char *save = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
        gba_backup_reset(save);
        free(save);
    }

    /* The ROM is up to 32MB and stays in external flash, memory-mapped. Nothing
     * is copied into RAM and nothing is paged: the core reads the cart where it
     * lies. Page 0 is the exception — an RTC cart has its GPIO registers *written*
     * into "ROM" at 0x080000C4, and flash does not take writes, so the core keeps
     * a RAM shadow of it. Ruby, Sapphire and Emerald all keep time that way. */
    uint32_t rom_size = 0;
    uint8_t *rom = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &rom_size, false);
    if (rom == NULL || rom_size == 0)
        gba_fatal("Could not cache the ROM in flash", "The cart may be larger than the free flash");
    gba_set_xip_rom(rom, rom_size);
    gba_rom = rom;
    gba_rom_size = rom_size;
    init_gamepak_buffer();

    /* force_rtc, force_rumble, force_serial — and only the first one changes.
     *
     * RTC: -1, not 0. Zero is not "no opinion" here, it is FEAT_DISABLE — an override
     * that says turn the clock OFF. gpSP recognised Ruby, set rtc_enabled from the
     * cart, and then the 0 we passed switched it straight back off. The game reported
     * that its internal battery had run dry, which for a cart with no working clock is
     * exactly true. Ruby, Sapphire and Emerald all keep time: berries grow, tides turn.
     * Let the cart decide.
     *
     * Rumble: 0 stays. There is no motor in a Game & Watch, so emulating the pak is
     * work with nowhere to land.
     *
     * Serial: 0 stays, and note it is NOT the same tri-state — it is a serial MODE
     * (serial.h:20-26), where 0 is SERIAL_MODE_DISABLED and "auto" is 6. There is no
     * link port either, and leaving it disabled also keeps gba_over.h from turning on
     * Pokemon's serial emulation, which would be per-frame work for a cable that does
     * not exist. */
    if (load_gamepak(NULL, ACTIVE_FILE->path,
                     FEAT_AUTODETECT,        /* rtc: ask the cart          */
                     FEAT_DISABLE,           /* rumble: no motor           */
                     SERIAL_MODE_DISABLED) != 0)   /* serial: no link port */
        gba_fatal("Not a Game Boy Advance ROM", "The header did not check out");

    /* The idle skip: the generic one (gba_idle.c) for every game. It finds the
     * loops a game waits in while it runs and skips their passes exactly, up to
     * the next hardware event; on 47 games of a 150-game set that the per-game
     * tables cover, it removed 39-88% more instructions than their entries in
     * 37 and never fewer, and the tables' slice burn is not exact. It replaces
     * gpSP's gba_over.h entry (set by load_gamepak) and the two tables below,
     * which GBA_IDLE_TABLES=1 (make) brings back instead. */
#ifdef GBA_IDLE_TABLES
    /* After load_gamepak, on purpose: it is what sets idle_loop_target_pc from
     * gpSP's own gba_over.h, and ours has to win. A game with no busy-wait PC
     * spins through the whole 280,896-cycle frame instead of doing ~75,000 cycles
     * of work and stopping — so this is not a tuning knob, it is the difference
     * between full speed and no chance of it. See gba_idle_loop.c. */
    uint32_t idle_pc = gba_idle_loop_lookup((const char *)&rom[0xAC]);
    if (idle_pc != 0) {
        idle_loop_target_pc = idle_pc;
        printf("gba: idle loop at 0x%08lX\n", (unsigned long)idle_pc);
    }

    /* The OTHER kind of wait: a raster poll — `ldrh rN,[VCOUNT]; cmp; bne` —
     * that the classic always-burn skip must not touch, because these games'
     * delay code CALLS the poll in a counted burst and on hardware ~120 calls
     * fit inside the matching scanline; burn every arrival and a six-frame
     * intro becomes seven hundred (proven: Super Robot Taisen D froze). So the
     * target is the poll's closing branch and the slice burns only while the
     * branch will loop (IDLE_COND_WHEN_NE; the check costs nothing off-match).
     *
     * Hand-curated, one entry per game PROVEN on the host A/B rig
     * (tools/gba_m4a/prove_main.c, IDLE_PC= + IDLE_COND=ne): screens 99.8%
     * identical at a two-frame shift, interpreted instructions -15..-17%.
     * Only for carts with NO entry in the generated idle table — the two
     * waits would otherwise fight over one target slot. */
    static const struct { char code[5]; uint32_t branch_pc; } vcount_polls[] = {
        { "A6SJ", 0x8932178 },   /* Super Robot Taisen D  (-15.2%) */
        { "ATIJ", 0x858f088 },   /* Tennis no Ouji-sama Genius Boys Academy (-16.8%) */
    };
    if (idle_pc == 0) {
        for (size_t i = 0; i < sizeof(vcount_polls) / sizeof(vcount_polls[0]); i++) {
            if (memcmp(vcount_polls[i].code, &rom[0xAC], 4) == 0) {
                idle_loop_target_pc = vcount_polls[i].branch_pc;
                idle_loop_cond = 1;   /* IDLE_COND_WHEN_NE */
                printf("gba: vcount poll at 0x%08lX (cond NE)\n",
                       (unsigned long)idle_loop_target_pc);
                break;
            }
        }
    }

#else
    gba_idle_enable();
#endif

    gba_SramLoad();
    reset_gba();

    gba_apply_cheats();   /* after the reset, which would throw the hooks away */

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    common_emu_enable_dwt_cycles();
    /* the real frame rate, the LCD at it, then the audio DMA at a fixed point
     * of the LCD's scan (gba_audio_start) -- last, after every slow load */
    gba_timing_apply();
    gba_audio_start();
#if defined(GBA_PROF) && !defined(HOST_BUILD)
    gba_prof_set_xip(g_xip_addr, g_xip_size);   /* the PC sampler maps gba.xip's code */
#endif
    GBA_PROF_INIT(GBA_AUDIO_FRAMES, GBA_SAMPLE_RATE);
#ifdef HOST_BUILD
    /* GBA_RECORD=1 / GBA_REPLAY=1: from power-on at start-up, or at loop
     * frame GBA_REC_AT (a replay started mid-session must log the same
     * REC/hash lines); the recording is closed at exit */
    uint32_t host_loop_frame = 0, host_rec_at = 0;
    int host_rec_mode = getenv("GBA_RECORD") ? GBA_REC_RECORD :
                        getenv("GBA_REPLAY") ? GBA_REC_REPLAY : GBA_REC_OFF;
    if (getenv("GBA_REC_AT"))
        host_rec_at = (uint32_t)atoi(getenv("GBA_REC_AT"));
    atexit(gba_rec_close);
#endif

    while (true) {
        wdog_refresh();

        gba_timing_frame_tick();
        bool drawFrame = common_emu_frame_loop();
        skip_next_frame = drawFrame ? 0 : 1;

#if defined(GBA_AUTOREPLAY) && !defined(HOST_BUILD)
        {   /* make AUTOREPLAY=1 (tests, gwemu): replay "<rom>.rec" from the
             * first frame, no pause menu needed */
            static int auto_done;
            if (!auto_done) {
                auto_done = 1;
                gba_rec_toggle(GBA_REC_REPLAY);
            }
        }
#endif
#ifdef HOST_BUILD
        if (host_rec_mode != GBA_REC_OFF && host_loop_frame == host_rec_at) {
            gba_rec_start(host_rec_mode, ACTIVE_FILE->path, gba_rom_hash(), gba_setup_hash(),
                          gba_power_on);
            host_rec_mode = GBA_REC_OFF;
        }
        host_loop_frame++;
#endif
        odroid_input_read_gamepad(&joystick);
        {
            /* Read-only (enabled = -1): who spends the frame. If Emulate dominates,
             * the answer is clock and the interpreter; if Draw does, the renderer
             * and where its code lives. Then record / replay (gba_rec.c). */
            int rm = gba_rec_mode();
            odroid_dialog_choice_t options[] = {
                {0, "Emu+ppu",  diag_emu_draw_str, -1, NULL},
                {0, "Emu only", diag_emu_skip_str, -1, NULL},
                {0, " = PPU",   diag_ppu_str,      -1, NULL},
                {0, "Scale",    diag_scale_str,    -1, NULL},
                {0, "Ovl",      diag_overlay_str,  -1, NULL},
                {0, "Wait",     diag_wait_str,     -1, NULL},
                {0, "Path",     diag_path_str,     -1, NULL},
                {104, rm == GBA_REC_RECORD ? "Stop recording" : "Record input", NULL, 1, &rec_record_cb},
                {105, rm == GBA_REC_REPLAY ? "Stop replay" : "Replay input", NULL, 1, &rec_replay_cb},
                ODROID_DIALOG_CHOICE_LAST
            };
            common_emu_input_loop(&joystick, options, &blit);
        }
        common_emu_input_loop_handle_turbo(&joystick);

        gba_input_read(&joystick);
        GBA_PROF_MARK(GBA_PH_FRAMECTL);

        /* execute_arm() returns when the frame driver says the frame is done — and it
         * has drawn the picture into gba_framebuffer along the way, unless
         * skip_next_frame said not to. */
        uint32_t t = gba_cyc();
        execute_arm(execute_cycles);
        gba_diag_add(drawFrame ? &diag_emu_draw : &diag_emu_skip, gba_cyc() - t);
        GBA_PROF_MARK(GBA_PH_EMU);

        /* Emulate, then audio, then wait, then present: the wait ends at a
         * fixed point of the LCD's scan (gba_audio_start), so the swap does. */
        gba_pcm_submit();
        gba_rec_frame_done();
#ifdef GBA_PCHIST
        gba_pchist_frame();
#endif
        GBA_PROF_MARK(GBA_PH_AUDIO);

        t = gba_cyc();
        int waited = gba_audio_pace();
        /* Draw at a fixed point of the scan: a frame on time always, a late
         * one too while the audio ring still holds 2 frames to spare -- it
         * then shows one LCD refresh later instead of being dropped because
         * it landed in the same refresh as the last picture (Kirby: 292
         * pictures dropped while the ring sat at ~2.6 frames). Below that,
         * a late frame does not wait: it draws if the last swap has shown,
         * else drops its picture, and the emulation catches up. */
        if (drawFrame && (waited ||
                          (gba_ring_live && gba_ring_queued() >= 2u * GBA_AUDIO_FRAMES &&
                           odroid_system_get_app()->speedupEnabled == SPEEDUP_1x)))
            gba_lcd_window_wait();
        gba_diag_add(&diag_wait, gba_cyc() - t);
        GBA_PROF_MARK(GBA_PH_WAIT);

        int drawn = drawFrame, vskip = 0;
#ifndef HOST_BUILD
        /* Never draw while the LCD still shows the back buffer: the last
         * lcd_swap() takes effect at the LCD's next vertical blank. A frame on
         * time, or late with audio to spare, has waited for it
         * (gba_lcd_window_wait); one that finds it still pending DROPS its
         * picture rather than wait, so it does not fall further behind the
         * audio. */
        if (drawn && lcd_is_swap_pending()) {
            drawn = 0;
            vskip = 1;
        }
#endif
        if (drawn) {
            t = gba_cyc();
            blit_emulator();
            gba_diag_add(&diag_scale, gba_cyc() - t);
            GBA_PROF_MARK(GBA_PH_SCALE);
            t = gba_cyc();
            common_ingame_overlay();
            gba_diag_add(&diag_overlay, gba_cyc() - t);
            lcd_swap();
            GBA_PROF_MARK(GBA_PH_PRESENT);
        }
        gba_diag_publish();
        GBA_PROF_FRAME_END(drawn, !drawFrame, vskip, waited);
    }
}
