/*
 * gba_rec.c -- input record / replay, for repeatable sessions: the same
 * frames for a PROF A/B, and the state hashes that prove an optimisation
 * did not change the emulation.
 *
 * A recording is the GBA keys of every emulated frame, AFTER the G&W button
 * mapping (Y = L, X = R), from a power-on, in "<rom path without
 * extension>.rec" next to the ROM:
 *   header (32 bytes): "GBRC", version 1, flags (0), FNV-1a of the cart
 *   header and the first 64 KiB of the ROM, FNV-1a of the setup (BIOS,
 *   cheats), frame count (0 until the recording is closed), the RTC's base
 *   time (the cart clock runs from it, counted in emulated frames);
 *   then the cart save (backup memory, 128 KiB) as it was at the power-on,
 *   then one little-endian uint16 per frame: the KEYINPUT bits, set = held
 *   (A 0x001, B 0x002, SELECT 0x004, START 0x008, right 0x010, left 0x020,
 *   up 0x040, down 0x080, R 0x100, L 0x200).
 * Record or replay from the pause menu (device and host) or GBA_RECORD=1 /
 * GBA_REPLAY=1 at start-up (host). Every 120 frames both log
 *   REC/hash frame=N iwram ewram vram oam pal io cpu audio
 * hashes of the six RAM regions a savestate streams, the ARM7's registers
 * and the audio output so far: two runs of one recording must log the same
 * lines (host and device alike).
 *
 * A replay puts the recording's cart save in RAM, so the game sees what it
 * saw when it was recorded. That save is then no longer the player's: for
 * the rest of the session it is not written back to the SD
 * (gba_rec_backup_is_players), and the player's save file stays as it was.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gba_rec.h"
#include "gba_savestate_abi.h"
#include "gw_malloc.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#endif

#define REC_MAGIC   0x43524247u      /* "GBRC" */
#define REC_VERSION 1u
#define REC_BUF     1024u

typedef struct {
    uint32_t magic;
    uint16_t version, flags;
    uint32_t rom_hash, setup_hash, frames;
    uint32_t rtc_lo, rtc_hi;
    uint32_t backup_len;
} rec_header_t;

/* gpSP */
extern uint8_t gamepak_backup[1024 * 128];
extern uint32_t reg[64];
extern uint32_t spsr[6];
extern uint32_t reg_mode[7][7];
int64_t gba_rtc_base_get(void);
void gba_rtc_base_set(int64_t t);

static int rec_mode, backup_replaced;
static rec_header_t rec_hdr;         /* a recording's, rewritten at the close */
static FILE *rec_f;
static uint32_t rec_frame, rec_rom_hash, rec_setup_hash, rec_audio_h;
static uint16_t *rec_buf;            /* AHB heap at the first use */
static uint32_t rec_n, rec_pos;
static char rec_file[300];

uint32_t gba_rec_fnv(const void *p, uint32_t n, uint32_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}
#define fnv gba_rec_fnv
#define FNV0 GBA_REC_FNV0

int gba_rec_mode(void) { return rec_mode; }
uint32_t gba_rec_frame(void) { return rec_frame; }
const char *gba_rec_file(void) { return rec_file; }
int gba_rec_backup_is_players(void) { return !backup_replaced; }

static void rec_flush(void)
{
    if (rec_f && rec_n)
        fwrite(rec_buf, sizeof(uint16_t), rec_n, rec_f);
    rec_n = 0;
}

void gba_rec_close(void)
{
    if (!rec_f) {
        rec_mode = GBA_REC_OFF;
        return;
    }
    if (rec_mode == GBA_REC_RECORD) {
        rec_flush();
        rec_hdr.frames = rec_frame;
        fseek(rec_f, 0, SEEK_SET);
        fwrite(&rec_hdr, sizeof rec_hdr, 1, rec_f);
        printf("REC: recorded %lu frames in %s\n", (unsigned long)rec_frame, rec_file);
    } else {
        printf("REC: replay stopped at frame %lu\n", (unsigned long)rec_frame);
    }
    fclose(rec_f);
    rec_f = NULL;
    rec_mode = GBA_REC_OFF;
}

