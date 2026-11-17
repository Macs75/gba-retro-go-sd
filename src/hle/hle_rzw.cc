/* hle_rzw.cc -- WarioWare: Twisted!'s sound mixing loops, run natively.
 *
 * The contract is gba_hle.h's (see hle_vrally3.cc): the interpreter's exact
 * memory accesses, cycle charges and stopping points (cycles <= 0 after an
 * instruction: stop with the PC on the next one), so the machine state is
 * the same instruction by instruction (REC/hash). The interpreter calls it
 * when an ARM jump lands in [0x03000D38, 0x03003820); it runs the
 * routines below and declines every other PC there.
 *
 * The game's own sound driver (not M4A) runs from IWRAM 0x03003000. In the
 * first level's intro its three inner loops were ~25-31% of the guest
 * instructions (host PC histogram, 2026-11-17):
 *   0x030030E0-0x030033AC  one channel resampled into the 32-bit L/R mix
 *                          buffer, four samples a pass: 18.14 position,
 *                          linear interpolation, two volumes (mla)
 *   0x030036FC-0x03003768  two sources summed, scaled, written as 4 words
 *   0x030037B0-0x0300381C  the mix buffer through a byte table into the two
 *                          8-bit output buffers, 8 samples a pass
 * And its 32-bit unsigned division, 0x03000D38-0x03000F40 (r0 / r1: the
 * quotient in r0, the remainder in r2), fully unrolled: 130 instructions,
 * ~205 calls a frame in the story select (30% of its instructions). No
 * memory accesses, so each instruction costs its fetch: from its entry with
 * the slice outlasting all 130 it runs as one loop; otherwise (the slice
 * ends inside it, or it resumes there after an interrupt) instruction by
 * instruction, stopping where the interpreter would.
 * Every loop exit, the careful path near a sample's end and the other mixer
 * variants are interpreted. A block transfer or store outside work RAM
 * stops it before that instruction. The routines' opcodes are hashed once
 * a frame and two of each checked on every entry.
 */
extern "C" {
#include "../gpsp/common.h"   /* gpSP's: another common.h is earlier in the path */
}
#include "gba_hle.h"
#include "../porting/gba_idle.h"

#define RZW_MIX_FNV 0x2843701bu   /* the four ranges in rzw_code_ok */
#define RZW_DIV_FNV 0x3203ce29u   /* 0x03000D38-0x03000F40 */

/* fast_read_memory(8): the caller sign- or zero-extends */
static inline u32 rzw_rd8(u32 a, s32 &cyc)
{
  u8 *map;
  if (a < 0x10000000)
    cyc -= ws_cyc_nseq[a >> 24][0];
  if ((a >> 24) == 0 || (a & 0xF0000000) || !(map = memory_map_read[a >> 15]))
    return (u8)read_memory8(a);
  return readaddress8(map, a & 0x7FFF);
}

/* the same, out of line for the colder loops (GCC: inlined at all 26 loads
 * it was ~500 bytes over the ITCM window); the cycles stay inline */
static __attribute__((noinline)) u32 rzw_ld8(u32 a)
{
  u8 *map;
  if ((a >> 24) == 0 || (a & 0xF0000000) || !(map = memory_map_read[a >> 15]))
    return (u8)read_memory8(a);
  return readaddress8(map, a & 0x7FFF);
}
static inline s32 rzw_c8(u32 a) { return a < 0x10000000 ? ws_cyc_nseq[a >> 24][0] : 0; }

static inline u32 rzw_wram32(u32 a) { return readaddress32(memory_map_read[a >> 15], a & 0x7FFF); }
static inline void rzw_wram_st32(u32 a, u32 v)
{
  *(u32 *)(memory_map_read[a >> 15] + (a & 0x7FFF)) = v;
  gba_idle_sidefx++;
}
/* n words from a (word-aligned) all in work RAM: what the interpreter's
 * block transfer does there without a call */
static inline bool rzw_wram_span(u32 a, u32 n)
{
  return ((a >> 24) - 2) <= 1 && (((a + 4 * n - 4) >> 24) - 2) <= 1;
}

