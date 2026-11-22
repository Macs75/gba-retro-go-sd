/* hle_wcp.cc -- Wing Commander: Prophecy's 3D routines, run natively.
 *
 * The contract is gba_hle.h's: the interpreter's exact memory accesses,
 * cycle charges and stopping points, so the machine state is the same as
 * the interpreter's. The interpreter calls it when a jump lands in
 * 0x03002230-0x03002B18; it runs the game's IWRAM 3D there
 * (tools/gba_hle_wcp_gen.py -> hle_wcp.inc): the vertex transform, the
 * projection, the textured span drawer and the polygon edge walker's loop
 * that calls it a line at a time (BL; its return, BX, comes back natively),
 * entered at their starts, branch targets and return sites, and declines
 * the rest of that range.
 *
 * A checkpoint at each of those and every 16th instruction runs what
 * follows only when the cycles left exceed the most it can cost (the
 * fetches, taken branches, each access at the slowest wait state of the
 * memory these routines use): no stop (cycles <= 0 after an instruction)
 * can fall in it. Otherwise it returns there and the interpreter goes on;
 * so do the returns (BX) to code outside them. The routines' words are
 * hashed once a frame. On the device this unit is an overlay copied to ITCM
 * only for this game (gba_hle_select).
 */
extern "C" {
#include "../gpsp/common.h"   /* gpSP's: another common.h is earlier in the path */
}
#include "gba_hle.h"

extern "C" unsigned int gba_idle_sidefx;

/* the byte load (fast_read_memory(8) of cpu.cc): BIOS, unmapped through
 * read_memory8; the caller charges the cycles */
static __attribute__((noinline)) u32 wc_rd8(u32 a)
{
  u8 *map;
  if ((a >> 24) == 0 || (a & 0xF0000000) || !(map = memory_map_read[a >> 15]))
    return (u8)read_memory8(a);
  return readaddress8(map, a & 0x7FFF);
}

/* the word load (fast_read_memory(32)): BIOS, unaligned, unmapped through
 * read_memory32; the caller charges */
static __attribute__((noinline)) u32 wc_rd32(u32 a)
{
  u8 *map;
  if ((a >> 24) == 0 || (a & 0xF0000003) || !(map = memory_map_read[a >> 15]))
    return read_memory32(a);
  return readaddress32(map, a & 0x7FFF);
}

/* the halfword loads (fast_read_memory(16)): BIOS, unaligned, unmapped
 * through read_memory16(_signed); the caller charges */
static __attribute__((noinline)) u32 wc_rd16(u32 a)
{
  u8 *map;
  if ((a >> 24) == 0 || (a & 0xF0000001) || !(map = memory_map_read[a >> 15]))
    return (u16)read_memory16(a);
  return readaddress16(map, a & 0x7FFF);
}

static __attribute__((noinline)) u32 wc_rd16s(u32 a)
{
  u8 *map;
  if ((a >> 24) == 0 || (a & 0xF0000001) || !(map = memory_map_read[a >> 15]))
    return (u32)(s32)(s16)read_memory16_signed(a);
  return (u32)(s32)(s16)readaddress16(map, a & 0x7FFF);
}

/* The loads with their charge, out of line: the value in the low word, the
 * cycles left in the high (r0:r1), so a call site is a call and two moves
 * (the inline charge took most of the routines' ITCM). */
#define WC_LOADC(name, rd, idx)                                                \
  static __attribute__((noinline)) u64 name(u32 a, s32 cyc)                   \
  {                                                                           \
    if (a < 0x10000000)                                                       \
      cyc -= ws_cyc_nseq[a >> 24][idx];                                       \
    return (u32)rd(a) | ((u64)(u32)cyc << 32);                                \
  }
WC_LOADC(wc_ldb, wc_rd8, 0)
WC_LOADC(wc_ldh, wc_rd16, 0)
WC_LOADC(wc_ldsh, wc_rd16s, 0)
WC_LOADC(wc_ldw, wc_rd32, 1)
#undef WC_LOADC

/* a block store into VRAM: write_memory32's word stores (the 96 KB folded
 * into the 128 KB window) through one pointer when the words do not cross
 * a fold; the side effects counted by the caller */
static __attribute__((noinline)) void wc_vram_stm(u32 a, const u32 *v, u32 n)
{
  for (u32 k = 0; k < n; k++, a += 4) {
    u32 o = a & 0x1FFFF;
    if (o >= 0x18000)
      o -= 0x8000;
    address32(vram, o) = eswap32(v[k]);
  }
}

/* the frame + 1 the routine's words were last hashed in (0: never); at the
 * end of .bss: a .data word would move the RAM code after it */
#ifndef HOST_BUILD
static u32 wc_checked_frame __attribute__((section(".gba_tail_bss")));
#else
static u32 wc_checked_frame;
#endif

#include "hle_wcp.inc"

/* the words, once a frame (the caller tests the frame): in the flash with
 * the cold code, so the RAM code behind the late text keeps its place */
#ifndef HOST_BUILD
__attribute__((noinline, section(".gba_cold_text")))
#else
__attribute__((noinline))
#endif
static bool wc_words_ok(const u8 *code)
{
  u32 h = 2166136261u;
  for (u32 r = 0; r < sizeof wc_ranges / sizeof wc_ranges[0]; r++)
    for (u32 a = wc_ranges[r][0]; a < wc_ranges[r][1]; a += 4)
      h = (h ^ readaddress32(code, a & 0x7FFF)) * 16777619u;
  if (h != WC_FNV)
    return false;
  wc_checked_frame = frame_counter + 1;
  return true;
}

