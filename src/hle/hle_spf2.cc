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

/* ---------------------------------------------------------------------
 * The line interrupt that does nothing.
 *
 * The game takes an HBlank interrupt on every line (183 a frame) for a
 * sprite multiplexer that works on 3-6 of them; on the others its handler
 * compares the line with three thresholds and returns. Each of those ran
 * ~70 instructions: the BIOS entry, the game's ARM dispatcher (0x03002520,
 * IWRAM: acknowledge IF, IE = 0x20C0, system mode with interrupts on, call
 * the handler, back to IRQ mode, restore IE and SPSR) and the Thumb handler
 * (0x0802EBCC, ROM) -- 25% of the match's instructions. When the dispatcher
 * is entered (the BIOS's jump) and every check below holds, the dispatcher
 * and the empty handler run here at once, as the interpreter would run
 * them: the same stack writes, IF/IE writes (write_memory16), mode switches
 * (set_cpu_mode: the banked registers end as the interpreter leaves them),
 * registers, flags and cycles. Anything else -- another interrupt, a
 * nested one about to fire at the msr, the handler's working lines, too
 * few cycles left in the slice for the whole chain -- declines: the
 * interpreter runs it.
 * --------------------------------------------------------------------- */
#define SPF_DISP_FNV 0xdd76416du   /* 0x03002520-0x0300263C, 72 words */
#define SPF_HAND_FNV 0x38d07a6fu   /* ROM 0x0802EBCC-0x0802EFB6 */
#ifndef HOST_BUILD
static u32 spf_disp_checked_frame __attribute__((section(".gba_tail_bss")));
static u32 spf_hand_state __attribute__((section(".gba_tail_bss")));  /* 0 unchecked, 1 ok, 2 no */
#else
static u32 spf_disp_checked_frame, spf_hand_state;
#endif

static inline u32 spf_wram32(u32 a) { return readaddress32(memory_map_read[a >> 15], a & 0x7FFF); }
static inline void spf_wram_st32(u32 a, u32 v)
{
  *(u32 *)(memory_map_read[a >> 15] + (a & 0x7FFF)) = v;
}
/* stores of a block transfer, as exec_block_mem_fast */
static inline void spf_push(u32 a, const u32 *v, u32 n)
{
  for (u32 i = 0; i < n; i++)
    spf_wram_st32(a + 4 * i, v[i]);
  gba_idle_sidefx += n;
}
static inline bool spf_wram(u32 a) { return ((a >> 24) - 2) <= 1 && memory_map_read[a >> 15]; }

/* The code checks that run once a frame (once for the ROM): in the flash,
 * not in the ITCM window (the overlay must leave the firmware 1 KB).
 * which: 0 the mixing loops, 1 the dispatcher, 2 the ROM handler. */
#ifndef HOST_BUILD
__attribute__((noinline, section(".gba_cold_text.spf2")))
#else
__attribute__((noinline))
#endif
static bool spf_code_ok(u32 which)
{
  u8 *iw = memory_map_read[0x03000000u >> 15];
  u32 h = 2166136261u;
  if (which == 2) {
    for (u32 a = 0x0802EBCCu; a < 0x0802EFB8u; a += 2)
      h = (h ^ read_memory16(a)) * 16777619u;
    return h == SPF_HAND_FNV;
  }
  if (which == 1) {
    for (u32 a = 0x03002520u; a <= 0x0300263Cu; a += 4)
      h = (h ^ readaddress32(iw, a & 0x7FFF)) * 16777619u;
    return h == SPF_DISP_FNV;
  }
  for (u32 a = 0x03000038u; a <= 0x03000060u; a += 4)
    h = (h ^ readaddress32(iw, a & 0x7FFF)) * 16777619u;
  for (u32 a = 0x03000190u; a <= 0x03000268u; a += 4)
    h = (h ^ readaddress32(iw, a & 0x7FFF)) * 16777619u;
  return h == SPF_FNV;
}

/* fast_read_memory(16); the step path's only (in RAM with it) */
#ifndef HOST_BUILD
__attribute__((section(".gba_ram_text.spf2")))
#endif
static __attribute__((noinline)) u32 spf_ld16(u32 a, s32 &cyc)
{
  u8 *map;
  if (a < 0x10000000)
    cyc -= ws_cyc_nseq[a >> 24][0];
  if ((a >> 24) == 0 || (a & 0xF0000001) || !(map = memory_map_read[a >> 15]))
    return read_memory16(a);
  return readaddress16(map, a & 0x7FFF);
}

