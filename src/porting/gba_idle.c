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
/* the armed loop's lowest address (a path loop can start below its head) */
static uint32_t lo __attribute__((section(".gba_tail_bss")));
static int last_thumb, head_thumb;
static uint32_t black[NBLACK], black_n, black_hits;
static uint32_t black_head[NHEADS], black_head_n;
/* the probe: the machine at the head after the first arrival */
static int probe_ok;

/* A game's own wait loop that the generic skip cannot take (it stores on
 * every pass) but that is still the same pass between two events, apart
 * from what it stores: a function for its head (game_head_fn, set by
 * gba_idle_reset for that game; the generic search is off then). In the
 * flash, as the rest of this file's cold code; its state at the end of
 * .bss (nothing moves). */
#define IDLE_GAME_TAIL __attribute__((section(".gba_tail_bss")))
typedef int32_t (*idle_game_fn_t)(int32_t cycles, uint32_t flags);
static idle_game_fn_t game_head_fn IDLE_GAME_TAIL;
static int idle_on IDLE_GAME_TAIL;   /* gba_idle_enable ran: the skip is on */
static uint32_t probe_seq, probe_fx, probe_reg[15], probe_mode, probe_flags, fails;
static int32_t probe_cycles;

static void disarm(void)
{
    head = 0xFFFFFFFFu;
    gba_idle_head_pc = head;
    idle_loop_target_pc = 0xFFFFFFFFu;
    probe_ok = 0;
}

static void idle_game_setup(void);

