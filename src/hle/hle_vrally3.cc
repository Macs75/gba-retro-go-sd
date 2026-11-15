/* hle_vrally3.cc -- V-Rally 3's polygon rasterizer, run natively.
 *
 * The contract is gba_hle.h's: the routine runs with the interpreter's exact
 * memory accesses, cycle charges and stopping points (cycles <= 0 after an
 * instruction: stop with the PC on the next one), so the machine state is
 * the same instruction by instruction (REC/hash). The interpreter calls it
 * when a jump lands anywhere in the routine; it may enter at any of its
 * instructions. On the device this unit is an overlay copied to ITCM only
 * for this game (gba_hle_select).
 *
 * Device, V-Rally 3's race (2026-11-15): emulation 7.09M -> 5.28M cycles a
 * frame (89% of the budget), 15 -> 54 frames shown a second.
 */
extern "C" {
#include "../gpsp/common.h"   /* gpSP's: another common.h is earlier in the path */
}
#include "gba_hle.h"

/* V-Rally 3 (AVRE): its textured-polygon rasterizer, 0x03003AB0-0x03003D04
 * in IWRAM (tools/gba_hle_vrally3_gen.py -> hle_vrally3.inc). A store
 * outside work RAM / VRAM (which could raise an alert) stops it before the
 * store; so does the epilogue. The 150 opcodes are hashed once a frame and
 * four of them checked on every entry. */
#define VR3_FNV 0xb1cea218u

static inline u32 vr3_ld32(u32 a, s32 &cyc)
{
  u8 *map;
  if (a < 0x10000000)
    cyc -= ws_cyc_nseq[a >> 24][1];
  /* fast_read_memory(32): BIOS (the pc is in IWRAM), unaligned or unmapped
   * reads go through read_memory32 */
  if ((a >> 24) == 0 || (a & 0xF0000003) || !(map = memory_map_read[a >> 15]))
    return read_memory32(a);
  return readaddress32(map, a & 0x7FFF);
}

/* the byte read itself, one copy (it was inlined at the 22 texel loads:
 * the routine did not fit ITCM); the caller charges the cycles */
static __attribute__((noinline)) u32 vr3_rd8(u32 a)
{
  u8 *map;
  if ((a >> 24) == 0 || (a & 0xF0000000) || !(map = memory_map_read[a >> 15]))
    return (u8)read_memory8(a);
  return readaddress8(map, a & 0x7FFF);
}

/* the block transfers' load (load_aligned32): sequential cycles */
static inline u32 vr3_pop32(u32 a, s32 &cyc)
{
  u8 *map;
  if (a < 0x10000000)
    cyc -= ws_cyc_seq[a >> 24][1];
  if (a < 0x10000000 && (map = memory_map_read[a >> 15]))
    return readaddress32(map, a & 0x7FFF);
  return read_memory32(a);
}

/* the frame + 1 the opcodes were last hashed in (0: never); zero-initialised
 * at the end of .bss: a .data word would move the RAM code after it */
#ifndef HOST_BUILD
static u32 vr3_checked_frame __attribute__((section(".gba_tail_bss")));
#else
static u32 vr3_checked_frame;
#endif

/* GCC: built -O2 (Makefile; -O3 made it 6.5 KB); the byte read is one
 * shared copy, the stopping PC an index (see below): it runs from ITCM. */
extern "C" uint64_t hle_vrally3_run(uint32_t pc, int32_t cycles, uint32_t nzcv)
{
  s32 cyc = cycles;   /* gpSP's s32 (int; int32_t is long on the device) */
  u8 *code = memory_map_read[0x03003AB0u >> 15];
  if (!code)
    return (u32)cyc;
#define VR3_OP(a) readaddress32(code, (a) & 0x7FFF)
  if (VR3_OP(0x03003B28u) != 0xe04ef0acu || VR3_OP(0x03003B88u) != 0xe206ecffu ||
      VR3_OP(0x03003BF8u) != 0xe04ef0acu || VR3_OP(0x03003CB4u) != 0xe04cf0aeu)
    return (u32)cyc;
  if (vr3_checked_frame != frame_counter + 1) {
    u32 h = 2166136261u;
    for (u32 a = 0x03003AB0u; a <= 0x03003D04u; a += 4)
      h = (h ^ VR3_OP(a)) * 16777619u;
    if (h != VR3_FNV)
      return (u32)cyc;
    vr3_checked_frame = frame_counter + 1;
  }
#undef VR3_OP

  u32 r0 = reg[0], r1 = reg[1], r2 = reg[2], r3 = reg[3], r4 = reg[4];
  u32 r5 = reg[5], r6 = reg[6], r7 = reg[7], r8 = reg[8], r9 = reg[9];
  u32 r10 = reg[10], r11 = reg[11], r12 = reg[12], r13 = reg[13], r14 = reg[14];
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const s32 F = ws_cyc_seq[3][1];      /* each instruction's fetch (IWRAM) */
  const s32 BT = ws_cyc_nseq[3][1];    /* a taken branch (to IWRAM) */
  const s32 cyc0 = cyc;                /* every instruction charges: ran <=> cyc < cyc0 */
  u32 npc, nidx;

#define LD32(a)  vr3_ld32((a), cyc)
#define LD8(a)   ({ u32 _b = (a); if (_b < 0x10000000) cyc -= ws_cyc_nseq[_b >> 24][0]; vr3_rd8(_b); })
#define POP32(a) vr3_pop32((a), cyc)
#define ST_OK(a) ((a) >> 24 == 2 || (a) >> 24 == 3 || (a) >> 24 == 6)
#define ST32(a, val) { cyc -= ws_cyc_nseq[(a) >> 24][1]; cpu_hle_store32((a), (val)); }
#define LSLR(x, s) (((s) & 0xFF) <= 31 ? (x) << ((s) & 0xFF) : 0u)
#define LSRR(x, s) (((s) & 0xFF) <= 31 ? (x) >> ((s) & 0xFF) : 0u)
#define RORR(x, s) ({ u32 _v; u32 _x = (x); u32 _s = (s) & 0xFF; ror(_v, _x, _s); _v; })
/* the stopping PC as a small index into the routine (a 2-byte move at each
 * of the 150 exits instead of a 32-bit constant) */
#define STEP(nx, cost) { cyc -= (cost); if (__builtin_expect(cyc <= 0, 0)) { nidx = ((nx) - VR3_BASE) >> 2; goto done_idx; } }
#define EXIT_DONE(nx)  { npc = (nx); goto done; }
#define EXIT(nx)       { nidx = ((nx) - VR3_BASE) >> 2; goto done_idx; }
#include "hle_vrally3.inc"
#undef LD32
#undef LD8
#undef POP32
#undef ST_OK
#undef ST32
#undef LSLR
#undef LSRR
#undef RORR
#undef STEP
#undef EXIT_DONE
#undef EXIT

done_idx:
  npc = VR3_BASE + 4 * nidx;
done:
  if (cyc == cyc0)                     /* stopped before its first instruction */
    return (u32)cyc;
  reg[0] = r0; reg[1] = r1; reg[2] = r2; reg[3] = r3; reg[4] = r4;
  reg[5] = r5; reg[6] = r6; reg[7] = r7; reg[8] = r8; reg[9] = r9;
  reg[10] = r10; reg[11] = r11; reg[12] = r12; reg[13] = r13; reg[14] = r14;
  reg[REG_PC] = npc;
  return (u32)cyc | ((u64)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}