/* GCC: built -Os (Makefile): it runs from ITCM. */
extern "C" uint64_t hle_wcp_run(uint32_t pc, int32_t cycles, uint32_t nzcv)
{
  s32 cyc = cycles;   /* gpSP's s32 (int; int32_t is long on the device) */
  const u8 *code = memory_map_read[WC_LO >> 15];
  if (!code || (wc_checked_frame != frame_counter + 1 && !wc_words_ok(code)))
    return (u32)cyc;

  u32 r0 = reg[0], r1 = reg[1], r2 = reg[2], r3 = reg[3], r4 = reg[4];
  u32 r5 = reg[5], r6 = reg[6], r7 = reg[7], r8 = reg[8], r9 = reg[9];
  u32 r10 = reg[10], r11 = reg[11], r12 = reg[12], r13 = reg[13], r14 = reg[14];
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const s32 F = ws_cyc_seq[3][1];      /* each instruction's fetch (IWRAM) */
  const s32 BT = ws_cyc_nseq[3][1];    /* a taken branch (to IWRAM) */
  const s32 cyc0 = cyc;                /* every instruction charges: ran <=> cyc < cyc0 */
  u32 npc;
  /* a checkpoint's bound takes each access's wait states from the register
   * addressing it there (hle_wcp.inc): a byte the nonsequential 8-bit, a
   * word the slower of the 32-bit ones; addresses past the regions cost 0 */
#define WC_B8(a)  ((a) < 0x10000000 ? (s32)ws_cyc_nseq[(a) >> 24][0] : 0)
#define WC_W32(a) ((a) < 0x10000000 ? (ws_cyc_nseq[(a) >> 24][1] > ws_cyc_seq[(a) >> 24][1] ? \
                   (s32)ws_cyc_nseq[(a) >> 24][1] : (s32)ws_cyc_seq[(a) >> 24][1]) : 0)
#define WC_MAX(x, y) ((x) > (y) ? (x) : (y))
#define WC_MAX3(x, y, z) WC_MAX(WC_MAX(x, y), z)
  /* the edge walkers' variables: work RAM, the slower of its two regions */
  const s32 wc_wram32 = WC_MAX(WC_W32(0x02000000u), WC_W32(0x03000000u));
  /* the geometry's data: work RAM or the cartridge ROM, the slowest */
  const s32 wc_any8 = WC_MAX3(WC_B8(0x02000000u), WC_B8(0x03000000u),
                              WC_MAX3(WC_B8(0x08000000u), WC_B8(0x0A000000u), WC_B8(0x0C000000u)));
  const s32 wc_any32 = WC_MAX3(wc_wram32, WC_W32(0x08000000u),
                               WC_MAX(WC_W32(0x0A000000u), WC_W32(0x0C000000u)));

#define WC_LD(fn, a) ({ u64 _r = fn((a), cyc); cyc = (s32)(u32)(_r >> 32); (u32)_r; })
#define LDB(a)   WC_LD(wc_ldb, a)
#define LDW(a)   WC_LD(wc_ldw, a)
#define LDH(a)   WC_LD(wc_ldh, a)
#define LDSH(a)  WC_LD(wc_ldsh, a)
#define BLK_VRAM(a, bytes) (((a) >> 24) == 6 && (((a) + (bytes) - 4) >> 24) == 6)
/* write_memory32 per word: the write (folded), one side effect each, and
 * the general path's one more a transfer, counted at the call */
#define VRAM_STM(a, v, n) wc_vram_stm((a), (v), (n))
#define STW(a, val) { cyc -= ws_cyc_nseq[(a) >> 24][1]; \
                      *(u32 *)(memory_map_read[(a) >> 15] + ((a) & 0x7FFF)) = (val); \
                      gba_idle_sidefx++; }
/* work RAM: the stores cpu_store8/16 and the block transfers do there */
#define ST_OK(a) ((((a) >> 24) - 2) <= 1)
#define BLK_OK(a, bytes) ((((a) >> 24) - 2) <= 1 && ((a) & 0x7FFF) <= 0x8000u - (bytes))
#define STB(a, val) { cyc -= ws_cyc_nseq[(a) >> 24][0]; \
                      *(u8 *)(memory_map_read[(a) >> 15] + ((a) & 0x7FFF)) = (u8)(val); \
                      gba_idle_sidefx++; }
#define STH(a, val) { cyc -= ws_cyc_nseq[(a) >> 24][0]; \
                      *(u16 *)(memory_map_read[(a) >> 15] + ((a) & 0x7FFF)) = (u16)(val); \
                      gba_idle_sidefx++; }
#define EXIT_DONE(nx)  { npc = (nx); goto done; }
#define EXIT(nx)       { npc = (nx); goto done; }
#define WC_CODE
#include "hle_wcp.inc"
#undef WC_CODE
#undef WC_B8
#undef WC_W32
#undef WC_MAX
#undef WC_MAX3
#undef WC_LD
#undef LDB
#undef LDW
#undef LDH
#undef LDSH
#undef BLK_VRAM
#undef VRAM_STM
#undef STW
#undef ST_OK
#undef BLK_OK
#undef STB
#undef STH
#undef EXIT_DONE
#undef EXIT

done:
  if (cyc == cyc0)                     /* stopped before its first instruction */
    return (u32)cyc;
  reg[0] = r0; reg[1] = r1; reg[2] = r2; reg[3] = r3; reg[4] = r4;
  reg[5] = r5; reg[6] = r6; reg[7] = r7; reg[8] = r8; reg[9] = r9;
  reg[10] = r10; reg[11] = r11; reg[12] = r12; reg[13] = r13; reg[14] = r14;
  reg[REG_PC] = npc;
  return (u32)cyc | ((u64)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}
