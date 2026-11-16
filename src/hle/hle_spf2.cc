/* hle_spf2.cc -- Super Puzzle Fighter II's sound mixing loops, run natively.
 *
 * The contract is gba_hle.h's (see hle_vrally3.cc): the interpreter's exact
 * memory accesses, cycle charges and stopping points (cycles <= 0 after an
 * instruction: stop with the PC on the next one), so the machine state is
 * the same instruction by instruction (REC/hash). The interpreter calls it
 * when an ARM jump lands in [0x03000038, 0x0300026C); it runs the two loops
 * below from any of their instructions and declines every other PC there.
 *
 * Capcom's own sound driver (not M4A, so the M4A HLE never fires) copies
 * its mixer to IWRAM 0x03000000 (ROM 0x0834A838) and its ADPCM decoder to
 * 0x0300015C (ROM 0x0834A958). In gameplay the two inner loops were 33% of
 * the guest instructions (host PC histogram, 2026-11-15):
 *   0x03000038-0x03000060  one PCM channel added into the mix buffer:
 *                          20.12 resampling, volume, 11 instructions a byte
 *   0x03000190-0x03000268  4-bit ADPCM, each nibble decoded and added to two
 *                          bytes of the buffer
 * A store outside work RAM stops it before the store; the loops' exits are
 * interpreted. The loops' opcodes are hashed once a frame and two of each
 * checked on every entry.
 */
extern "C" {
#include "../gpsp/common.h"   /* gpSP's: another common.h is earlier in the path */
}
#include "gba_hle.h"
#include "../porting/gba_idle.h"

#define SPF_FNV 0xcc646792u   /* 0x03000038-0x03000060, 0x03000190-0x03000268 */

/* fast_read_memory(8): the caller sign- or zero-extends */
static inline u32 spf_rd8(u32 a, s32 &cyc)
{
  u8 *map;
  if (a < 0x10000000)
    cyc -= ws_cyc_nseq[a >> 24][0];
  if ((a >> 24) == 0 || (a & 0xF0000000) || !(map = memory_map_read[a >> 15]))
    return (u8)read_memory8(a);
  return readaddress8(map, a & 0x7FFF);
}

/* fast_read_memory(32) */
static inline u32 spf_ld32(u32 a, s32 &cyc)
{
  u8 *map;
  if (a < 0x10000000)
    cyc -= ws_cyc_nseq[a >> 24][1];
  if ((a >> 24) == 0 || (a & 0xF0000003) || !(map = memory_map_read[a >> 15]))
    return read_memory32(a);
  return readaddress32(map, a & 0x7FFF);
}

/* the frame + 1 the opcodes were last hashed in (0: never); zero-initialised
 * at the end of .bss: a .data word would move the RAM code after it */
#ifndef HOST_BUILD
static u32 spf_checked_frame __attribute__((section(".gba_tail_bss")));
#else
static u32 spf_checked_frame;
#endif