/* the ROM handler's literals (its code is hashed: these are fixed) */
#define SPF_LIT_VCOUNT 0x0802EC48u
#define SPF_LIT_THR0   0x0802EC4Cu
#define SPF_LIT_THR1   0x0802ED3Cu
#define SPF_LIT_THR2   0x0802EED8u

/* The whole chain at once, when it fits in the slice (most line
 * interrupts): in ITCM with the overlay. */
/* In RAM_EMU with the step path (.gba_ram_text): the ITCM window has no
 * room left beside the mixing loops. Once per interrupt, not per sample. */
#ifndef HOST_BUILD
__attribute__((section(".gba_ram_text.spf2")))
#endif
__attribute__((noinline))
static uint64_t spf_irq_fast(s32 cyc, u32 nzcv)
{
  u8 *iw = memory_map_read[0x03000000u >> 15];
  if (!iw || reg[CPU_MODE] != MODE_IRQ || !(reg[REG_CPSR] & 0x80))
    return (u32)cyc;
#define SPF_OP(a) readaddress32(iw, (a) & 0x7FFF)
  if (SPF_OP(0x03002600u) != 0xe129f003u || SPF_OP(0x03002618u) != 0xe12fff10u)
    return (u32)cyc;
  if (spf_disp_checked_frame != frame_counter + 1) {
    if (!spf_code_ok(1))
      return (u32)cyc;
    spf_disp_checked_frame = frame_counter + 1;
  }
#undef SPF_OP
  if (!spf_hand_state)          /* the ROM does not change: once */
    spf_hand_state = spf_code_ok(2) ? 1 : 2;
  if (spf_hand_state != 1)
    return (u32)cyc;
  /* the idle skip (or the cheat hook) parked inside the chain: it would
   * act at an instruction run here */
  if (idle_loop_target_pc - 0x03002520u < 0x120u || idle_loop_target_pc - 0x0802EBCCu < 0x3ECu)
    return (u32)cyc;

  /* the dispatcher's decisions, read first (no side effects) */
  u32 ieif = read_memory32(0x04000200u);              /* ldr r2, [r3]: IE | IF << 16 */
  u32 act = ieif & (ieif >> 16);                      /* and r1, r2, r2, lsr #16 */
  if ((act & 0xC0) || !(act & 2))
    return (u32)cyc;                                  /* not (only) HBlank */
  u32 if_after = (ieif >> 16) & ~2u;                  /* IF once acknowledged */
  if ((0x20C0u & if_after) && read_memory16(0x04000208u))
    return (u32)cyc;                                  /* nested IRQ at the msr */
  u32 sp_irq = reg[REG_SP], lr_irq = reg[REG_LR];
  u32 sp_sys = REG_MODE(MODE_SYSTEM)[5];
  if ((lr_irq & 1) || !spf_wram(sp_irq - 16) || !spf_wram(sp_irq - 4) ||
      !spf_wram(sp_sys - 36) || !spf_wram(sp_sys - 4))
    return (u32)cyc;
  u32 table = spf_wram32(0x03002640u);
  if (!spf_wram(table + 4) || (table & 3) || spf_wram32(table + 4) != 0x0802EBCDu)
    return (u32)cyc;
  /* the handler's empty path: the line past 0x84, or below all three */
  u32 vcount = read_memory16(0x04000006u);
  u32 thr[3] = { 0, 0, 0 };
  bool lng = vcount <= 0x84u;
  if (lng) {
    thr[0] = spf_wram32(0x030070B4u);
    thr[1] = spf_wram32(0x030070B8u);
    thr[2] = spf_wram32(0x030070BCu);
    if (vcount >= thr[0] || vcount >= thr[1] || vcount >= thr[2])
      return (u32)cyc;                                /* a working line */
  }

  /* the whole chain's cycles, as the interpreter charges them */
  const s32 F = ws_cyc_seq[3][1];                     /* ARM fetch, IWRAM */
  const s32 T = ws_cyc_seq[8][0];                     /* Thumb fetch, ROM */
  const s32 TB = ws_cyc_nseq[8][0];                   /* Thumb branch, taken or not */
  const s32 SI = ws_cyc_seq[sp_irq >> 24][1], SS = ws_cyc_seq[sp_sys >> 24][1];
  s32 cost = 28 * F + ws_cyc_nseq[4][1] + 4 * SI + ws_cyc_nseq[3][1] + 2 * ws_cyc_nseq[4][0] +
             ws_cyc_nseq[3][1] + ws_cyc_nseq[table >> 24][1] + SS;       /* to the bx r0 */
  cost += (lng ? 31 : 17) * T + 16 * SS + ws_cyc_nseq[8][1] + ws_cyc_nseq[4][0] +
          (lng ? 3 * ws_cyc_nseq[8][1] + 3 * ws_cyc_nseq[3][1] + 7 * TB : 2 * TB);
  cost += 8 * F + SS + 4 * SI + ws_cyc_nseq[4][0] +
          ws_cyc_nseq[lr_irq >> 24][1] + ws_cyc_seq[(lr_irq >> 24) & 0xF][1];
  if (cyc - cost <= 0)
    return (u32)cyc;                                  /* it would stop inside */

  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const u32 MSR_MASK = 0xF00000EFu;                   /* cpsr_masks/spsr_masks, fields f+c, privileged */

  /* the dispatcher, IRQ mode */
  reg[3] = 0x04000200u;
  reg[2] = ieif;
  reg[1] = ieif & 0xFFFF;
  reg[0] = REG_SPSR(MODE_IRQ);                        /* mrs r0, spsr */
  { u32 w[4] = { reg[0], reg[1], reg[3], lr_irq };    /* push {r0, r1, r3, lr} */
    spf_push(sp_irq - 16, w, 4); reg[REG_SP] = sp_irq - 16; }
  reg[1] = act;
  reg[2] = 4;
  reg[0] = 2;                                         /* ands r0, r1, #2: bne taken */
  n = 0; z = 0;
  write_memory16(0x04000202u, 2);                     /* strh r0, [r3, #2]: IF ack */
  reg[1] = 0x20C0;
  write_memory16(0x04000200u, 0x20C0);                /* strh r1, [r3]: IE (I set: no IRQ) */
  u32 cpsr = (reg[REG_CPSR] & 0x0FFFFFFFu) | n << 31 | z << 30 | c << 29 | v << 28;
  reg[3] = (cpsr & ~0xDFu) | 0x1F;                    /* system mode, interrupts on */
  reg[REG_CPSR] = (reg[3] & MSR_MASK) | (cpsr & ~MSR_MASK);
  set_cpu_mode(MODE_SYSTEM);
  reg[1] = table + 4;                                 /* r2 = 4: HBlank's slot */
  reg[0] = 0x0802EBCDu;
  reg[REG_SP] -= 4;                                   /* stmdb sp!, {lr} */
  spf_push(reg[REG_SP], &reg[REG_LR], 1);
  reg[REG_LR] = 0x0300261Cu;                          /* add lr, pc, #0 */

  /* the handler, Thumb */
  { u32 w[5] = { reg[4], reg[5], reg[6], reg[7], reg[REG_LR] };   /* push {r4-r7, lr} */
    spf_push(reg[REG_SP] - 20, w, 5); reg[REG_SP] -= 20; }
  reg[7] = reg[10]; reg[6] = reg[9]; reg[5] = reg[8];
  { u32 w[3] = { reg[5], reg[6], reg[7] };            /* push {r5, r6, r7} */
    spf_push(reg[REG_SP] - 12, w, 3); reg[REG_SP] -= 12; }
  reg[0] = vcount;
  reg[9] = vcount;
  u32 cx = 0x84u;                                     /* cmp r0, #0x84 */
  if (lng) {
    reg[1] = 0x030070B4u;
    reg[2] = 0x030070B8u;
    reg[1] = 0x030070BCu;
    reg[0] = thr[2];
    cx = thr[2];                                      /* cmp sb, r0 (the last) */
  }
  { u32 r = vcount - cx; n = r >> 31; z = r == 0; c = vcount >= cx; v = ((vcount ^ cx) & (vcount ^ r)) >> 31; }
  { u32 sp = reg[REG_SP];                             /* pop {r3, r4, r5}; mov r8-sl */
    reg[3] = spf_wram32(sp); reg[4] = spf_wram32(sp + 4); reg[5] = spf_wram32(sp + 8);
    reg[8] = reg[3]; reg[9] = reg[4]; reg[10] = reg[5];
    sp += 12;
    for (u32 i = 0; i < 4; i++)                       /* pop {r4-r7} */
      reg[4 + i] = spf_wram32(sp + 4 * i);
    sp += 16;
    reg[0] = spf_wram32(sp);                          /* pop {r0}; bx r0 */
    reg[REG_SP] = sp + 4; }

  /* back in the dispatcher, ARM, system mode */
  reg[REG_LR] = spf_wram32(reg[REG_SP]);              /* ldm sp!, {lr} */
  reg[REG_SP] += 4;
  cpsr = (reg[REG_CPSR] & 0x0FFFFFDFu) | n << 31 | z << 30 | c << 29 | v << 28;
  reg[3] = (cpsr & ~0xDFu) | 0x92;                    /* IRQ mode, interrupts off */
  reg[REG_CPSR] = (reg[3] & MSR_MASK) | (cpsr & ~MSR_MASK);
  set_cpu_mode(MODE_IRQ);
  { u32 sp = reg[REG_SP];                             /* pop {r0, r1, r3, lr} */
    reg[0] = spf_wram32(sp); reg[1] = spf_wram32(sp + 4);
    reg[3] = spf_wram32(sp + 8); reg[REG_LR] = spf_wram32(sp + 12);
    reg[REG_SP] = sp + 16; }
  write_memory16(reg[3], (u16)reg[1]);                /* strh r1, [r3]: IE back */
  REG_SPSR(MODE_IRQ) = (reg[0] & MSR_MASK) | (REG_SPSR(MODE_IRQ) & ~MSR_MASK);
  reg[REG_PC] = reg[REG_LR];                          /* bx lr: to the BIOS */
  cyc -= cost;
  return (u32)cyc | ((u64)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}

/* The chain from whichever of its ARM instructions the interpreter is at:
 * the dispatcher's entry (the BIOS's jump), or where a previous call ran
 * out of cycles (the next slice starts there and tests the special PCs).
 * One instruction at a time, the interpreter's charges, a stop at the
 * first instruction that leaves cycles <= 0 -- or, cycles left, before an
 * instruction this does not run (another interrupt's path, a nested IRQ at
 * the msr, a non-empty handler line, an I/O write that is not IF or IE):
 * the interpreter runs it. The Thumb handler runs from its start, checked
 * at the bx to it; a stop inside it sets the T bit, the interpreter runs
 * the rest, and its bx back to ARM (0x0300261C) enters here again. */
/* GCC: optimised for size and in RAM_EMU (ld/gba_core.ld .gba_ram_text),
 * not in the overlay: step by step it is 3.7 KB even at -Os, and the ITCM
 * window has ~5.5 KB for the whole overlay. spf_irq_fast, in ITCM, takes the
 * chains that fit in their slice (77%); this one the rest. */
#ifndef HOST_BUILD
__attribute__((section(".gba_ram_text.spf2")))
#endif
__attribute__((noinline, optimize("Os")))
static uint64_t spf_irq_step(u32 pc, s32 cyc, u32 nzcv)
{
  u8 *iw = memory_map_read[0x03000000u >> 15];
  if (!iw)
    return (u32)cyc;
#define SPF_OP(a) readaddress32(iw, (a) & 0x7FFF)
  if (SPF_OP(0x03002600u) != 0xe129f003u || SPF_OP(0x03002618u) != 0xe12fff10u)
    return (u32)cyc;
  if (spf_disp_checked_frame != frame_counter + 1) {
    if (!spf_code_ok(1))
      return (u32)cyc;
    spf_disp_checked_frame = frame_counter + 1;
  }
#undef SPF_OP
  if (!spf_hand_state)          /* the ROM does not change: once */
    spf_hand_state = spf_code_ok(2) ? 1 : 2;
  if (spf_hand_state != 1)
    return (u32)cyc;
  /* the idle skip (or the cheat hook) parked inside the chain: it would
   * act at an instruction run here */
  if (idle_loop_target_pc - 0x03002520u < 0x120u || idle_loop_target_pc - 0x0802EBCCu < 0x3ECu)
    return (u32)cyc;

  const s32 cyc0 = cyc;
  u32 n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
  const s32 F = ws_cyc_seq[3][1];                     /* ARM fetch, IWRAM */
  const s32 BA = ws_cyc_nseq[3][1];                   /* a taken ARM B, to IWRAM */
  const s32 T = ws_cyc_seq[8][0];                     /* Thumb fetch, ROM */
  const s32 TB = ws_cyc_nseq[8][0];                   /* Thumb B (taken or not), to ROM */
  const u32 MSR_MASK = 0xF00000EFu;                   /* fields f+c, privileged (cpsr/spsr_masks) */

#define STOP(nx)    { reg[REG_PC] = (nx); goto out; }
#define AST(nx)     { cyc -= F; if (cyc <= 0) STOP(nx); }
#define ABR(t, lab) { cyc -= BA + F; if (cyc <= 0) STOP(t); goto lab; }
#define TST(nx, x)  { cyc -= (x) + T; if (cyc <= 0) { reg[REG_PC] = (nx); reg[REG_CPSR] |= 0x20; goto out; } }
#define CPSR_NOW()  ((reg[REG_CPSR] & 0x0FFFFFFFu) | n << 31 | z << 30 | c << 29 | v << 28)
#define SUBF(x, y)  { u32 _x = (x), _y = (y), _r = _x - _y; n = _r >> 31; z = _r == 0; \
                      c = _x >= _y; v = ((_x ^ _y) & (_x ^ _r)) >> 31; }
#define LOGF(r)     { u32 _r = (r); n = _r >> 31; z = _r == 0; }
/* a block transfer's stack words: work RAM (no alert, the fast path's cost) */
#define STACK_OK(a, k) (spf_wram(a) && spf_wram((a) + 4 * (k) - 4))

  switch (pc) {
  case 0x03002520u: goto A20;  case 0x03002524u: goto A24;  case 0x03002528u: goto A28;
  case 0x0300252Cu: goto A2C;  case 0x03002530u: goto A30;  case 0x03002534u: goto A34;
  case 0x03002538u: goto A38;  case 0x0300253Cu: goto A3C;  case 0x03002540u: goto A40;
  case 0x03002544u: goto A44;  case 0x03002548u: goto A48;  case 0x0300254Cu: goto A4C;
  case 0x03002550u: goto A50;  case 0x03002554u: goto A54;  case 0x03002558u: goto A58;
  case 0x0300255Cu: goto A5C;
  case 0x030025E8u: goto AE8;  case 0x030025ECu: goto AEC;  case 0x030025F0u: goto AF0;
  case 0x030025F4u: goto AF4;  case 0x030025F8u: goto AF8;  case 0x030025FCu: goto AFC;
  case 0x03002600u: goto B00;  case 0x03002604u: goto B04;  case 0x03002608u: goto B08;
  case 0x0300260Cu: goto B0C;  case 0x03002610u: goto B10;  case 0x03002614u: goto B14;
  case 0x03002618u: goto B18;
  case 0x0300261Cu: goto C1C;  case 0x03002620u: goto C20;  case 0x03002624u: goto C24;
  case 0x03002628u: goto C28;  case 0x0300262Cu: goto C2C;  case 0x03002630u: goto C30;
  case 0x03002634u: goto C34;  case 0x03002638u: goto C38;  case 0x0300263Cu: goto C3C;
  default: return (u32)cyc;
  }

  /* the dispatcher: which interrupt, IRQ mode */
A20: reg[3] = 0x04000000u; AST(0x03002524u)                        /* mov r3, #0x4000000 */
A24: reg[3] += 0x200; AST(0x03002528u)                             /* add r3, r3, #0x200 */
A28: reg[2] = spf_ld32(reg[3], cyc); AST(0x0300252Cu)              /* ldr r2, [r3] */
A2C: reg[1] = reg[2] << 16; AST(0x03002530u)                       /* lsl r1, r2, #16 */
A30: reg[1] = reg[1] >> 16; AST(0x03002534u)                       /* lsr r1, r1, #16 */
A34: reg[0] = REG_SPSR(reg[CPU_MODE]); AST(0x03002538u)           /* mrs r0, spsr */
A38: { u32 a = reg[REG_SP] - 16;                                   /* push {r0, r1, r3, lr} */
       if (!STACK_OK(a, 4)) STOP(0x03002538u);
       u32 w[4] = { reg[0], reg[1], reg[3], reg[REG_LR] };
       spf_push(a, w, 4); reg[REG_SP] = a; cyc -= 4 * ws_cyc_seq[a >> 24][1]; }
     AST(0x0300253Cu)
A3C: reg[1] = reg[2] & (reg[2] >> 16); AST(0x03002540u)            /* and r1, r2, r2, lsr #16 */
A40: reg[2] = 0; AST(0x03002544u)                                  /* mov r2, #0 */
A44: reg[0] = reg[1] & 0x80; LOGF(reg[0]); AST(0x03002548u)        /* ands r0, r1, #0x80 */
A48: if (!z) ABR(0x030025E8u, AE8) AST(0x0300254Cu)               /* bne */
A4C: reg[0] = reg[1] & 0x40; LOGF(reg[0]); AST(0x03002550u)        /* ands r0, r1, #0x40 */
A50: if (!z) ABR(0x030025E8u, AE8) AST(0x03002554u)               /* bne */
A54: reg[2] += 4; AST(0x03002558u)                                 /* add r2, r2, #4 */
A58: reg[0] = reg[1] & 2; LOGF(reg[0]); AST(0x0300255Cu)           /* ands r0, r1, #2 */
A5C: if (!z) ABR(0x030025E8u, AE8) AST(0x03002560u)               /* bne */
     STOP(0x03002560u);                                            /* another interrupt: interpreted */

  /* acknowledge it, IE = 0x20C0, system mode with interrupts on, the handler */
AE8: { u32 a = reg[3] + 2;                                          /* strh r0, [r3, #2] (IF) */
       if (a != 0x04000202u) STOP(0x030025E8u);
       cyc -= ws_cyc_nseq[4][0]; write_memory16(a, (u16)reg[0]); }
     AST(0x030025ECu)
AEC: reg[1] = 0x20C0; AST(0x030025F0u)                             /* mov r1, #0x20c0 */
AF0: if (reg[3] != 0x04000200u || !(reg[REG_CPSR] & 0x80))         /* strh r1, [r3] (IE: no IRQ with I set) */
       STOP(0x030025F0u);
     cyc -= ws_cyc_nseq[4][0]; write_memory16(0x04000200u, (u16)reg[1]);
     AST(0x030025F4u)
AF4: reg[3] = CPSR_NOW(); AST(0x030025F8u)                          /* mrs r3, cpsr */
AF8: reg[3] &= ~0xDFu; AST(0x030025FCu)                             /* bic r3, r3, #0xdf */
AFC: reg[3] |= 0x1F; AST(0x03002600u)                               /* orr r3, r3, #0x1f */
B00: { u32 cpsr = CPSR_NOW();                                       /* msr cpsr_fc, r3 */
       u32 nw = (reg[3] & MSR_MASK) | (cpsr & ~MSR_MASK);
       if ((nw & 0x1F) != 0x1F || !PRIVMODE(reg[CPU_MODE]))
         STOP(0x03002600u);
       if (!(nw & 0x80) && read_memory16(0x04000208u) &&
           (read_memory16(0x04000200u) & read_memory16(0x04000202u)))
         STOP(0x03002600u);                                         /* a nested IRQ: interpreted */
       reg[REG_CPSR] = nw;
       n = nw >> 31; z = (nw >> 30) & 1; c = (nw >> 29) & 1; v = (nw >> 28) & 1;
       set_cpu_mode(MODE_SYSTEM); }
     AST(0x03002604u)
B04: reg[1] = spf_ld32(0x03002640u, cyc); AST(0x03002608u)          /* ldr r1, [pc, #0x34] */
B08: reg[1] += reg[2]; AST(0x0300260Cu)                             /* add r1, r1, r2 */
B0C: reg[0] = spf_ld32(reg[1], cyc); AST(0x03002610u)               /* ldr r0, [r1] */
B10: { u32 a = reg[REG_SP] - 4;                                     /* stmdb sp!, {lr} */
       if (!STACK_OK(a, 1)) STOP(0x03002610u);
       spf_push(a, &reg[REG_LR], 1); reg[REG_SP] = a; cyc -= ws_cyc_seq[a >> 24][1]; }
     AST(0x03002614u)
B14: reg[REG_LR] = 0x0300261Cu; AST(0x03002618u)                    /* add lr, pc, #0 */
B18: {                                                              /* bx r0: the handler, if empty */
       if (reg[0] != 0x0802EBCDu || reg[REG_LR] != 0x0300261Cu || !STACK_OK(reg[REG_SP] - 32, 8))
         STOP(0x03002618u);
       u32 vc = read_memory16(0x04000006u);                         /* VCOUNT, as the handler will read it */
       if (vc <= 0x84u) {
         if (vc >= spf_wram32(read_memory32(SPF_LIT_THR0)) || vc >= spf_wram32(read_memory32(SPF_LIT_THR1)) ||
             vc >= spf_wram32(read_memory32(SPF_LIT_THR2)))
           STOP(0x03002618u);                                       /* a working line: interpreted */
       }
     }                                                              /* (no charge: to the Thumb loop) */

  /* the handler, Thumb, ROM 0x0802EBCC (its empty path) */
  { u32 sp = reg[REG_SP] - 20;                                      /* push {r4-r7, lr} */
    u32 w[5] = { reg[4], reg[5], reg[6], reg[7], reg[REG_LR] };
    spf_push(sp, w, 5); reg[REG_SP] = sp; }
  TST(0x0802EBCEu, 5 * ws_cyc_seq[reg[REG_SP] >> 24][1])
  reg[7] = reg[10]; TST(0x0802EBD0u, 0)                              /* mov r7, sl */
  reg[6] = reg[9];  TST(0x0802EBD2u, 0)                              /* mov r6, sb */
  reg[5] = reg[8];  TST(0x0802EBD4u, 0)                              /* mov r5, r8 */
  { u32 sp = reg[REG_SP] - 12;                                      /* push {r5, r6, r7} */
    u32 w[3] = { reg[5], reg[6], reg[7] };
    spf_push(sp, w, 3); reg[REG_SP] = sp; }
  TST(0x0802EBD6u, 3 * ws_cyc_seq[reg[REG_SP] >> 24][1])
  { s32 k = cyc; reg[0] = spf_ld32(SPF_LIT_VCOUNT, k); TST(0x0802EBD8u, cyc - k) }   /* ldr r0, [pc, #0x70] */
  { s32 k = cyc; reg[0] = spf_ld16(reg[0], k); TST(0x0802EBDAu, cyc - k) }           /* ldrh r0, [r0] */
  reg[9] = reg[0]; TST(0x0802EBDCu, 0)                               /* mov sb, r0 */
  SUBF(reg[0], 0x84u); TST(0x0802EBDEu, 0)                           /* cmp r0, #0x84 */
  if (!c || z) {                                                     /* bls: the line <= 0x84 */
    TST(0x0802EBE2u, TB)
    { s32 k = cyc; reg[1] = spf_ld32(SPF_LIT_THR0, k); TST(0x0802EBE4u, cyc - k) }  /* ldr r1, [pc, #0x68] */
    { s32 k = cyc; reg[0] = spf_ld32(reg[1], k); TST(0x0802EBE6u, cyc - k) }        /* ldr r0, [r1] */
    SUBF(reg[9], reg[0]); TST(0x0802EBE8u, 0)                        /* cmp sb, r0 */
    TST(0x0802EBEAu, TB)                                             /* bhs (not taken: checked) */
    TST(0x0802ED1Au, TB)                                             /* b 0x802ed1a */
    { s32 k = cyc; reg[2] = spf_ld32(SPF_LIT_THR1, k); TST(0x0802ED1Cu, cyc - k) }  /* ldr r2, [pc, #0x20] */
    { s32 k = cyc; reg[0] = spf_ld32(reg[2], k); TST(0x0802ED1Eu, cyc - k) }        /* ldr r0, [r2] */
    SUBF(reg[9], reg[0]); TST(0x0802ED20u, 0)                        /* cmp sb, r0 */
    TST(0x0802ED22u, TB)                                             /* bhs */
    TST(0x0802EE76u, TB)                                             /* b 0x802ee76 */
    { s32 k = cyc; reg[1] = spf_ld32(SPF_LIT_THR2, k); TST(0x0802EE78u, cyc - k) }  /* ldr r1, [pc, #0x60] */
    { s32 k = cyc; reg[0] = spf_ld32(reg[1], k); TST(0x0802EE7Au, cyc - k) }        /* ldr r0, [r1] */
    SUBF(reg[9], reg[0]); TST(0x0802EE7Cu, 0)                        /* cmp sb, r0 */
    TST(0x0802EE7Eu, TB)                                             /* bhs */
    TST(0x0802EFAAu, TB)                                             /* b 0x802efaa */
  } else {
    TST(0x0802EBE0u, TB)                                             /* bls (not taken) */
    TST(0x0802EFAAu, TB)                                             /* b 0x802efaa */
  }
  { u32 sp = reg[REG_SP];                                           /* pop {r3, r4, r5} */
    reg[3] = spf_wram32(sp); reg[4] = spf_wram32(sp + 4); reg[5] = spf_wram32(sp + 8);
    reg[REG_SP] = sp + 12; TST(0x0802EFACu, 3 * ws_cyc_seq[sp >> 24][1]) }
  reg[8] = reg[3];  TST(0x0802EFAEu, 0)                              /* mov r8, r3 */
  reg[9] = reg[4];  TST(0x0802EFB0u, 0)                              /* mov sb, r4 */
  reg[10] = reg[5]; TST(0x0802EFB2u, 0)                              /* mov sl, r5 */
  { u32 sp = reg[REG_SP];                                           /* pop {r4-r7} */
    for (u32 i = 0; i < 4; i++)
      reg[4 + i] = spf_wram32(sp + 4 * i);
    reg[REG_SP] = sp + 16; TST(0x0802EFB4u, 4 * ws_cyc_seq[sp >> 24][1]) }
  { u32 sp = reg[REG_SP];                                           /* pop {r0} */
    reg[0] = spf_wram32(sp);
    reg[REG_SP] = sp + 4; TST(0x0802EFB6u, ws_cyc_seq[sp >> 24][1]) }
  /* bx r0: 0x0300261C, ARM (no charge: to the ARM loop) */

  /* back in the dispatcher, system mode */
C1C: { u32 a = reg[REG_SP];                                         /* ldm sp!, {lr} */
       if (!STACK_OK(a, 1)) STOP(0x0300261Cu);
       reg[REG_LR] = spf_wram32(a); reg[REG_SP] = a + 4; cyc -= ws_cyc_seq[a >> 24][1]; }
     AST(0x03002620u)
C20: reg[3] = CPSR_NOW(); AST(0x03002624u)                          /* mrs r3, cpsr */
C24: reg[3] &= ~0xDFu; AST(0x03002628u)                             /* bic r3, r3, #0xdf */
C28: reg[3] |= 0x92; AST(0x0300262Cu)                               /* orr r3, r3, #0x92 */
C2C: { u32 cpsr = CPSR_NOW();                                       /* msr cpsr_fc, r3: IRQ mode, I set */
       u32 nw = (reg[3] & MSR_MASK) | (cpsr & ~MSR_MASK);
       if ((nw & 0x1F) != 0x12 || !(nw & 0x80) || !PRIVMODE(reg[CPU_MODE]))
         STOP(0x0300262Cu);
       reg[REG_CPSR] = nw;
       n = nw >> 31; z = (nw >> 30) & 1; c = (nw >> 29) & 1; v = (nw >> 28) & 1;
       set_cpu_mode(MODE_IRQ); }
     AST(0x03002630u)
C30: { u32 a = reg[REG_SP];                                         /* pop {r0, r1, r3, lr} */
       if (!STACK_OK(a, 4)) STOP(0x03002630u);
       reg[0] = spf_wram32(a); reg[1] = spf_wram32(a + 4);
       reg[3] = spf_wram32(a + 8); reg[REG_LR] = spf_wram32(a + 12);
       reg[REG_SP] = a + 16; cyc -= 4 * ws_cyc_seq[a >> 24][1]; }
     AST(0x03002634u)
C34: if (reg[3] != 0x04000200u || !(reg[REG_CPSR] & 0x80))         /* strh r1, [r3] (IE back) */
       STOP(0x03002634u);
     cyc -= ws_cyc_nseq[4][0]; write_memory16(0x04000200u, (u16)reg[1]);
     AST(0x03002638u)
C38: REG_SPSR(reg[CPU_MODE]) = (reg[0] & MSR_MASK) |               /* msr spsr_fc, r0 */
       (REG_SPSR(reg[CPU_MODE]) & ~MSR_MASK);
     AST(0x0300263Cu)
C3C: { u32 t = reg[REG_LR];                                         /* bx lr: to the BIOS */
       if (t & 1) STOP(0x0300263Cu);
       cyc -= ws_cyc_nseq[t >> 24][1] + ws_cyc_seq[(t >> 24) & 0xF][1];
       STOP(t); }
#undef STOP
#undef AST
#undef ABR
#undef TST
#undef CPSR_NOW
#undef SUBF
#undef LOGF
#undef STACK_OK

out:
  if (cyc == cyc0)
    return (u32)cyc;                                  /* stopped before its first instruction */
  return (u32)cyc | ((u64)(1 | (n << 4) | (z << 3) | (c << 2) | (v << 1)) << 32);
}

extern "C" uint64_t hle_spf2_run(uint32_t pc, int32_t cycles, uint32_t nzcv)
{
  s32 cyc = cycles;   /* gpSP's s32 (int; int32_t is long on the device) */
  if (pc == 0x03002520u) {
    uint64_t r = spf_irq_fast(cyc, nzcv);
    if (r >> 32)
      return r;
  }
  if (pc - 0x03002520u < 0x120u)
    return spf_irq_step(pc, cyc, nzcv);
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
    if (!spf_code_ok(0))
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