#ifndef HOST_BUILD
static u32 rzw_checked_frame __attribute__((section(".gba_tail_bss")));
static u32 rzw_div_checked_frame __attribute__((section(".gba_tail_bss")));
#else
static u32 rzw_checked_frame, rzw_div_checked_frame;
#endif

/* once a frame: in the flash, not in the ITCM window */
#ifndef HOST_BUILD
__attribute__((noinline, section(".gba_cold_text.rzw")))
#else
__attribute__((noinline))
#endif
static bool rzw_code_ok(bool div)
{
  static const u16 ranges[5][2] = {
    { 0x30E0, 0x30F0 }, { 0x32EC, 0x33AC }, { 0x36FC, 0x3768 }, { 0x37B0, 0x381C },
    { 0x0D38, 0x0F40 } };
  u8 *iw = memory_map_read[0x03000000u >> 15];
  u32 h = 2166136261u;
  for (u32 i = div ? 4 : 0; i < (div ? 5u : 4u); i++)
    for (u32 a = ranges[i][0]; a <= ranges[i][1]; a += 4)
      h = (h ^ readaddress32(iw, a)) * 16777619u;
  return h == (div ? RZW_DIV_FNV : RZW_MIX_FNV);
}

/* the division: i is its instruction, 0x03000D38 + 4 * i, 0 <= i < 130:
 *   0 mov r2, #0      1 adcs r0, r0, r0
 *   then 32 times: adc r2, r2, r2; cmp r2, r1; subcs r2, r2, r1;
 *   adcs r0, r0, r0 (the very last, 129, adc)
 * and 130 is its bx lr (interpreted). */
