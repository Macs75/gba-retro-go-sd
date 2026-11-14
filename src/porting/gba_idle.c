/*
 * gba_idle.c -- generic, exact skip of the ARM7's poll loops, for every
 * game (main_gba.c; it replaces the per-game tables: gpSP's gba_over.h,
 * gba_idle_loop.c, main_gba.c's vcount_polls, kept behind GBA_IDLE_TABLES).
 *
 * GBA games that finish a frame's work early often wait in a loop that
 * only reads and compares: DISPSTAT's vblank bit (`ldrh r1,[r2]; ands;
 * cmp; bne`), a RAM flag an interrupt handler sets (`ldr r0,[r1]; cmp;
 * beq`), VCOUNT against a line. In gpSP every value such a loop can read
 * changes only at an EVENT (update_gba: a scan line, a timer, a DMA, an
 * interrupt) or by the CPU's own stores -- and the loop makes none. So
 * between two events every pass of it is the same pass: same registers,
 * same flags, same cycles.
 *
 *  1. ARM (gba_idle_event, at every event): a poll loop spans many events,
 *     so the PC at two consecutive events lies in it. When the PC is within
 *     64 bytes of the previous event's, decode forward from it to the
 *     backward branch that closes the loop (at most 16 instructions back
 *     to its head) and check the body: loads, register arithmetic,
 *     compares, branches -- no store, call, SWI, stack or PC write. The
 *     head becomes idle_loop_target_pc (IDLE_COND_GENERIC). A rejected
 *     loop is remembered by its head; other code by 64-byte block, decoded
 *     again only at every 16th event pair landing in the block.
 *  2. PROVE (gba_idle_head, each time the PC comes to the head): the first
 *     arrival after an event takes a snapshot of r0-r14, the flags, the
 *     mode, the cycles left, the event and side-effect counters. The next
 *     arrival with no event in between compares: identical means one whole
 *     pass changed nothing, so every pass until the next event is that
 *     same pass, costing what this one cost (the cycles measured, not
 *     derived).
 *  3. SKIP: with R cycles left and a pass of C, the k = (R-1)/C passes that
 *     would all have completed before the event are taken at once
 *     (cycles only). The interpreter runs the last one and crosses into
 *     the event at the same instruction, with the same overshoot, as it
 *     would have on its own: the machine is exactly the same. Verified by
 *     replays' REC/hash lines, skip on and off.
 * A loop whose passes do differ (it counts, it reads a value that moves)
 * fails step 2 every time and is rejected after 8 tries.
 *
 * Host: GBA_NOIDLE=1 leaves it off (no skip at all), GBA_IDLEBUG=1 charges each skip one
 * cycle short (the control: it must change the hashes), GBA_IDLE_LOG=1
 * logs arms and rejects.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "gba_idle.h"

#define IDLE_COND_GENERIC 2u           /* cpu.h */
#define MAX_BODY   16u                 /* instructions */
#define NEAR       64u                 /* bytes between two events' PCs */
#define MAX_FAILS  8u
#define NBLACK     32u                 /* rejected spots, by 64-byte block */
#define NHEADS     32u                 /* rejected loops, by head */
#define RETRY      16u                 /* a rejected block: decode every 16th pair */

extern uint32_t idle_loop_target_pc, idle_loop_cond;
extern uint32_t reg[64];
extern uint8_t *memory_map_read[8 * 1024];
extern uint32_t cheat_master_hook;
extern unsigned int m4a_hook_pc;

uint32_t gba_idle_sidefx;
uint32_t gba_idle_skipped, gba_idle_skips, gba_idle_arms, gba_idle_rejects;
uint32_t gba_idle_head_pc = 0xFFFFFFFFu;
#ifdef HOST_BUILD
uint32_t gba_idle_heads;      /* arrivals at the armed head (REC/icount) */
#endif

static int enabled, bug, log_on;
static uint32_t seq, last_pc = 0xFFFFFFFFu, head = 0xFFFFFFFFu, tail;
static int last_thumb, head_thumb;
static uint32_t black[NBLACK], black_n, black_hits;
static uint32_t black_head[NHEADS], black_head_n;
/* the probe: the machine at the head after the first arrival */
static int probe_ok;
static uint32_t probe_seq, probe_fx, probe_reg[15], probe_mode, probe_flags, fails;
static int32_t probe_cycles;

static void disarm(void)
{
    head = 0xFFFFFFFFu;
    gba_idle_head_pc = head;
    idle_loop_target_pc = 0xFFFFFFFFu;
    probe_ok = 0;
}

void gba_idle_reset(void)
{
    disarm();
    last_pc = 0xFFFFFFFFu;
    black_n = black_head_n = black_hits = 0;
    memset(black, 0xff, sizeof black);
    memset(black_head, 0xff, sizeof black_head);
    seq++;
}

