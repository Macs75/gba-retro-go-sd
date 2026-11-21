/* hle_mgolf.cc -- Mario Golf: Advance Tour's sound mixing loops, run natively.
 *
 * The contract is gba_hle.h's: the interpreter's exact memory accesses,
 * cycle charges and stopping points, so the machine state is the same as
 * the interpreter's. The interpreter calls it when a jump lands in
 * 0x03000A28-0x03000EE8; it runs whole passes of the two loops there
 * (tools/gba_hle_mgolf_gen.py -> hle_mgolf.inc) from their heads, and
 * declines every other landing in that range.
 *
 * Before every 16th instruction of a pass a checkpoint: the next 16 run only
 * when the cycles left exceed the most they can cost (their fetches, the
 * back branch, each memory access at its buffer's wait state); then no stop
 * (cycles <= 0 after an instruction) can fall among them, and they need no
 * test each. Otherwise it returns there, at an instruction boundary, and the
 * interpreter runs the rest of the pass itself, stopping where it must, and
 * lands at the head again. The machine state at each return is the
 * interpreter's.
 *
 * The game rewrites these loops as it plays (the interpolating loop's 8
 * sample slots take one of three templates, the output stage's back branch
 * one of two targets, 0x03000E3C or 0x03000E34), so each entry checks which
 * form is in IWRAM (the template the game noted at 0x03000B7C against all 8
 * slots; the back branch's word), and each form's words are hashed once a
 * frame. On the device this unit is an overlay copied to ITCM only for this
 * game (gba_hle_select).
 */
extern "C" {
#include "../gpsp/common.h"   /* gpSP's: another common.h is earlier in the path */
}
#include "gba_hle.h"

extern "C" unsigned int gba_idle_sidefx;

/* the byte load (fast_read_memory(8) of cpu.cc): BIOS, unmapped through
 * read_memory8; the caller charges the cycles */
static __attribute__((noinline)) u32 mg_rd8(u32 a)
{
  u8 *map;
  if ((a >> 24) == 0 || (a & 0xF0000000) || !(map = memory_map_read[a >> 15]))
    return (u8)read_memory8(a);
  return readaddress8(map, a & 0x7FFF);
}

/* which forms' words were hashed this frame (bits 0-2 the templates, 3 the
 * output stage) and in which frame + 1; at the end of .bss: a .data word
 * would move the RAM code after it */
#ifndef HOST_BUILD
static u32 mg_checked_frame __attribute__((section(".gba_tail_bss")));
static u32 mg_checked_mask __attribute__((section(".gba_tail_bss")));
#else
static u32 mg_checked_frame, mg_checked_mask;
#endif

#include "hle_mgolf.inc"

/* The entry checks run once per entry, not per sample: RAM code behind
 * everything else (ld: .gba_late_text), out of the ITCM window, which holds
 * the loops. */
#ifndef HOST_BUILD
#define MG_CHECK __attribute__((noinline, section(".gba_late_text.mgolf")))
#else
#define MG_CHECK __attribute__((noinline))
#endif

static inline u32 mg_word(const u8 *code, u32 a)
{
  return readaddress32(code, a & 0x7FFF);
}

static MG_CHECK u32 mg_fnv(const u8 *code, u32 lo, u32 hi)
{
  u32 h = 2166136261u;
  for (u32 a = lo; a < hi; a += 4)
    h = (h ^ mg_word(code, a)) * 16777619u;
  return h;
}

/* which template the interpolating loop holds now: 0-2, or -1 */
static MG_CHECK int mg_l1_variant(const u8 *code)
{
  u32 t = mg_word(code, 0x03000B7Cu);
  int vv = t == 0x03000B80u ? 0 : t == 0x03000B98u ? 1 : t == 0x03000BB0u ? 2 : -1;
  if (vv < 0)
    return -1;
  for (u32 i = 0; i < 4; i++)
    for (u32 half = 0; half <= 0x98; half += 0x98) {
      u32 b = 0x03000A30u + 0x24 * i + half;
      if (mg_word(code, b) != mg_tpl[vv][0] || mg_word(code, b + 4) != mg_tpl[vv][1] ||
          mg_word(code, b + 0xC) != mg_tpl[vv][2] || mg_word(code, b + 0x10) != mg_tpl[vv][3] ||
          mg_word(code, b + 0x14) != mg_tpl[vv][4] || mg_word(code, b + 0x18) != mg_tpl[vv][5])
        return -1;
    }
  return vv;
}

/* a form's words, once a frame */
static MG_CHECK bool mg_form_ok(const u8 *code, u32 bit, u32 lo, u32 hi, u32 want)
{
  if (mg_checked_frame != frame_counter + 1) {
    mg_checked_frame = frame_counter + 1;
    mg_checked_mask = 0;
  }
  if (mg_checked_mask & bit)
    return true;
  if (mg_fnv(code, lo, hi) != want)
    return false;
  mg_checked_mask |= bit;
  return true;
}