static uint64_t rzw_div(u32 i, s32 cyc, u32 nzcv)
{
  const s32 F = ws_cyc_seq[3][1];
  u8 *iw = memory_map_read[0x03000000u >> 15];
  if (!iw || readaddress32(iw, 0x0D3C) != 0xe0b00000u || readaddress32(iw, 0x0F3C) != 0xe0a00000u)
    return (u32)cyc;
  if (rzw_div_checked_frame != frame_counter + 1) {
    if (!rzw_code_ok(true))
      return (u32)cyc;
    rzw_div_checked_frame = frame_counter + 1;
  }
  u32 r0 = reg[0], r1 = reg[1], r2 = reg[2];
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  if (i == 0 && cyc > 129 * F) {
    /* whole: no instruction before the last ends the slice. The N, Z, V of
     * every adcs are the next cmp's; only the last cmp's are left. */
    r2 = 0;
    { u64 s = (u64)r0 + r0 + c; r0 = (u32)s; c = (u32)(s >> 32); }
    for (u32 k = 0; k < 32; k++) {
      r2 = r2 + r2 + c;
      u32 d = r2 - r1;
      n = d >> 31; z = d == 0; v = ((r2 ^ r1) & (r2 ^ d)) >> 31; c = r2 >= r1;
      if (c)
        r2 = d;
      u64 s = (u64)r0 + r0 + c;
      r0 = (u32)s;
      if (k < 31)
        c = (u32)(s >> 32);
    }
    cyc -= 130 * F;
    i = 130;
  } else {
    do {
      u32 op = i < 2 ? i : 2 + ((i - 2) & 3);
      if (op == 0) {
        r2 = 0;                                                     /* mov r2, #0 */
      } else if (op == 1 || op == 5) {
        u32 x = r0;                                                 /* adcs r0, r0, r0 */
        u64 s = (u64)x + x + c;
        r0 = (u32)s;
        if (i != 129) {                                             /* (129: adc) */
          n = r0 >> 31; z = r0 == 0; c = (u32)(s >> 32); v = (x ^ r0) >> 31;
        }
      } else if (op == 2) {
        r2 = r2 + r2 + c;                                           /* adc r2, r2, r2 */
      } else if (op == 3) {
        u32 d = r2 - r1;                                            /* cmp r2, r1 */
        n = d >> 31; z = d == 0; v = ((r2 ^ r1) & (r2 ^ d)) >> 31; c = r2 >= r1;
      } else if (c) {
        r2 = r2 - r1;                                               /* subcs r2, r2, r1 */
      }
      i++;
      cyc -= F;
    } while (i < 130 && cyc > 0);
  }
  reg[0] = r0; reg[2] = r2;
  reg[REG_PC] = 0x03000D38u + 4 * i;
  return (u32)cyc | ((u64)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}

extern "C" uint64_t hle_rzw_run(uint32_t pc, int32_t cycles, uint32_t nzcv)
{
  s32 cyc = cycles;   /* gpSP's s32 (int; int32_t is long on the device) */
  if (pc - 0x03000D38u < 130 * 4)
    return rzw_div((pc - 0x03000D38u) >> 2, cyc, nzcv);
  u8 *code = memory_map_read[0x03000000u >> 15];
  if (!code)
    return (u32)cyc;
#define RZW_OP(a) readaddress32(code, (a) & 0x7FFF)
  if (pc - 0x030030E0u <= 0x10u || pc - 0x030032ECu <= 0xC0u) {
    if (RZW_OP(0x030030ECu) != 0xe0584724u || RZW_OP(0x0300330Cu) != 0xe0244b93u)
      return (u32)cyc;
  } else if (pc - 0x030036FCu <= 0x6Cu) {
    if (RZW_OP(0x03003704u) != 0xe19060deu || RZW_OP(0x03003764u) != 0xe8a203c0u)
      return (u32)cyc;
  } else if (pc - 0x030037B0u <= 0x6Cu) {
    if (RZW_OP(0x030037B8u) != 0xe8b10ff0u || RZW_OP(0x030037DCu) != 0xe7de4004u)
      return (u32)cyc;
  } else
    return (u32)cyc;
#undef RZW_OP
  if (rzw_checked_frame != frame_counter + 1) {
    if (!rzw_code_ok(false))
      return (u32)cyc;
    rzw_checked_frame = frame_counter + 1;
  }

  u32 r0 = reg[0], r1 = reg[1], r2 = reg[2], r3 = reg[3], r4 = reg[4], r5 = reg[5];
  u32 r6 = reg[6], r7 = reg[7], r8 = reg[8], r9 = reg[9], r10 = reg[10];
  u32 r11 = reg[11], r12 = reg[12], r14 = reg[14];
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const s32 F = ws_cyc_seq[3][1];      /* each instruction's fetch (IWRAM) */
  const s32 BT = ws_cyc_nseq[3][1];    /* a taken branch (to IWRAM) */
  const s32 cyc0 = cyc;                /* every instruction charges: ran <=> cyc < cyc0 */
  /* the guest PC: the instruction about to run. Every instruction steps it,
   * so a stop is one branch, with the PC already right (GCC: a constant
   * PC per stop doubled the code, over the ITCM window) */
  u32 gpc = pc;

#define STOP      goto done
#define STEP      { gpc += 4; cyc -= F; if (__builtin_expect(cyc <= 0, 0)) STOP; }
/* a taken B: its target's fetch and the next instruction's */
#define B(tgt, lab) { gpc = (tgt); cyc -= BT + F; if (__builtin_expect(cyc <= 0, 0)) STOP; goto lab; }
#define B_OUT(tgt)  { gpc = (tgt); cyc -= BT + F; STOP; }      /* to an interpreted exit */
#define SUBF(d, x, y) { u32 _x = (x), _y = (y), _r = _x - _y; n = _r >> 31; z = _r == 0; \
                        c = _x >= _y; v = ((_x ^ _y) & (_x ^ _r)) >> 31; d; }
/* LDRSB / LDRB */
#define LDS8(a) ((u32)(s32)(s8)rzw_rd8((a), cyc))
/* the colder loops' */
#define LDS8_O(a) ({ u32 _l = (a); cyc -= rzw_c8(_l); (u32)(s32)(s8)rzw_ld8(_l); })
#define LDU8_O(a) ({ u32 _l = (a); cyc -= rzw_c8(_l); rzw_ld8(_l); })
/* LDM / STM (increment after) of work RAM; anything else stops before it */
#define WRAM_SPAN(base, nw) u32 _a = (base) & ~3u; \
                            if (!rzw_wram_span(_a, (nw))) STOP; \
                            cyc -= (nw) * ws_cyc_seq[_a >> 24][1];
/* STR, post-indexed by 4, of work RAM */
#define STR_INC(rn, val) { u32 _a = (rn) & ~3u; if (((_a >> 24) - 2) > 1) STOP; \
                           cyc -= ws_cyc_nseq[_a >> 24][1]; \
                           rzw_wram_st32(_a, (val)); rn = rn + 4; }