extern "C" uint64_t hle_spf2_run(uint32_t pc, int32_t cycles, uint32_t nzcv)
{
  s32 cyc = cycles;   /* gpSP's s32 (int; int32_t is long on the device) */
  u8 *code = memory_map_read[0x03000000u >> 15];
  if (!code)
    return (u32)cyc;
#define SPF_OP(a) readaddress32(code, (a) & 0x7FFF)
  if (pc < 0x03000190u) {
    if (pc > 0x03000060u || SPF_OP(0x03000048u) != 0xe19370d6u ||
        SPF_OP(0x03000058u) != 0xe4c17001u)
      return (u32)cyc;
  } else {
    if (pc > 0x03000268u || SPF_OP(0x030001d0u) != 0xe0130793u ||
        SPF_OP(0x03000204u) != 0xe4c14001u)
      return (u32)cyc;
  }
  if (spf_checked_frame != frame_counter + 1) {
    u32 h = 2166136261u;
    for (u32 a = 0x03000038u; a <= 0x03000060u; a += 4)
      h = (h ^ SPF_OP(a)) * 16777619u;
    for (u32 a = 0x03000190u; a <= 0x03000268u; a += 4)
      h = (h ^ SPF_OP(a)) * 16777619u;
    if (h != SPF_FNV)
      return (u32)cyc;
    spf_checked_frame = frame_counter + 1;
  }
#undef SPF_OP

  u32 r1 = reg[1], r2 = reg[2], r3 = reg[3], r4 = reg[4], r5 = reg[5];
  u32 r6 = reg[6], r7 = reg[7], r8 = reg[8], r9 = reg[9], r10 = reg[10];
  u32 r11 = reg[11], r12 = reg[12], r14 = reg[14];
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const s32 F = ws_cyc_seq[3][1];      /* each instruction's fetch (IWRAM) */
  const s32 BT = ws_cyc_nseq[3][1];    /* a taken branch (to IWRAM) */
  const s32 cyc0 = cyc;                /* every instruction charges: ran <=> cyc < cyc0 */
  u32 npc;

#define EXIT(nx)  { npc = (nx); goto done; }
#define STEP(nx)  { cyc -= F; if (__builtin_expect(cyc <= 0, 0)) EXIT(nx); }
/* a taken B: its target's fetch and the next instruction's */
#define B(tgt, lab) { cyc -= BT + F; if (__builtin_expect(cyc <= 0, 0)) EXIT(tgt); goto lab; }
#define SUBF(d, x, y) { u32 _x = (x), _y = (y), _r = _x - _y; n = _r >> 31; z = _r == 0; \
                        c = _x >= _y; v = ((_x ^ _y) & (_x ^ _r)) >> 31; d; }
/* LDRSB / LDRB */
#define LDS8(a) ((u32)(s32)(s8)spf_rd8((a), cyc))
#define LDU8(a) spf_rd8((a), cyc)
/* STRB, post-indexed by 1: work RAM only (cpu_store8's fast path), any
 * other address stops before the instruction */
#define STB_INC(rn, val, here) { u32 _a = (rn); if (((_a >> 24) - 2) > 1) EXIT(here); \
                                 cyc -= ws_cyc_nseq[_a >> 24][0]; \
                                 *(memory_map_read[_a >> 15] + (_a & 0x7FFF)) = (u8)(val); \
                                 gba_idle_sidefx++; rn = _a + 1; }

  switch (pc) {
  case 0x03000038u: goto L038;   case 0x0300003Cu: goto L03c;
  case 0x03000040u: goto L040;   case 0x03000044u: goto L044;
  case 0x03000048u: goto L048;   case 0x0300004Cu: goto L04c;
  case 0x03000050u: goto L050;   case 0x03000054u: goto L054;
  case 0x03000058u: goto L058;   case 0x0300005Cu: goto L05c;
  case 0x03000060u: goto L060;
  case 0x03000190u: goto L190;   case 0x03000194u: goto L194;
  case 0x03000198u: goto L198;   case 0x0300019Cu: goto L19c;
  case 0x030001A0u: goto L1a0;   case 0x030001A4u: goto L1a4;
  case 0x030001A8u: goto L1a8;   case 0x030001ACu: goto L1ac;
  case 0x030001B0u: goto L1b0;   case 0x030001B4u: goto L1b4;
  case 0x030001B8u: goto L1b8;   case 0x030001BCu: goto L1bc;
  case 0x030001C0u: goto L1c0;   case 0x030001C4u: goto L1c4;
  case 0x030001C8u: goto L1c8;   case 0x030001CCu: goto L1cc;
  case 0x030001D0u: goto L1d0;   case 0x030001D4u: goto L1d4;
  case 0x030001D8u: goto L1d8;   case 0x030001DCu: goto L1dc;
  case 0x030001E0u: goto L1e0;   case 0x030001E4u: goto L1e4;
  case 0x030001E8u: goto L1e8;   case 0x030001ECu: goto L1ec;
  case 0x030001F0u: goto L1f0;   case 0x030001F4u: goto L1f4;
  case 0x030001F8u: goto L1f8;   case 0x030001FCu: goto L1fc;
  case 0x03000200u: goto L200;   case 0x03000204u: goto L204;
  case 0x03000208u: goto L208;   case 0x0300020Cu: goto L20c;
  case 0x03000230u: goto L230;   case 0x03000234u: goto L234;
  case 0x03000238u: goto L238;   case 0x0300023Cu: goto L23c;
  case 0x03000240u: goto L240;   case 0x03000264u: goto L264;
  case 0x03000268u: goto L268;
  default: return (u32)cyc;      /* the exits, the setup: interpreted */
  }

  /* the mixer: r3 the samples, r4 the 20.12 position, r5 its step, r10 the
   * end, r9 the volume, r1 the buffer, r2 the bytes left */