void gba_idle_enable(void)
{
#ifdef HOST_BUILD
    if (getenv("GBA_NOIDLE")) {      /* no skip at all, a table's neither */
        idle_loop_target_pc = 0xFFFFFFFFu;
        idle_loop_cond = 0;
        printf("gba: generic idle skip off (GBA_NOIDLE)\n");
        return;
    }
    bug = getenv("GBA_IDLEBUG") != NULL;
    log_on = getenv("GBA_IDLE_LOG") != NULL;
#endif
    enabled = 1;
    idle_loop_cond = IDLE_COND_GENERIC;
    gba_idle_reset();
    printf("gba: generic poll-loop skip on\n");
}

/* Code that is not a poll loop is remembered by 64-byte block, not by PC:
 * two events landing near each other in it (a long loop with stores, a busy
 * routine) land at many PCs, and each one used to be decoded again
 * (thumb_ok was 0.6% of the device's time, Kirby). But a block can also
 * hold a real poll loop next to the rejected spot, so every RETRY-th event
 * pair landing in a rejected block is decoded anyway: blocking the block
 * outright cost 18 of the 150 games most of their skip (Broken Sword 12K
 * interpreted instructions a frame -> 73K). A loop whose passes differ is
 * remembered by its exact head instead. */
#ifndef HOST_BUILD
#define GBA_IDLE_HOT __attribute__((section(".gba_itcm_text")))
#else
#define GBA_IDLE_HOT
#endif

GBA_IDLE_HOT static int blacklisted(uint32_t pc)
{
    pc >>= 6;
    for (uint32_t i = 0; i < NBLACK; i++)
        if (black[i] == pc)
            return 1;
    return 0;
}

static void blacklist(uint32_t pc)
{
    if (!blacklisted(pc))
        black[black_n++ % NBLACK] = pc >> 6;
}

GBA_IDLE_HOT static int head_rejected(uint32_t h)
{
    for (uint32_t i = 0; i < NHEADS; i++)
        if (black_head[i] == h)
            return 1;
    return 0;
}

static const uint8_t *code_at(uint32_t a)
{
    const uint8_t *p = memory_map_read[a >> 15];
    return p ? p + (a & 0x7FFFu) : NULL;
}

/* 1 if the Thumb instruction may sit in a poll loop; *target = a branch's
 * target (or 0xFFFFFFFF) */
static int thumb_ok(uint32_t a, uint16_t op, uint32_t *target)
{
    *target = 0xFFFFFFFFu;
    if (op < 0x4400)                       /* shifts, add/sub, mov/cmp/add/sub #, ALU */
        return 1;
    if (op < 0x4700)                       /* hi-reg add/cmp/mov: not to PC */
        return ((op >> 8) & 3) == 1 || (((op >> 4) & 8) | (op & 7)) != 15;
    if (op < 0x4800)                       /* BX */
        return 0;
    if (op < 0x5000)                       /* LDR pc-relative */
        return 1;
    if (op < 0x6000)                       /* reg-offset loads only */
        return ((op >> 9) & 7) >= 3;
    if (op < 0xA000)                       /* imm-offset / halfword / SP-relative: loads */
        return (op >> 11) & 1;
    if (op < 0xB000)                       /* ADD rd, PC/SP */
        return 1;
    if (op >= 0xD000 && op < 0xDE00) {     /* Bcc */
        *target = a + 4u + (uint32_t)((int32_t)(int8_t)(op & 0xFF) * 2);
        return 1;
    }
    if (op >= 0xE000 && op < 0xE800) {     /* B */
        int32_t off = (int32_t)(op & 0x7FF);
        if (off & 0x400)
            off -= 0x800;
        *target = a + 4u + (uint32_t)(off * 2);
        return 1;
    }
    return 0;                              /* stack, LDM/STM, SWI, BL, ADD SP */
}

static int arm_ok(uint32_t a, uint32_t op, uint32_t *target)
{
    *target = 0xFFFFFFFFu;
    if ((op >> 28) == 0xF)
        return 0;
    switch ((op >> 25) & 7) {
    case 0: case 1: {                      /* data processing, multiply, halfword */
        uint32_t rd = (op >> 12) & 15, opc = (op >> 21) & 15, s = (op >> 20) & 1;
        if ((op & 0x0E000090) == 0x00000090) {   /* multiply / swap / halfword */
            if ((op & 0x0F0000F0) == 0x00000090) /* MUL/MLA: rd in 19:16 */
                return ((op >> 16) & 15) != 15;
            if ((op & 0x0FB00FF0) == 0x01000090) /* SWP */
                return 0;
            return ((op >> 20) & 1) && rd != 15; /* LDRH/LDRSB/LDRSH */
        }
        if (opc >= 8 && opc <= 11 && !s)         /* MRS/MSR/BX */
            return 0;
        if (opc >= 8 && opc <= 11)               /* TST/TEQ/CMP/CMN */
            return 1;
        return rd != 15;
    }
    case 2: case 3:                        /* LDR/STR */
        if ((op >> 25) & 1 && (op >> 4) & 1)
            return 0;                      /* undefined */
        return ((op >> 20) & 1) && ((op >> 12) & 15) != 15;
    case 5:                                /* B / BL */
        if ((op >> 24) & 1)
            return 0;
        *target = a + 8u + (uint32_t)(((int32_t)(op << 8) >> 8) * 4);
        return 1;
    default:                               /* LDM/STM, coprocessor, SWI */
        return 0;
    }
}