#define ASRS9(d) { c = (d >> 8) & 1; d = (u32)((s32)d >> 9); n = d >> 31; z = d == 0; }

  /* where pc is: one small dense switch per loop (GCC: one switch over the
   * whole range made a 471-entry jump table) */
  if (pc - 0x030032ECu < 0xC0u) {
    /* the four samples' code is the same: one body, run from the
     * instruction pc is at in its sample */
    switch (((pc - 0x030032ECu) % 0x30u) >> 2) {
    case 0: goto S_0;  case 1: goto S_1;  case 2: goto S_2;   case 3: goto S_3;
    case 4: goto S_4;  case 5: goto S_5;  case 6: goto S_6;   case 7: goto S_7;
    case 8: goto S_8;  case 9: goto S_9;  case 10: goto S_10; default: goto S_11;
    }
  }
  if (pc - 0x030036FCu < 0x70u) {
    switch ((pc - 0x030036FCu) >> 2) {
    case 0: goto B6fc; case 1: goto B700; case 2: goto B704; case 3: goto B708;
    case 4: goto B70c; case 5: goto B710; case 6: goto B714; case 7: goto B718;
    case 8: goto B71c; case 9: goto B720; case 10: goto B724; case 11: goto B728;
    case 12: goto B72c; case 13: goto B730; case 14: goto B734; case 15: goto B738;
    case 16: goto B73c; case 17: goto B740; case 18: goto B744; case 19: goto B748;
    case 20: goto B74c; case 21: goto B750; case 22: goto B754; case 23: goto B758;
    case 24: goto B75c; case 25: goto B760; case 26: goto B764; case 27: goto B768;
    }
  }
  if (pc - 0x030037B0u < 0x70u) {
    switch ((pc - 0x030037B0u) >> 2) {
    case 0: goto C7b0; case 1: goto C7b4; case 2: goto C7b8; case 3: goto C7bc;
    case 4: goto C7c0; case 5: goto C7c4; case 6: goto C7c8; case 7: goto C7cc;
    case 8: goto C7d0; case 9: goto C7d4; case 10: goto C7d8; case 11: goto C7dc;
    case 12: goto C7e0; case 13: goto C7e4; case 14: goto C7e8; case 15: goto C7ec;
    case 16: goto C7f0; case 17: goto C7f4; case 18: goto C7f8; case 19: goto C7fc;
    case 20: goto C800; case 21: goto C804; case 22: goto C808; case 23: goto C80c;
    case 24: goto C810; case 25: goto C814; case 26: goto C818; case 27: goto C81c;
    }
  }
  switch (pc) {
  case 0x030030E0u: goto A0e0;  case 0x030030E4u: goto A0e4;  case 0x030030E8u: goto A0e8;
  case 0x030030ECu: goto A0ec;  case 0x030030F0u: goto A0f0;  case 0x030033ACu: goto A3ac;
  default: return (u32)cyc;      /* the exits, the other paths: interpreted */
  }

  /* the resampler: r12 the 18.14 position, r6 its step, r8 the end, r9 the
   * samples, r7 the fraction mask, r11/r10 the volumes, r1 the L/R buffer,
   * r2 the passes left */
A0e0: SUBF(r2 = _r, r2, 1u) STEP                       /* subs r2, r2, #1 */
A0e4: if (z) B_OUT(0x030036C4u)                         /* beq (done: interpreted) */
      STEP
A0e8: r4 = r12 + (r6 << 2); STEP                        /* add r4, ip, r6, lsl #2 */
A0ec: SUBF(r4 = _r, r8, r4 >> 14) STEP                  /* subs r4, r8, r4, lsr #14 */
A0f0: if (!z && n == v) B(0x030032ECu, S_0)             /* bgt (4 more fit) */
      STEP
      STOP;                                             /* near the end: interpreted */

  /* one sample, 0x030032EC + 0x30 * k, k = 0..3 */