void gba_idle_reset(void)
{
    disarm();
    last_pc = 0xFFFFFFFFu;
    black_n = black_head_n = black_hits = 0;
    memset(black, 0xff, sizeof black);
    memset(black_head, 0xff, sizeof black_head);
    seq++;
    idle_game_setup();
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
    idle_on = 1;
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
extern uint32_t game_hle_pc, game_hle_size;   /* gba_memory.c: a game HLE's guest range */

/* The decoder and the loop search: in RAM_EMU (ld/gba_core.ld .gba_ram_text),
 * not the flash -- they run whenever two events land near each other outside
 * an armed loop: with a line interrupt that is dozens of times a frame, each
 * from cold flash lines (1.6% of the device's time, Super Puzzle Fighter II's
 * full board). */
#ifndef HOST_BUILD
#define IDLE_RAM __attribute__((section(".gba_ram_text.idle")))
#else
#define IDLE_RAM
#endif

#ifndef HOST_BUILD
#define GBA_IDLE_HOT __attribute__((section(".gba_itcm_text")))
/* its own section: ld/gba_core.ld keeps it at one place mod 32 */
#define GBA_IDLE_HEAD_HOT __attribute__((section(".gba_itcm_text.idle_head")))
#else
#define GBA_IDLE_HOT
#define GBA_IDLE_HEAD_HOT
#endif

GBA_IDLE_HOT static int blacklisted(uint32_t pc)
{
    pc >>= 6;
    for (uint32_t i = 0; i < NBLACK; i++)
        if (black[i] == pc)
            return 1;
    return 0;
}

IDLE_RAM static void blacklist(uint32_t pc)
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

IDLE_RAM static const uint8_t *code_at(uint32_t a)
{
    const uint8_t *p = memory_map_read[a >> 15];
    return p ? p + (a & 0x7FFFu) : NULL;
}

/* 1 if the Thumb instruction may sit in a poll loop; *target = a branch's
 * target (or 0xFFFFFFFF) */
IDLE_RAM static int thumb_ok(uint32_t a, uint16_t op, uint32_t *target)
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

IDLE_RAM static int arm_ok(uint32_t a, uint32_t op, uint32_t *target)
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
IDLE_RAM static int find_loop(uint32_t pc, int thumb, uint32_t *h, uint32_t *t)
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

/* A poll loop that jumps over code or data (its literal pool between a
 * branch and its target: Super Mario Advance's VBlank wait,
 * b 0x8001cde ... beq 0x8001cfc; b 0x8001cbc, 56% of its instructions) is
 * not a straight run back to a backward branch: follow the code as it runs,
 * from the PC along each branch (both ways at a conditional one), back to
 * the PC within MAX_BODY instructions, every instruction on that path
 * allowed. The probe (gba_idle_head) still proves each skip. *h = the first
 * branch target on the path (reached by a jump once a pass: the probe's
 * PC), *l..*t the path's extent. */
#define IDLE_TAIL_BSS __attribute__((section(".gba_tail_bss")))
static uint32_t path_pc[MAX_BODY] IDLE_TAIL_BSS, path_tgt[MAX_BODY] IDLE_TAIL_BSS, path_steps IDLE_TAIL_BSS;
static uint8_t path_taken[MAX_BODY] IDLE_TAIL_BSS;

IDLE_RAM static int walk(uint32_t a, uint32_t pc, int thumb, uint32_t depth)
{
    if (depth && a == pc)
        return (int)(path_steps = depth), 1;
    if (depth >= MAX_BODY)
        return 0;
    const uint8_t *p = code_at(a);
    if (!p)
        return 0;
    uint32_t op = thumb ? (uint32_t)(p[0] | p[1] << 8)
                        : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    uint32_t tgt, step = thumb ? 2u : 4u;
    if (!(thumb ? thumb_ok(a, (uint16_t)op, &tgt) : arm_ok(a, op, &tgt)))
        return 0;
    path_pc[depth] = a;
    path_tgt[depth] = tgt;
    path_taken[depth] = 0;
    if (tgt == 0xFFFFFFFFu)
        return walk(a + step, pc, thumb, depth + 1);
    int always = thumb ? (op >= 0xE000 && op < 0xE800) : (op >> 28) == 0xE;
    path_taken[depth] = 1;
    if (walk(tgt, pc, thumb, depth + 1))
        return 1;
    if (always)
        return 0;
    path_taken[depth] = 0;
    return walk(a + step, pc, thumb, depth + 1);
}

/* noinline: it runs only when a loop is armed; inlined, it grew the ITCM
 * hook (gba_idle_event) and moved the code after it */
IDLE_RAM __attribute__((noinline)) static int find_poll_loop(uint32_t pc, int thumb, uint32_t *h, uint32_t *l, uint32_t *t)
{
    if (find_loop(pc, thumb, h, t)) {
        *l = *h;
        return 1;
    }
    if (!walk(pc, pc, thumb, 0))
        return 0;
    uint32_t i, first = MAX_BODY;
    *l = 0xFFFFFFFFu;
    *t = 0;
    for (i = 0; i < path_steps; i++) {
        if (path_pc[i] < *l) *l = path_pc[i];
        if (path_pc[i] > *t) *t = path_pc[i];
        if (path_taken[i] && first == MAX_BODY)
            first = i;
    }
    if (first == MAX_BODY)
        return 0;                          /* no jump: find_loop's shape */
    *h = path_tgt[first];
    if ((cheat_master_hook >= *l && cheat_master_hook <= *t) ||
        (m4a_hook_pc && m4a_hook_pc >= *l && m4a_hook_pc <= *t))
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
    if (head != 0xFFFFFFFFu && thumb == head_thumb && pc >= lo && pc <= tail)
        return;                            /* still in the armed loop */
    if (pc == 0xFFFFFFFFu || thumb != prev_thumb || pc - prev + NEAR > 2u * NEAR)
        return;                            /* not two events in one short loop */
    if (pc - game_hle_pc < game_hle_size)
        return;                            /* a game HLE runs it natively (src/hle):
                                            * a slice ends there when the routine
                                            * stops mid-loop -- no poll loop to
                                            * find (Super Puzzle Fighter II's
                                            * mixer: ~39 decodes a frame) */
    if (blacklisted(pc) && (++black_hits % RETRY))
        return;
    uint32_t l;
    if (!find_poll_loop(pc, thumb, &h, &l, &t) || head_rejected(h)) {
        blacklist(pc);
        return;
    }
    head = h;
    lo = l;
    tail = t;
    head_thumb = thumb;
    gba_idle_head_pc = h;
    idle_loop_target_pc = h;
    probe_ok = 0;
    fails = 0;
    gba_idle_arms++;
    if (log_on)
        printf("IDLE: armed %s loop %08lx-%08lx head %08lx\n", thumb ? "Thumb" : "ARM",
               (unsigned long)l, (unsigned long)t, (unsigned long)h);
}

GBA_IDLE_HEAD_HOT int32_t gba_idle_head(int32_t cycles, uint32_t n, uint32_t z, uint32_t c, uint32_t v)
{
    uint32_t flags = n | z << 1 | c << 2 | v << 3, mode = reg[16] & 0xFFu;
    if (__builtin_expect(game_head_fn != 0, 0))
        return game_head_fn(cycles, flags);
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

/* ---- WarioWare: Twisted! (USA): its VBlank wait ----------------------------
 * 0x080012F8: until the VBlank handler sets the halfword at 0x03000008, each
 * pass steps the game's random number (0x08001424: x = x * 109 + 0x3FD, 16
 * bits at 0x03000010), reads the ROM word at 0x08000000 + 4 * x and stores
 * it on the stack. ~60% of its instructions on the title (host PC
 * histogram, 2026-11-17); the generic skip rejects it (stores). Between two
 * events every pass is the same pass but for those two stores, so k passes
 * are: the number stepped k times, the last pass's ROM word on the stack,
 * k times the pass's cycles (measured between two arrivals, as the generic
 * probe does). Verified by the replay's REC/hash lines. */
#define RZW_HEAD 0x0800130Cu
#define RZW_RNG  0x03000010u
#define RZW_FNV  0x7c26f928u   /* 0x080012F8-0x08001338 + 0x08001424-0x08001440 */

extern unsigned int read_memory32(unsigned int address);
extern unsigned short read_memory16(unsigned int address);

extern uint16_t io_registers[];
#define RZW_FLAG 0x03000008u   /* the VBlank handler's flag the loop waits on */
#define RZW_WAITCNT 0x102      /* REG_WAITCNT, io_registers' index */

static int32_t rzw_cycles IDLE_GAME_TAIL;
static uint32_t rzw_seq IDLE_GAME_TAIL, rzw_fx IDLE_GAME_TAIL, rzw_flags IDLE_GAME_TAIL,
                rzw_mode IDLE_GAME_TAIL, rzw_x IDLE_GAME_TAIL, rzw_reg[15] IDLE_GAME_TAIL;
static int rzw_ok IDLE_GAME_TAIL;
/* a pass's cycles once proven (the probe below), with the wait states and
 * mode it was measured at: from then on any arrival skips at once */
static int32_t rzw_pass IDLE_GAME_TAIL;
static uint32_t rzw_pass_waitcnt IDLE_GAME_TAIL, rzw_pass_mode IDLE_GAME_TAIL;

static uint32_t rzw_lcg(uint32_t x) { return (x * 109u + 0x3FDu) & 0xFFFFu; }

static uint32_t rzw_code_hash(void)
{
    uint32_t h = 2166136261u;
    for (uint32_t a = 0x080012F8u; a < 0x08001338u; a += 2)
        h = (h ^ read_memory16(a)) * 16777619u;
    for (uint32_t a = 0x08001424u; a < 0x08001440u; a += 2)
        h = (h ^ read_memory16(a)) * 16777619u;
    return h;
}

/* k whole passes from the head: what they leave (a pass overwrites r0-r2,
 * lr and the flags with the same values every time: the arrival's own flags
 * already are those -- the loop's beq follows its cmp) */
static int32_t rzw_skip(uint8_t *iw, uint32_t x, uint32_t sp, int32_t cycles, int32_t pass)
{
    int32_t k = (cycles - 1) / pass;
    if (k <= 0)
        return cycles;
    for (int32_t i = 0; i < k; i++)
        x = rzw_lcg(x);
    *(uint16_t *)(iw + (RZW_RNG & 0x7FFF)) = (uint16_t)x;
    *(uint32_t *)(iw + (sp & 0x7FFC)) = read_memory32(0x08000000u + (x << 2));
    reg[0] = 0;                 /* the flag (ldrh) */
    reg[1] = 0x08000000u;       /* movs r1, #128; lsls r1, #20 */
    reg[2] = 0x3FDu;            /* the generator's literal */
    reg[14] = 0x08001311u;      /* bl's return */
    gba_idle_sidefx += 2u * (uint32_t)k;
    gba_idle_skipped += (uint32_t)(k * pass);
    gba_idle_skips++;
    return cycles - (k * pass - (bug ? 1 : 0));
}

static int32_t rzw_wait(int32_t cycles, uint32_t flags)
{
    uint8_t *iw = memory_map_read[0x03000000u >> 15];
    uint32_t x = *(uint16_t *)(iw + (RZW_RNG & 0x7FFF)), sp = reg[13];
    uint32_t mode = reg[16] & 0xFFu;
    /* proven: skip at the first arrival after an event, if the next pass is
     * a pass (the flag still 0: an event between its check and the branch
     * back can have set it) */
    if (rzw_pass > 0 && rzw_pass_waitcnt == io_registers[RZW_WAITCNT] && rzw_pass_mode == mode &&
        (sp >> 24) == 3 && *(uint16_t *)(iw + (RZW_FLAG & 0x7FFF)) == 0) {
        rzw_ok = 0;
        return rzw_skip(iw, x, sp, cycles, rzw_pass);
    }
    if (rzw_ok && rzw_seq == seq && cycles < rzw_cycles && flags == rzw_flags &&
        mode == rzw_mode && gba_idle_sidefx == rzw_fx + 2 && x == rzw_lcg(rzw_x) &&
        (sp >> 24) == 3 && !memcmp(reg, rzw_reg, sizeof rzw_reg) &&
        *(uint32_t *)(iw + (sp & 0x7FFC)) == read_memory32(0x08000000u + (x << 2))) {
        int32_t pass = rzw_cycles - cycles;
        rzw_pass = pass;                       /* proven: the next arrivals skip at once */
        rzw_pass_waitcnt = io_registers[RZW_WAITCNT];
        rzw_pass_mode = mode;
        rzw_ok = 0;
        return rzw_skip(iw, x, sp, cycles, pass);
    }
    rzw_ok = 1;
    rzw_seq = seq;
    rzw_cycles = cycles;
    rzw_flags = flags;
    rzw_mode = mode;
    rzw_fx = gba_idle_sidefx;
    rzw_x = x;
    memcpy(rzw_reg, reg, sizeof rzw_reg);
    return cycles;
}

/* at every reset: a game with its own wait loop gets its head armed for good */
static void idle_game_setup(void)
{
    game_head_fn = 0;
    rzw_ok = 0;
    rzw_pass = 0;
    if (!idle_on)
        return;
    enabled = 1;
    if (read_memory32(0x080000ACu) == 0x45575A52u) {   /* "RZWE" */
        uint32_t h = rzw_code_hash();
#ifdef HOST_BUILD
        if (getenv("GBA_IDLE_LOG"))
            printf("IDLE: WarioWare wait loop code hash %08lx\n", (unsigned long)h);
#endif
        if (h == RZW_FNV) {
            enabled = 0;                  /* no generic arming over it */
            game_head_fn = rzw_wait;
            head = RZW_HEAD;
            head_thumb = 1;
            gba_idle_head_pc = RZW_HEAD;
            idle_loop_target_pc = RZW_HEAD;
        }
    }
}
