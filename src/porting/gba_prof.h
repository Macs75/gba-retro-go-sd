#ifndef GBA_PROF_H
#define GBA_PROF_H

/*
 * Device profiling: `make PROF=1` (GBA_PROF) times the main loop's phases
 * with the Cortex-M7 DWT cycle counter and prints PROF/ lines every
 * GBA_PROF_WINDOW frames (src/porting/gba_prof.c). Without it every probe
 * compiles to nothing.
 *
 * GBA_PROF_MARK(phase) charges the time since the previous mark to one of
 * the main loop's phases (src/main_gba.c); GBA_PROF_START/STOP(part)
 * accumulate a part of the emulation inside execute_arm() (the scanline
 * renderer, the M4A mixer).
 */

/* parts timed inside execute_arm() */
#define GBA_PART_PPU  0   /* update_scanline's drawing (video.cc) */
#define GBA_PART_M4A  1   /* the M4A mixer HLE (m4a_gpsp.c) */
#define GBA_PART_N    2

/* the main loop's phases, in loop order */
enum {
    GBA_PH_FRAMECTL,   /* common_emu_frame_loop + input + pause menu */
    GBA_PH_EMU,        /* execute_arm: the ARM7, the PPU, the sound */
    GBA_PH_AUDIO,      /* gba_pcm_submit: the samples into the ISR's ring */
    GBA_PH_WAIT,       /* gba_audio_pace: sleeping until the ring has room */
    GBA_PH_SCALE,      /* 240x160 to the LCD buffer */
    GBA_PH_PRESENT,    /* overlay + lcd_swap */
    GBA_PH_N
};

#if defined(GBA_PROF) && !defined(HOST_BUILD)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_PROF_WINDOW 120u
#define GBA_PROF_NOW()  (*(volatile uint32_t *)0xE0001004u)   /* DWT_CYCCNT */

extern uint32_t gba_prof_acc[GBA_PART_N];
extern uint32_t gba_prof_t0[GBA_PART_N];
extern uint32_t gba_prof_m4a_calls;

/* once before the loop: the audio-paced frame (samples per frame / rate)
 * is the budget */
void gba_prof_init(unsigned spf, unsigned rate);
void gba_prof_mark(int phase);
/* the PC sampler (gba_prof.c): where gba.xip landed in flash, and the
 * firmware's interrupt table back before the core quits */
void gba_prof_set_xip(const void *addr, uint32_t size);
void gba_prof_quit(void);
/* end of a main-loop iteration: drawn = the frame was drawn and presented,
 * fw_skip = common_emu_frame_loop asked to skip it, vskip = its picture was
 * dropped because the last swap was still pending, waited = the audio
 * pacing slept (the frame was on time) */
void gba_prof_frame_end(int drawn, int fw_skip, int vskip, int waited);

#ifdef __cplusplus
}
#endif

#define GBA_PROF_START(x)        (gba_prof_t0[(x)] = GBA_PROF_NOW())
#define GBA_PROF_STOP(x)         (gba_prof_acc[(x)] += GBA_PROF_NOW() - gba_prof_t0[(x)])
#define GBA_PROF_INIT(n, r)      gba_prof_init((n), (r))
#define GBA_PROF_MARK(p)         gba_prof_mark(p)
#define GBA_PROF_FRAME_END(d, s, v, w) gba_prof_frame_end((d), (s), (v), (w))

#else

#define GBA_PROF_START(x)        ((void)0)
#define GBA_PROF_STOP(x)         ((void)0)
#define GBA_PROF_INIT(n, r)      ((void)0)
#define GBA_PROF_MARK(p)         ((void)0)
#define GBA_PROF_FRAME_END(d, s, v, w) ((void)0)

#endif

#endif