S_0:  r5 = r12 >> 14; STEP                              /* lsr r5, ip, #14 */
S_1:  r5 = r5 + r9; r3 = LDS8(r5); STEP                 /* ldrsb r3, [r5, r9]! */
S_2:  r4 = LDS8(r5 + 1u); STEP                          /* ldrsb r4, [r5, #1] */
S_3:  r4 = r4 - r3; STEP                                /* sub r4, r4, r3 */
S_4:  r5 = r12 & r7; STEP                               /* and r5, ip, r7 */
S_5:  r4 = r5 * r4; STEP                                /* mul r4, r5, r4 */
S_6:  r3 = r3 + (r4 >> 14); STEP                        /* add r3, r3, r4, lsr #14 */
S_7:  { WRAM_SPAN(r1, 2)                                /* ldm r1, {r4, r5} */
        r4 = rzw_wram32(_a); r5 = rzw_wram32(_a + 4); }
      STEP
S_8:  r4 = r3 * r11 + r4; STEP                          /* mla r4, r3, fp, r4 */
S_9:  r5 = r3 * r10 + r5; STEP                          /* mla r5, r3, sl, r5 */
S_10: { WRAM_SPAN(r1, 2)                                /* stmia r1!, {r4, r5} */
        rzw_wram_st32(_a, r4); rzw_wram_st32(_a + 4, r5); r1 = r1 + 8; }
      STEP
S_11: r12 = r12 + r6; STEP                              /* add ip, ip, r6 */
      if (gpc != 0x030033ACu)
        goto S_0;
A3ac: B(0x030030E0u, A0e0)                              /* b */

  /* two sources (r0, r1; r14 apart) summed, scaled by r12, 4 words to r2 */
B6fc: SUBF(r3 = _r, r3, 1u) STEP                       /* subs r3, r3, #1 */
B700: if (z) B_OUT(0x0300376Cu)                         /* beq */
      STEP
B704: r6 = LDS8_O(r0 + r14); STEP                         /* ldrsb r6, [r0, lr] */
B708: r7 = LDS8_O(r1 + r14); STEP                         /* ldrsb r7, [r1, lr] */
B70c: { u32 _a = r0; r0 = _a + 1; r4 = LDS8_O(_a); } STEP /* ldrsb r4, [r0], #1 */
B710: { u32 _a = r1; r1 = _a + 1; r5 = LDS8_O(_a); } STEP /* ldrsb r5, [r1], #1 */
B714: r10 = LDS8_O(r0 + r14); STEP                        /* ldrsb sl, [r0, lr] */
B718: r11 = LDS8_O(r1 + r14); STEP                        /* ldrsb fp, [r1, lr] */
B71c: { u32 _a = r0; r0 = _a + 1; r8 = LDS8_O(_a); } STEP /* ldrsb r8, [r0], #1 */
B720: { u32 _a = r1; r1 = _a + 1; r9 = LDS8_O(_a); } STEP /* ldrsb r9, [r1], #1 */
B724: r6 = r6 + r4; STEP                                /* add r6, r6, r4 */
B728: r6 = r6 + r5; STEP                                /* add r6, r6, r5 */
B72c: r6 = r6 + r7; STEP                                /* add r6, r6, r7 */
B730: r8 = r8 + r9; STEP                                /* add r8, r8, r9 */
B734: r8 = r8 + r10; STEP                               /* add r8, r8, sl */
B738: r8 = r8 + r11; STEP                               /* add r8, r8, fp */
B73c: r6 = r12 * r6; STEP                               /* mul r6, ip, r6 */
B740: r8 = r12 * r8; STEP                               /* mul r8, ip, r8 */
B744: ASRS9(r6) STEP                                    /* asrs r6, r6, #9 */
B748: if (n) r6 = r6 + 1; STEP                          /* addmi r6, r6, #1 */
B74c: ASRS9(r8) STEP                                    /* asrs r8, r8, #9 */
B750: if (n) r8 = r8 + 1; STEP                          /* addmi r8, r8, #1 */
B754: r6 = r6 << 7; STEP                                /* lsl r6, r6, #7 */
B758: r8 = r8 << 7; STEP                                /* lsl r8, r8, #7 */
B75c: r7 = r6; STEP                                     /* mov r7, r6 */
B760: r9 = r8; STEP                                     /* mov r9, r8 */
B764: { WRAM_SPAN(r2, 4)                                /* stmia r2!, {r6, r7, r8, r9} */
        rzw_wram_st32(_a, r6); rzw_wram_st32(_a + 4, r7);
        rzw_wram_st32(_a + 8, r8); rzw_wram_st32(_a + 12, r9); r2 = r2 + 16; }
      STEP
