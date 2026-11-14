#ifndef GBA_TIMING_H
#define GBA_TIMING_H

#include <stdint.h>

/*
 * The GBA's frame: 280,896 cycles of a 16.777216 MHz clock, 59.7275 Hz.
 * gpSP's mixer runs at GBA_OUT_RATE, so a frame produces 48000 * 280896 /
 * 2^24 = 803.65 samples on average (803 or 804). The main loop pushes them
 * all into the firmware's ISR-fed ring and paces on its fill level
 * (main_gba.c), so it runs at the SAI's real rate / 803.65: the GBA's
 * frame rate, as exactly as the SAI is 48 kHz.
 */
#define GBA_OUT_RATE        48000u
#define GBA_FRAME_CYCLES_I  280896u
#define GBA_CPU_HZ_I        16777216u

/* once the SAI runs at GBA_OUT_RATE (after odroid_audio_init): read its real
 * rate back, derive the frame rate, set the LCD to it */
void gba_timing_apply(void);
/* the frame rate in milli-Hz and the SAI rate in milli-Hz */
uint32_t gba_timing_frame_mhz(void);
uint32_t gba_timing_rate_mhz(void);
/* before every common_emu_frame_loop(): the firmware pacer's frame time for
 * this frame (whole 10 us units whose mean is the exact frame time) */
void gba_timing_frame_tick(void);

#endif
