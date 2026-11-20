/* m4a_reverb.c -- M4A's reverb loop, run natively.
 *
 * SoundMainRAM (the M4A sound driver's IWRAM routine) runs its reverb before
 * mixing the channels: once a frame, a loop over the frame's samples that
 * folds the previous buffer back into this one, 15 ARM instructions a
 * sample (Advance Wars: 6% of the guest's instructions, 2026-11-20):
 *
 *   L: ldrsb r0, [r5, r6]     ; this buffer's right sample
 *      ldrsb r1, [r5]         ; ...and left
 *      add   r0, r0, r1
 *      ldrsb r1, [r7, r6]     ; the other buffer's two
 *      add   r0, r0, r1
 *      ldrsb r1, [r7], #1
 *      add   r0, r0, r1
 *      mul   r1, r0, r3       ; r3: the reverb level
 *      asr   r0, r1, #9
 *      tst   r0, #0x80
 *      addne r0, r0, #1
 *      strb  r0, [r5, r6]
 *      strb  r0, [r5], #1
 *      subs  r4, r4, #1
 *      bgt   L
 *
 * Found by those bytes in IWRAM once the mixer is (m4a_gpsp.c), wherever the
 * driver copied it; run through the interpreter's native-routine hook
 * (gba_hle.h: game_hle_*) when no game HLE holds it. The contract is
 * src/hle's: the interpreter's exact accesses, cycle charges and stopping
 * points, from any of its instructions; a store outside work RAM stops
 * before it. In the flash with the cold code: no RAM moves (once a frame,
 * through a pointer: no veneer). No tables: an orphan .rodata would land on
 * the HLE overlays in gba.xip.
 */
#include "../gpsp/common.h"
#include "../porting/gba_idle.h"
#include "m4a_reverb.h"

#ifndef HOST_BUILD
#define REV_COLD __attribute__((section(".gba_cold_text")))
#define REV_TAIL __attribute__((section(".gba_tail_bss")))
#else
#define REV_COLD
#define REV_TAIL
#endif

#define REV_SIZE 0x3Cu            /* 15 instructions */

static u32 rev_pc REV_TAIL;       /* the loop's head while hooked, else 0 */

/* the loop's 15 words at a (an IWRAM pointer) */
static REV_COLD int rev_is_loop(const u8 *a)
{
  const u32 *w = (const u32 *)a;
  return w[0] == 0xe19500d6u && w[1] == 0xe1d510d0u && w[2] == 0xe0800001u &&
         w[3] == 0xe19710d6u && w[4] == 0xe0800001u && w[5] == 0xe0d710d1u &&
         w[6] == 0xe0800001u && w[7] == 0xe0010390u && w[8] == 0xe1a004c1u &&
         w[9] == 0xe3100080u && w[10] == 0x12800001u && w[11] == 0xe7c50006u &&
         w[12] == 0xe4c50001u && w[13] == 0xe2544001u && w[14] == 0xcafffff0u;
}

/* fast_read_memory(8) */
static inline __attribute__((always_inline)) u32 rev_rd8(u32 a, s32 *cyc)
{
  u8 *map;
  if (a < 0x10000000)
    *cyc -= ws_cyc_nseq[a >> 24][0];
  if ((a >> 24) == 0 || (a & 0xF0000000) || !(map = memory_map_read[a >> 15]))
    return (u8)read_memory8(a);
  return readaddress8(map, a & 0x7FFF);
}