B768: B(0x030036FCu, B6fc)                              /* b */

  /* the mix buffer (r1) through the byte table r14 (index r12-masked) into
   * the two 8-bit outputs r0 and r3, 8 samples a pass, r2 the passes left */
C7b0: SUBF(r2 = _r, r2, 1u) STEP                       /* subs r2, r2, #1 */
C7b4: if (z) B_OUT(0x03003820u)                         /* beq */
      STEP
C7b8: { WRAM_SPAN(r1, 8)                                /* ldm r1!, {r4-fp} */
        r4 = rzw_wram32(_a);       r5 = rzw_wram32(_a + 4);
        r6 = rzw_wram32(_a + 8);   r7 = rzw_wram32(_a + 12);
        r8 = rzw_wram32(_a + 16);  r9 = rzw_wram32(_a + 20);
        r10 = rzw_wram32(_a + 24); r11 = rzw_wram32(_a + 28); r1 = r1 + 32; }
      STEP
C7bc: r4 = r12 & (r4 >> 7); STEP                        /* and r4, ip, r4, lsr #7 */
C7c0: r5 = r12 & (r5 >> 7); STEP
C7c4: r6 = r12 & (r6 >> 7); STEP
C7c8: r7 = r12 & (r7 >> 7); STEP
C7cc: r8 = r12 & (r8 >> 7); STEP
C7d0: r9 = r12 & (r9 >> 7); STEP
C7d4: r10 = r12 & (r10 >> 7); STEP
C7d8: r11 = r12 & (r11 >> 7); STEP
C7dc: r4 = LDU8_O(r14 + r4); STEP                         /* ldrb r4, [lr, r4] */
C7e0: r5 = LDU8_O(r14 + r5); STEP
C7e4: r6 = LDU8_O(r14 + r6); STEP
C7e8: r7 = LDU8_O(r14 + r7); STEP
C7ec: r8 = LDU8_O(r14 + r8); STEP
C7f0: r9 = LDU8_O(r14 + r9); STEP
C7f4: r10 = LDU8_O(r14 + r10); STEP
C7f8: r11 = LDU8_O(r14 + r11); STEP
C7fc: r4 = r4 | (r6 << 8); STEP                         /* orr r4, r4, r6, lsl #8 */
C800: r4 = r4 | (r8 << 16); STEP                        /* orr r4, r4, r8, lsl #16 */
C804: r4 = r4 | (r10 << 24); STEP                       /* orr r4, r4, sl, lsl #24 */
C808: STR_INC(r0, r4) STEP                              /* str r4, [r0], #4 */
C80c: r5 = r5 | (r7 << 8); STEP                         /* orr r5, r5, r7, lsl #8 */
C810: r5 = r5 | (r9 << 16); STEP                        /* orr r5, r5, r9, lsl #16 */
C814: r5 = r5 | (r11 << 24); STEP                       /* orr r5, r5, fp, lsl #24 */
C818: STR_INC(r3, r5) STEP                              /* str r5, [r3], #4 */
C81c: B(0x030037B0u, C7b0)                              /* b */
#undef STOP
#undef STEP
#undef B
#undef B_OUT
#undef SUBF
#undef LDS8
#undef LDS8_O
#undef LDU8_O
#undef WRAM_SPAN
#undef STR_INC
#undef ASRS9

done:
  if (cyc == cyc0)                     /* stopped before its first instruction */
    return (u32)cyc;
  reg[0] = r0; reg[1] = r1; reg[2] = r2; reg[3] = r3; reg[4] = r4; reg[5] = r5;
  reg[6] = r6; reg[7] = r7; reg[8] = r8; reg[9] = r9; reg[10] = r10;
  reg[11] = r11; reg[12] = r12; reg[14] = r14;
  reg[REG_PC] = gpc;
  return (u32)cyc | ((u64)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}