static void rec_path(char *out, size_t cap, const char *rom_path)
{
    snprintf(out, cap, "%s", rom_path ? rom_path : "gba");
    char *dot = strrchr(out, '.'), *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    strncat(out, ".rec", cap - strlen(out) - 1);
}

static int rec_fail(const char *why)
{
    printf("REC: %s: %s\n", rec_file, why);
    if (rec_f)
        fclose(rec_f);
    rec_f = NULL;
    return 0;
}

int gba_rec_start(int mode, const char *rom_path, uint32_t rom_hash, uint32_t setup_hash,
                  void (*power_on)(void))
{
    gba_rec_close();
    if (!rec_buf)
        rec_buf = (uint16_t *)ahb_calloc(REC_BUF, sizeof(uint16_t));
    if (!rec_buf) {
        printf("REC: no memory for the input buffer\n");
        return 0;
    }
    rec_path(rec_file, sizeof rec_file, rom_path);
    rec_rom_hash = rom_hash;
    rec_setup_hash = setup_hash;
    if (mode == GBA_REC_RECORD) {
        int64_t rtc = gba_rtc_base_get();
        rec_header_t h = { REC_MAGIC, REC_VERSION, 0, rom_hash, setup_hash, 0,
                           (uint32_t)(uint64_t)rtc, (uint32_t)((uint64_t)rtc >> 32),
                           sizeof(gamepak_backup) };
        rec_hdr = h;
        rec_f = fopen(rec_file, "wb");
        if (!rec_f)
            return rec_fail("cannot create it");
        if (fwrite(&h, sizeof h, 1, rec_f) != 1 ||
            fwrite(gamepak_backup, 1, sizeof(gamepak_backup), rec_f) != sizeof(gamepak_backup))
            return rec_fail("cannot write it");
    } else {
        rec_header_t h;
        rec_f = fopen(rec_file, "rb");
        if (!rec_f)
            return rec_fail("no recording");
        if (fread(&h, sizeof h, 1, rec_f) != 1 || h.magic != REC_MAGIC ||
            h.version != REC_VERSION || h.backup_len != sizeof(gamepak_backup))
            return rec_fail("not a version 1 recording");
        if (h.rom_hash != rom_hash) {
            printf("REC: recorded with another game (%08lx, this one %08lx)\n",
                   (unsigned long)h.rom_hash, (unsigned long)rom_hash);
            return rec_fail("wrong game");
        }
        if (h.setup_hash != setup_hash)
            return rec_fail("recorded with another BIOS or other cheats");
        if (fread(gamepak_backup, 1, sizeof(gamepak_backup), rec_f) != sizeof(gamepak_backup))
            return rec_fail("truncated");
        backup_replaced = 1;
        gba_rtc_base_set((int64_t)((uint64_t)h.rtc_lo | ((uint64_t)h.rtc_hi << 32)));
        printf("REC: replaying %lu frames from %s (the cart save is the recording's until the game is reloaded)\n",
               (unsigned long)h.frames, rec_file);
    }
    rec_mode = mode;
    rec_frame = 0;
    rec_audio_h = FNV0;
    rec_n = rec_pos = 0;
    power_on();
    printf("REC: %s from power-on, game %08lx, setup %08lx\n",
           mode == GBA_REC_RECORD ? "recording" : "replay",
           (unsigned long)rom_hash, (unsigned long)setup_hash);
    return 1;
}

#ifdef HOST_BUILD
#include <stdlib.h>
/* GBA_INPUT_RAW=<file> (host): the "live" keys of loop frame k are the
 * file's k-th little-endian uint16, so a recording can be made without a
 * keyboard; 0 after its end */
static uint16_t host_scripted(uint16_t live)
{
    static FILE *f;
    static int tried;
    uint16_t w = 0;
    if (!tried) {
        const char *e = getenv("GBA_INPUT_RAW");
        tried = 1;
        if (e && e[0] && !(f = fopen(e, "rb")))
            printf("REC: cannot open GBA_INPUT_RAW %s\n", e);
    }
    if (!f)
        return live;
    if (fread(&w, sizeof w, 1, f) != 1)
        w = 0;
    return w;
}
#endif