/* the poll loop the PC is in, from the PC forward to the backward branch
 * closing it: 1 and its head / last instruction, or 0 */
static int find_loop(uint32_t pc, int thumb, uint32_t *h, uint32_t *t)
{
    uint32_t step = thumb ? 2u : 4u, a = pc, tgt;
    for (uint32_t i = 0; i < MAX_BODY; i++, a += step) {
        const uint8_t *p = code_at(a);
        if (!p)
            return 0;
        uint32_t op = thumb ? (uint32_t)(p[0] | p[1] << 8)
                            : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
        if (!(thumb ? thumb_ok(a, (uint16_t)op, &tgt) : arm_ok(a, op, &tgt)))
            return 0;
        if (tgt != 0xFFFFFFFFu && tgt <= pc && a - tgt < MAX_BODY * step) {
            *h = tgt;
            *t = a;
            break;
        }
        if (i == MAX_BODY - 1)
            return 0;
    }
    /* the whole body, head to branch: every instruction allowed, every
     * branch staying in the loop or leaving it (an exit) */
    for (a = *h; a < pc; a += step) {
        const uint8_t *p = code_at(a);
        if (!p)
            return 0;
        uint32_t op = thumb ? (uint32_t)(p[0] | p[1] << 8)
                            : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
        if (!(thumb ? thumb_ok(a, (uint16_t)op, &tgt) : arm_ok(a, op, &tgt)))
            return 0;
        if (tgt != 0xFFFFFFFFu && tgt < *h)
            return 0;                      /* an outer loop's branch: not this shape */
    }
    if ((cheat_master_hook >= *h && cheat_master_hook <= *t) ||
        (m4a_hook_pc && m4a_hook_pc >= *h && m4a_hook_pc <= *t))
        return 0;                          /* hooks the interpreter must reach */
    return 1;
}

/* gba_idle_event and gba_idle_head run at every event and at every pass's
 * head: in ITCM (they ran from the flash); the decoder stays in flash. */
GBA_IDLE_HOT void gba_idle_event(uint32_t pc, int thumb)
{
    uint32_t h, t, prev = last_pc;
    int prev_thumb = last_thumb;

    seq++;
    if (!enabled)
        return;
    last_pc = pc;
    last_thumb = thumb;
    if (head != 0xFFFFFFFFu && thumb == head_thumb && pc >= head && pc <= tail)
        return;                            /* still in the armed loop */
    if (pc == 0xFFFFFFFFu || thumb != prev_thumb || pc - prev + NEAR > 2u * NEAR)
        return;                            /* not two events in one short loop */
    if (blacklisted(pc) && (++black_hits % RETRY))
        return;
    if (!find_loop(pc, thumb, &h, &t) || head_rejected(h)) {
        blacklist(pc);
        return;
    }
    head = h;
    tail = t;
    head_thumb = thumb;
    gba_idle_head_pc = h;
    idle_loop_target_pc = h;
    probe_ok = 0;
    fails = 0;
    gba_idle_arms++;
    if (log_on)
        printf("IDLE: armed %s loop %08lx-%08lx\n", thumb ? "Thumb" : "ARM",
               (unsigned long)h, (unsigned long)t);
}

GBA_IDLE_HOT int32_t gba_idle_head(int32_t cycles, uint32_t n, uint32_t z, uint32_t c, uint32_t v)
{
    uint32_t flags = n | z << 1 | c << 2 | v << 3, mode = reg[16] & 0xFFu;
#ifdef HOST_BUILD
    gba_idle_heads++;
#endif

    if (probe_ok && probe_seq == seq) {
        if (probe_fx == gba_idle_sidefx && flags == probe_flags && mode == probe_mode &&
            cycles < probe_cycles && !memcmp(reg, probe_reg, sizeof probe_reg)) {
            int32_t pass = probe_cycles - cycles;
            int32_t k = (cycles - 1) / pass;
            if (k > 0) {
                cycles -= k * pass - (bug ? 1 : 0);
                gba_idle_skipped += (uint32_t)(k * pass);
                gba_idle_skips++;
            }
            fails = 0;
            probe_ok = 0;
            return cycles;
        }
        if (++fails >= MAX_FAILS) {        /* its passes differ: not a poll loop */
            if (log_on)
                printf("IDLE: rejected loop %08lx (passes differ)\n", (unsigned long)head);
            gba_idle_rejects++;
            black_head[black_head_n++ % NHEADS] = head;
            blacklist(head);
            disarm();
            return cycles;
        }
    }
    probe_ok = 1;
    probe_seq = seq;
    probe_fx = gba_idle_sidefx;
    probe_flags = flags;
    probe_mode = mode;
    probe_cycles = cycles;
    memcpy(probe_reg, reg, sizeof probe_reg);
    return cycles;
}