L038: SUBF(, r4, r10) STEP(0x0300003Cu)                   /* cmp r4, sl */
L03c: if (n == v) { cyc -= BT + F; EXIT(0x0300008Cu); }   /* bge (the end: interpreted) */
      STEP(0x03000040u)
L040: r6 = (u32)((s32)r4 >> 12); STEP(0x03000044u)        /* asr r6, r4, #12 */
L044: r4 = r4 + r5; STEP(0x03000048u)                     /* add r4, r4, r5 */
L048: r7 = LDS8(r3 + r6); STEP(0x0300004Cu)               /* ldrsb r7, [r3, r6] */
L04c: r8 = LDS8(r1); STEP(0x03000050u)                    /* ldrsb r8, [r1] */
L050: r7 = r9 * r7; STEP(0x03000054u)                     /* mul r7, sb, r7 */
L054: r7 = r8 + (u32)((s32)r7 >> 8); STEP(0x03000058u)    /* add r7, r8, r7, asr #8 */
L058: STB_INC(r1, r7, 0x03000058u) STEP(0x0300005Cu)      /* strb r7, [r1], #1 */
L05c: SUBF(r2 = _r, r2, 1u) STEP(0x03000060u)             /* subs r2, r2, #1 */
L060: if (!z) B(0x03000038u, L038)                        /* bne */
      STEP(0x03000064u)
      EXIT(0x03000064u);

  /* the ADPCM decoder: r6 the nibbles, r11 which half, r9 the step index,
   * r5 the tables, r8 the predictor, r14 the volume, r12 the nibbles left */
L190: n = r11 >> 31; z = r11 == 0; STEP(0x03000194u)      /* tst fp, fp */
L194: if (z) B(0x030001A0u, L1a0)                         /* beq */
      STEP(0x03000198u)
L198: r3 = r10 & 0xFu; STEP(0x0300019Cu)                  /* and r3, sl, #0xf */
L19c: B(0x030001A8u, L1a8)                                /* b */
L1a0: { u32 _a = r6; r6 = _a + 1; r10 = LDU8(_a); }       /* ldrb sl, [r6], #1 */
      STEP(0x030001A4u)
L1a4: r3 = r10 >> 4; STEP(0x030001A8u)                    /* lsr r3, sl, #4 */
L1a8: r11 = ~r11; STEP(0x030001ACu)                       /* mvn fp, fp */
L1ac: r4 = r5 + (r3 << 2); STEP(0x030001B0u)              /* add r4, r5, r3, lsl #2 */
L1b0: r4 = spf_ld32(r4, cyc); STEP(0x030001B4u)           /* ldr r4, [r4] */
L1b4: r9 = r9 + r4; STEP(0x030001B8u)                     /* add sb, sb, r4 */
L1b8: SUBF(, r9, 0u) STEP(0x030001BCu)                    /* cmp sb, #0 */
L1bc: if (n != v) r9 = 0; STEP(0x030001C0u)               /* movlt sb, #0 */
L1c0: SUBF(, r9, 0x58u) STEP(0x030001C4u)                 /* cmp sb, #0x58 */
L1c4: if (!z && n == v) r9 = 0x58u; STEP(0x030001C8u)     /* movgt sb, #0x58 */
L1c8: r4 = r3 & 8u; STEP(0x030001CCu)                     /* and r4, r3, #8 */
L1cc: r3 = r3 & 7u; STEP(0x030001D0u)                     /* and r3, r3, #7 */
L1d0: r3 = r3 * r7; n = r3 >> 31; z = r3 == 0;            /* muls r3, r3, r7 */
      STEP(0x030001D4u)