/* GCC: built -O2 (Makefile), as the other routines: it runs from ITCM. */
extern "C" uint64_t hle_mgolf_run(uint32_t pc, int32_t cycles, uint32_t nzcv)
{
  s32 cyc = cycles;   /* gpSP's s32 (int; int32_t is long on the device) */
  const u8 *code = memory_map_read[0x03000A28u >> 15];
  if (!code)
    return (u32)cyc;
  int variant = -1;
  bool l2_back_b = false;
  if (pc == MG_L1_LO) {
    variant = mg_l1_variant(code);
    if (variant < 0 || !(MG_L1_NATIVE_MASK & (1u << variant)) ||
        !mg_form_ok(code, 1u << variant, MG_L1_LO, 0x03000B60u, mg_l1_fnv[variant]))
      return (u32)cyc;
  } else if (pc == 0x03000E3Cu || pc == 0x03000E34u) {
    u32 back = mg_word(code, 0x03000EE4u);
    if (back != MG_L2_BACK_A && back != MG_L2_BACK_B)
      return (u32)cyc;
    l2_back_b = back == MG_L2_BACK_B;
    if (pc != (l2_back_b ? 0x03000E34u : 0x03000E3Cu) ||
        !mg_form_ok(code, 8u, MG_L2_LO, 0x03000EE4u, MG_L2_FNV))
      return (u32)cyc;
  } else
    return (u32)cyc;                   /* the range's other code: interpreted */
#ifdef HOST_BUILD
  {   /* GBA_MG_ONLY=1|2: only that loop natively (debugging) */
    static int only = -1;
    if (only < 0) { const char *e = getenv("GBA_MG_ONLY"); only = e ? atoi(e) : 0; }
    if ((only == 1 && variant < 0) || (only == 2 && variant >= 0))
      return (u32)cyc;
  }
#endif

  u32 r0 = reg[0], r1 = reg[1], r2 = reg[2], r3 = reg[3], r4 = reg[4];
  u32 r5 = reg[5], r6 = reg[6], r7 = reg[7], r8 = reg[8], r9 = reg[9];
  u32 r10 = reg[10], r11 = reg[11], r12 = reg[12], r13 = reg[13], r14 = reg[14];
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const s32 F = ws_cyc_seq[3][1];      /* each instruction's fetch (IWRAM) */
  const s32 BT = ws_cyc_nseq[3][1];    /* a taken branch (to IWRAM) */
  const s32 cyc0 = cyc;                /* every instruction charges: ran <=> cyc < cyc0 */
  u32 npc;
  /* a pass's byte accesses and block words go through one buffer each,
   * whose region (and so wait states) the pass does not leave: the
   * interpolating loop's samples (r3: the stack's copy in IWRAM, or the ROM)
   * and mix buffer (r5); the output stage's buffers (r5, r10) and mix
   * buffer (r0). The checkpoints take the slower of a pass's byte regions. */
#define MG_B8(a)  ((a) < 0x10000000 ? ws_cyc_nseq[(a) >> 24][0] : 0)
#define MG_W32(a) ((a) < 0x10000000 ? ws_cyc_seq[(a) >> 24][1] : 0)
  s32 b8, w32;
  if (variant >= 0) {
    b8 = MG_B8(r3);
    w32 = MG_W32(r5);
  } else {
    b8 = MG_B8(r5) > MG_B8(r10) ? MG_B8(r5) : MG_B8(r10);
    w32 = MG_W32(r0);
  }
#undef MG_B8
#undef MG_W32

/* a block transfer the native code does: all its words in one work-RAM
 * page (the work RAM is mapped, page by page, in memory_map_read) */
#define BLK_OK(a, bytes) ((((a) >> 24) - 2) <= 1 && ((a) & 0x7FFF) <= 0x8000u - (bytes))
#define LDSB(a)  ({ u32 _b = (a); if (_b < 0x10000000) cyc -= ws_cyc_nseq[_b >> 24][0]; \
                    (u32)(s32)(s8)mg_rd8(_b); })
/* work RAM: the stores cpu_store8 / store_aligned32 do there themselves */
#define ST_OK(a) ((((a) >> 24) - 2) <= 1)
#define STB(a, val) { cyc -= ws_cyc_nseq[(a) >> 24][0]; \
                      *(u8 *)(memory_map_read[(a) >> 15] + ((a) & 0x7FFF)) = (u8)(val); \
                      gba_idle_sidefx++; }
#define EXIT_DONE(nx)  { npc = (nx); goto done; }
#define EXIT(nx)       { npc = (nx); goto done; }
#if defined(HOST_BUILD)
  /* GBA_MG_TRACE (debugging): the output stage's head, as the interpreter's
   * landing there would print it */
  static int mg_tr = -1;
  if (mg_tr < 0) mg_tr = getenv("GBA_MG_TRACE") != NULL;
#define MG_TRACE_PT(p) if (mg_tr) printf("MGT f=%u pc=%08x cyc=%d nzcv=%x r0=%08x r2=%08x r3=%08x r4=%08x r5=%08x r9=%08x r10=%08x r14=%08x\n", \
    (unsigned)frame_counter, (p), (int)cyc, (n << 3) | (z << 2) | (c << 1) | v, r0, r2, r3, r4, r5, r9, r10, r14)
#else
#define MG_TRACE_PT(p)
#endif
  if (variant < 0)
    goto l2_entry;
  if (variant == 0)
    goto l1_v0;
  goto l1_v1;
l2_entry:
  if (l2_back_b)
    goto l2_b;
  goto l2_a;
#define MG_CODE
#include "hle_mgolf.inc"
#undef MG_CODE
#undef BLK_OK
#undef LDSB
#undef ST_OK
#undef STB
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
