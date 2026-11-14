/*
 * gba_pchist.c -- host PC histogram of the ARM7 (GBA_PCHIST, host builds),
 * for the idle-skip pass and the interpreter work
 * (gw-arcade-core/docs/ARCADE_CPU_IDLE_SKIPS.md, "THE PROCEDURE").
 *
 * Every interpreted instruction counts at its own address (cpu.cc, after
 * the fetch). Every GBA_PCHIST_EVERY frames (default 600) it prints, for
 * that window only (then zeroes):
 *   PCHIST frames=A-B instr=N per frame, region split: bios ewram iwram rom
 *   PCHIST pc=XXXXXXXX A|T n=COUNT pct=P op=OPCODE     (the top GBA_PCHIST_TOP)
 * The region split says where the code runs from: on the device ROM code
 * is fetched from the QSPI flash, IWRAM/EWRAM code from internal RAM.
 * tools/gba_pchist.py disassembles the top PCs.
 *
 * GBA_PCHIST_FROM=N: start counting at loop frame N (a hot scene).
 */
#ifdef GBA_PCHIST
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "gba_savestate_abi.h"   /* gba_bulk_regions: [0] is the GBA's IWRAM */

extern uint8_t *gamepak_xip_base;
extern uint8_t ewram[];

#define ROM_SLOTS   (32u * 1024u * 1024u / 2u)
#define IW_SLOTS    (32u * 1024u / 2u)
#define EW_SLOTS    (256u * 1024u / 2u)
#define BIOS_SLOTS  (16u * 1024u / 2u)

static uint32_t *h_rom, *h_iw, *h_ew, *h_bios;
static uint8_t *m_rom, *m_iw, *m_ew, *m_bios;   /* 1 = Thumb */
static uint64_t n_region[4], n_total, n_other;
static uint32_t frame, frame0, every, top, from;
static int inited;
int gba_pchist_on = -1;      /* GBA_PCHIST=1 at run time (cpu.cc tests it) */

static void init(void)
{
    const char *e;
    inited = 1;
    h_rom = calloc(ROM_SLOTS, 4);  m_rom = calloc(ROM_SLOTS, 1);
    h_iw = calloc(IW_SLOTS, 4);    m_iw = calloc(IW_SLOTS, 1);
    h_ew = calloc(EW_SLOTS, 4);    m_ew = calloc(EW_SLOTS, 1);
    h_bios = calloc(BIOS_SLOTS, 4); m_bios = calloc(BIOS_SLOTS, 1);
    every = (e = getenv("GBA_PCHIST_EVERY")) ? (uint32_t)atoi(e) : 600u;
    top = (e = getenv("GBA_PCHIST_TOP")) ? (uint32_t)atoi(e) : 24u;
    from = (e = getenv("GBA_PCHIST_FROM")) ? (uint32_t)atoi(e) : 0u;
    if (!every)
        every = 600u;
    frame0 = from;
}

void gba_pchist_hit(uint32_t pc, int thumb)
{
    uint32_t i;
    if (!inited)
        init();
    if (frame < from)
        return;
    n_total++;
    switch (pc >> 24) {
    case 0x00:
        i = (pc & 0x3fffu) >> 1; h_bios[i]++; m_bios[i] = (uint8_t)thumb; n_region[0]++;
        break;
    case 0x02:
        i = (pc & 0x3ffffu) >> 1; h_ew[i]++; m_ew[i] = (uint8_t)thumb; n_region[1]++;
        break;
    case 0x03:
        i = (pc & 0x7fffu) >> 1; h_iw[i]++; m_iw[i] = (uint8_t)thumb; n_region[2]++;
        break;
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        i = (pc & 0x1ffffffu) >> 1; h_rom[i]++; m_rom[i] = (uint8_t)thumb; n_region[3]++;
        break;
    default:
        n_other++;
    }
}

typedef struct { uint32_t pc, n; uint8_t thumb; } ent_t;

static void consider(ent_t *best, uint32_t pc, uint32_t n, uint8_t thumb)
{
    if (n <= best[top - 1].n)
        return;
    uint32_t k = top - 1;
    while (k > 0 && best[k - 1].n < n) {
        best[k] = best[k - 1];
        k--;
    }
    best[k].pc = pc; best[k].n = n; best[k].thumb = thumb;
}

static uint32_t opcode_at(uint32_t pc, int thumb)
{
    const uint8_t *p = NULL;
    switch (pc >> 24) {
    case 0x02: p = ewram + (pc & 0x3ffffu); break;
    case 0x03: {
        unsigned n;
        p = (const uint8_t *)gba_bulk_regions(&n)[0].ptr + (pc & 0x7fffu);
        break;
    }
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        p = gamepak_xip_base ? gamepak_xip_base + (pc & 0x1ffffffu) : NULL; break;
    default: return 0;
    }
    if (!p)
        return 0;
    return thumb ? (uint32_t)(p[0] | p[1] << 8)
                 : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
}

static void dump(void)
{
    uint32_t frames = frame - frame0;
    ent_t *best = calloc(top, sizeof(ent_t));
    for (uint32_t i = 0; i < ROM_SLOTS; i++)
        if (h_rom[i]) consider(best, 0x08000000u + i * 2u, h_rom[i], m_rom[i]);
    for (uint32_t i = 0; i < IW_SLOTS; i++)
        if (h_iw[i]) consider(best, 0x03000000u + i * 2u, h_iw[i], m_iw[i]);
    for (uint32_t i = 0; i < EW_SLOTS; i++)
        if (h_ew[i]) consider(best, 0x02000000u + i * 2u, h_ew[i], m_ew[i]);
    for (uint32_t i = 0; i < BIOS_SLOTS; i++)
        if (h_bios[i]) consider(best, i * 2u, h_bios[i], m_bios[i]);
    double t = n_total ? (double)n_total : 1.0;
    printf("PCHIST frames=%u-%u instr=%llu per frame, region split: bios=%.1f%% ewram=%.1f%% iwram=%.1f%% rom=%.1f%% other=%.1f%%\n",
           frame0, frame - 1, (unsigned long long)(frames ? n_total / frames : 0),
           100.0 * n_region[0] / t, 100.0 * n_region[1] / t, 100.0 * n_region[2] / t,
           100.0 * n_region[3] / t, 100.0 * n_other / t);
    for (uint32_t k = 0; k < top && best[k].n; k++)
        printf("PCHIST pc=%08x %c n=%u pct=%.2f op=%0*x\n", best[k].pc, best[k].thumb ? 'T' : 'A',
               best[k].n, 100.0 * best[k].n / t, best[k].thumb ? 4 : 8,
               opcode_at(best[k].pc, best[k].thumb));
    free(best);
    memset(h_rom, 0, ROM_SLOTS * 4); memset(h_iw, 0, IW_SLOTS * 4);
    memset(h_ew, 0, EW_SLOTS * 4);   memset(h_bios, 0, BIOS_SLOTS * 4);
    memset(n_region, 0, sizeof n_region);
    n_total = n_other = 0;
    frame0 = frame;
}

/* once per emulated frame (main_gba.c) */
void gba_pchist_frame(void)
{
    if (gba_pchist_on < 0)
        gba_pchist_on = getenv("GBA_PCHIST") != NULL;
    if (!gba_pchist_on)
        return;
    if (!inited)
        init();
    frame++;
    if (frame > from && (frame - from) % every == 0)
        dump();
}
#endif