L1d4: r3 = r3 >> 2; STEP(0x030001D8u)                     /* lsr r3, r3, #2 */
L1d8: n = r4 >> 31; z = r4 == 0; STEP(0x030001DCu)        /* tst r4, r4 */
L1dc: if (!z) r8 = r8 - r3; STEP(0x030001E0u)             /* subne r8, r8, r3 */
L1e0: if (z) r8 = r8 + r3; STEP(0x030001E4u)              /* addeq r8, r8, r3 */
L1e4: r3 = r9 << 2; STEP(0x030001E8u)                     /* lsl r3, sb, #2 */
L1e8: r3 = r3 + 0x40u; STEP(0x030001ECu)                  /* add r3, r3, #0x40 */
L1ec: r3 = r3 + r5; STEP(0x030001F0u)                     /* add r3, r3, r5 */
L1f0: r7 = spf_ld32(r3, cyc); STEP(0x030001F4u)           /* ldr r7, [r3] */
L1f4: r3 = r8 * r14; STEP(0x030001F8u)                    /* mul r3, r8, lr */
L1f8: r3 = r3 >> 16; STEP(0x030001FCu)                    /* lsr r3, r3, #16 */
L1fc: r4 = LDS8(r1); STEP(0x03000200u)                    /* ldrsb r4, [r1] */
L200: r4 = r4 + r3; STEP(0x03000204u)                     /* add r4, r4, r3 */
L204: STB_INC(r1, r4, 0x03000204u) STEP(0x03000208u)      /* strb r4, [r1], #1 */
L208: SUBF(r2 = _r, r2, 1u) STEP(0x0300020Cu)             /* subs r2, r2, #1 */
L20c: if (!z) B(0x03000230u, L230)                        /* bne */
      STEP(0x03000210u)
      EXIT(0x03000210u);                                  /* the buffer is full */
L230: r4 = LDS8(r1); STEP(0x03000234u)                    /* ldrsb r4, [r1] */
L234: r4 = r4 + r3; STEP(0x03000238u)                     /* add r4, r4, r3 */
L238: STB_INC(r1, r4, 0x03000238u) STEP(0x0300023Cu)      /* strb r4, [r1], #1 */
L23c: SUBF(r2 = _r, r2, 1u) STEP(0x03000240u)             /* subs r2, r2, #1 */
L240: if (!z) B(0x03000264u, L264)                        /* bne */
      STEP(0x03000244u)
      EXIT(0x03000244u);
L264: SUBF(r12 = _r, r12, 1u) STEP(0x03000268u)           /* subs ip, ip, #1 */
L268: if (!z) B(0x03000190u, L190)                        /* bne */
      STEP(0x0300026Cu)
      EXIT(0x0300026Cu);
#undef EXIT
#undef STEP
#undef B
#undef SUBF
#undef LDS8
#undef LDU8
#undef STB_INC

done:
  if (cyc == cyc0)                     /* stopped before its first instruction */
    return (u32)cyc;
  reg[1] = r1; reg[2] = r2; reg[3] = r3; reg[4] = r4; reg[5] = r5;
  reg[6] = r6; reg[7] = r7; reg[8] = r8; reg[9] = r9; reg[10] = r10;
  reg[11] = r11; reg[12] = r12; reg[14] = r14;
  reg[REG_PC] = npc;
  return (u32)cyc | ((u64)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}