static REV_COLD unsigned long long m4a_reverb_run(u32 pc, s32 cycles, u32 nzcv)
{
  s32 cyc = cycles;
  u32 off = pc - rev_pc;
  u8 *iw = memory_map_read[0x03000000u >> 15];
  if (off >= REV_SIZE || (off & 3) || !iw || !rev_is_loop(iw + (rev_pc & 0x7FFF)))
    return (u32)cyc;

  u32 r0 = reg[0], r1 = reg[1], r3 = reg[3], r4 = reg[4], r5 = reg[5];
  u32 r6 = reg[6], r7 = reg[7];
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const s32 F = ws_cyc_seq[3][1];      /* each instruction's fetch (IWRAM) */
  const s32 BT = ws_cyc_nseq[3][1];    /* a taken branch (to IWRAM) */
  const s32 cyc0 = cyc;
  u32 gpc = pc;                        /* the instruction about to run */

#define STOP      goto done
#define STEP      { gpc += 4; cyc -= F; if (cyc <= 0) STOP; }
#define LDS8(a)   ((u32)(s32)(s8)rev_rd8((a), &cyc))
/* STRB of work RAM; anything else stops before it */
#define STB(a, val) { u32 _a = (a); if (((_a >> 24) - 2) > 1) STOP; \
                      cyc -= ws_cyc_nseq[_a >> 24][0]; \
                      *(memory_map_read[_a >> 15] + (_a & 0x7FFF)) = (u8)(val); \
                      gba_idle_sidefx++; }

  switch (off >> 2) {
  case 0: goto I0;   case 1: goto I1;   case 2: goto I2;   case 3: goto I3;
  case 4: goto I4;   case 5: goto I5;   case 6: goto I6;   case 7: goto I7;
  case 8: goto I8;   case 9: goto I9;   case 10: goto I10; case 11: goto I11;
  case 12: goto I12; case 13: goto I13; default: goto I14;
  }
I0:  r0 = LDS8(r5 + r6); STEP                          /* ldrsb r0, [r5, r6] */
I1:  r1 = LDS8(r5); STEP                               /* ldrsb r1, [r5] */
I2:  r0 = r0 + r1; STEP                                /* add r0, r0, r1 */
I3:  r1 = LDS8(r7 + r6); STEP                          /* ldrsb r1, [r7, r6] */
I4:  r0 = r0 + r1; STEP                                /* add r0, r0, r1 */
I5:  { u32 _a = r7; r7 = _a + 1; r1 = LDS8(_a); } STEP /* ldrsb r1, [r7], #1 */
I6:  r0 = r0 + r1; STEP                                /* add r0, r0, r1 */
I7:  r1 = r0 * r3; STEP                                /* mul r1, r0, r3 */
I8:  r0 = (u32)((s32)r1 >> 9); STEP                    /* asr r0, r1, #9 */
I9:  n = 0; z = (r0 & 0x80u) == 0; STEP                /* tst r0, #0x80 (C, V kept) */
I10: if (!z) r0 = r0 + 1; STEP                         /* addne r0, r0, #1 */
I11: STB(r5 + r6, r0) STEP                             /* strb r0, [r5, r6] */
I12: STB(r5, r0) r5 = r5 + 1; STEP                     /* strb r0, [r5], #1 */
I13: { u32 _r = r4 - 1; n = _r >> 31; z = _r == 0;      /* subs r4, r4, #1 */
       c = r4 >= 1; v = ((r4 ^ 1u) & (r4 ^ _r)) >> 31; r4 = _r; }
     STEP
I14: if (!z && n == v) {                               /* bgt L */
       gpc = rev_pc; cyc -= BT + F;
       if (cyc <= 0) STOP;
       goto I0;
     }
     STEP                                              /* out: interpreted */
#undef STOP
#undef STEP
#undef LDS8
#undef STB

done:
  if (cyc == cyc0)                     /* stopped before its first instruction */
    return (u32)cyc;
  reg[0] = r0; reg[1] = r1; reg[4] = r4; reg[5] = r5; reg[7] = r7;
  reg[REG_PC] = gpc;
  return (u32)cyc | ((unsigned long long)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}

/* m4a_gpsp.c, when the mixer is found: the loop, if IWRAM holds it and the
 * native-routine hook is free */
REV_COLD void m4a_reverb_find(void)
{
  u8 *iw = memory_map_read[0x03000000u >> 15];
  if (!iw || game_hle_fn)
    return;
  for (u32 o = 0; o + REV_SIZE <= 0x8000u; o += 4) {
    if (rev_is_loop(iw + o)) {
      rev_pc = 0x03000000u + o;
      game_hle_pc = rev_pc;
      game_hle_size = REV_SIZE;
      game_hle_fn = m4a_reverb_run;
      return;
    }
  }
}

/* m4a_gpsp.c's reset: the hook is ours to drop */
REV_COLD void m4a_reverb_reset(void)
{
  if (rev_pc && game_hle_fn == m4a_reverb_run) {
    game_hle_fn = 0;
    game_hle_pc = 0;
    game_hle_size = 0;
  }
  rev_pc = 0;
}