uint16_t gba_rec_input(uint16_t live)
{
#ifdef HOST_BUILD
    live = host_scripted(live);
#endif
    if (rec_mode == GBA_REC_RECORD) {
        rec_buf[rec_n++] = live;
        if (rec_n == REC_BUF)
            rec_flush();
        return live;
    }
    if (rec_mode == GBA_REC_REPLAY) {
        if (rec_pos == rec_n) {
            rec_n = (uint32_t)fread(rec_buf, sizeof(uint16_t), REC_BUF, rec_f);
            rec_pos = 0;
            if (!rec_n) {
                printf("REC: replay finished at frame %lu, live input from here\n",
                       (unsigned long)rec_frame);
                gba_rec_close();
                return live;
            }
        }
        return rec_buf[rec_pos++];
    }
    return live;
}

static uint32_t cpu_hash(void)
{
    uint32_t h = FNV0;
    h = fnv(reg, 17 * sizeof(uint32_t), h);     /* r0-r15, CPSR */
    h = fnv(spsr, sizeof(spsr), h);
    h = fnv(reg_mode, sizeof(reg_mode), h);
    return h;
}

void gba_rec_audio(const int16_t *audio, int pairs)
{
    if (rec_mode != GBA_REC_OFF && audio && pairs > 0)
        rec_audio_h = fnv(audio, (uint32_t)pairs * 2u * sizeof(int16_t), rec_audio_h);
}

void gba_rec_frame_done(void)
{
    if (rec_mode == GBA_REC_OFF)
        return;
    rec_frame++;
#ifdef HOST_BUILD
    static unsigned int every;
    if (!every) {   /* GBA_HASH_EVERY=N (host): a REC/hash line every N frames */
        const char *e = getenv("GBA_HASH_EVERY");
        every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 120u;
    }
    if (rec_frame % every)
        return;
#else
    if (rec_frame % 120u)
        return;
#endif
    unsigned n;
    const gba_bulk_region_t *rg = gba_bulk_regions(&n);
    uint32_t h[6] = { 0 };
    for (unsigned i = 0; i < n && i < 6; i++)
        h[i] = fnv(rg[i].ptr, rg[i].len, FNV0);
#ifdef HOST_BUILD
    {   /* interpreted instructions per frame since the last line (cpu.cc) */
        extern uint32_t gba_icount;
        static uint32_t last_i, last_f;
        if (rec_frame <= last_f)          /* a new replay */
            last_f = 0;
        extern uint32_t gba_idle_skipped, gba_idle_skips, gba_idle_arms, gba_idle_rejects;
        static uint32_t last_sk, last_n;
        printf("REC/icount frame=%lu instr=%lu per frame, idle: skipped=%lu cycles per frame in %lu skips, arms=%lu rejects=%lu\n",
               (unsigned long)rec_frame,
               (unsigned long)((gba_icount - last_i) / (rec_frame - last_f)),
               (unsigned long)((gba_idle_skipped - last_sk) / (rec_frame - last_f)),
               (unsigned long)(gba_idle_skips - last_n),
               (unsigned long)gba_idle_arms, (unsigned long)gba_idle_rejects);
        last_i = gba_icount;
        last_f = rec_frame;
        last_sk = gba_idle_skipped;
        last_n = gba_idle_skips;
    }
#endif
    printf("REC/hash frame=%lu iwram=%08lx ewram=%08lx vram=%08lx oam=%08lx pal=%08lx io=%08lx cpu=%08lx audio=%08lx\n",
           (unsigned long)rec_frame, (unsigned long)h[0], (unsigned long)h[1],
           (unsigned long)h[2], (unsigned long)h[3], (unsigned long)h[4],
           (unsigned long)h[5], (unsigned long)cpu_hash(), (unsigned long)rec_audio_h);
}
