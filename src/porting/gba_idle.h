#ifndef GBA_IDLE_H
#define GBA_IDLE_H

/* the generic poll-loop skip (src/porting/gba_idle.c) */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* turn it on for a game no per-game table covers (main_gba.c, at load) */
void gba_idle_enable(void);
/* forget every armed / rejected loop (a power-on) */
void gba_idle_reset(void);
/* cpu.cc, at every event (before update_gba): the PC the interpreter is at
 * (0xFFFFFFFF: halted) and whether it runs Thumb */
void gba_idle_event(uint32_t pc, int thumb);
/* cpu.cc, when the PC arrives at the armed loop's head: the cycles left in
 * the slice and the live flags; returns the cycles left after the skip */
int32_t gba_idle_head(int32_t cycles, uint32_t n, uint32_t z, uint32_t c, uint32_t v);

/* reads with a side effect (the EEPROM's bit counter, the backup chip):
 * gba_memory.c counts them, so a pass that made one is never repeated */
extern uint32_t gba_idle_sidefx;
/* Thumb pushes (cpu.cc exec_block_mem_fast) at [gba_idle_stk_lo, +64) are
 * not side effects: the probe of a loop that calls compares those bytes
 * itself (0: every store counts) */
extern uint32_t gba_idle_stk_lo;

/* statistics (PROF / host): ARM7 cycles skipped, skips, loops armed,
 * loops rejected */
extern uint32_t gba_idle_skipped, gba_idle_skips, gba_idle_arms, gba_idle_rejects;
extern uint32_t gba_idle_head_pc;

#ifdef __cplusplus
}
#endif

#endif
