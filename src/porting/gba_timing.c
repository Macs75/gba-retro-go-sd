/*
 * gba_timing.c -- the GBA's real frame rate on the G&W (from the Neo Geo
 * core's neo_timing.c).
 *
 * The loop runs at the SAI's rate / 803.65 samples (gba_timing.h). The SAI
 * is not exactly 48 kHz (PLL2 and the SAI divider round it), so its rate is
 * read back from the PLL2 and SAI registers, and the LCD (PLL3) and the
 * firmware's pacer are set from it:
 *   - the LCD at the loop's rate, in milli-Hz (firmwares that only know
 *     whole rates ignore it and stay at 60 Hz): the two then keep their
 *     phase, so the point of the scan where each frame draws holds still
 *     (main_gba.c, gba_audio_start);
 *   - the pacer (common_emu_frame_loop) sums the measured frame time minus
 *     frame_time_10us, and skips frames when the sum passes one frame.
 *     frame_time_10us is whole 10 us units; the frame is not (1674.27). A
 *     rounded constant (1674, while the loop really took 1677) let the sum
 *     drift into a 2-frame skip every 600 frames on a frame that had 10-16%
 *     to spare (Kirby, 2026-11-14). Whole units chosen frame by frame so
 *     their mean is the exact frame time keep the sum on real slowdowns.
 */
#include <odroid_system.h>
#include <stdint.h>
#include <stdio.h>
#include "common.h"
#include "gw_lcd.h"
#include "gba_timing.h"
#ifdef HOST_BUILD
#include "host_compat.h"
#else
#include "stm32h7xx.h"
#include "gw_core_bridge.h"
#endif

static uint32_t rate_mhz = GBA_OUT_RATE * 1000u;
static uint32_t frame_mhz = 59727u;
/* the frame time in 10 us units, 16.16 fixed point, and the fraction
 * carried from frame to frame */
static uint32_t frame_q16 = 1674u << 16, frame_frac;

uint32_t gba_timing_frame_mhz(void) { return frame_mhz; }
uint32_t gba_timing_rate_mhz(void) { return rate_mhz; }

#ifndef HOST_BUILD
/* the SAI's sample rate in milli-Hz from PLL2 and the SAI divider (0: a
 * configuration this does not decode); neo_timing.c's */
static uint32_t sai_rate_mhz(void)
{
    uint32_t ck = RCC->PLLCKSELR, divr = RCC->PLL2DIVR, cr1 = SAI1_Block_A->CR1;
    uint32_t src, m, n, p, frac = 0, div, osr;

    if ((ck & RCC_PLLCKSELR_PLLSRC) != RCC_PLLCKSELR_PLLSRC_HSI || (cr1 & SAI_xCR1_NODIV))
        return 0;
    src = HSI_VALUE >> ((RCC->CR & RCC_CR_HSIDIV) >> RCC_CR_HSIDIV_Pos);
    m = (ck & RCC_PLLCKSELR_DIVM2) >> RCC_PLLCKSELR_DIVM2_Pos;
    n = ((divr & RCC_PLL2DIVR_N2) >> RCC_PLL2DIVR_N2_Pos) + 1;
    p = ((divr & RCC_PLL2DIVR_P2) >> RCC_PLL2DIVR_P2_Pos) + 1;
    if (RCC->PLLCFGR & RCC_PLLCFGR_PLL2FRACEN)
        frac = (RCC->PLL2FRACR & RCC_PLL2FRACR_FRACN2) >> RCC_PLL2FRACR_FRACN2_Pos;
    div = (cr1 & SAI_xCR1_MCKDIV) >> SAI_xCR1_MCKDIV_Pos;
    if (!div)
        div = 1;
    osr = (cr1 & SAI_xCR1_OSR) ? 2 : 1;
    if (!m)
        return 0;
    return (uint32_t)((uint64_t)src * (n * 8192u + frac) * 1000u /
                      ((uint64_t)m * 8192u * p * div * 256u * osr));
}
#endif

void gba_timing_apply(void)
{
#ifndef HOST_BUILD
    uint32_t got = sai_rate_mhz();
    rate_mhz = (got > (GBA_OUT_RATE - 100u) * 1000u && got < (GBA_OUT_RATE + 100u) * 1000u)
               ? got : GBA_OUT_RATE * 1000u;
#endif
    /* frame rate = SAI rate * 2^24 / (48000 * 280896), in milli-Hz */
    frame_mhz = (uint32_t)(((uint64_t)rate_mhz * GBA_CPU_HZ_I +
                            (uint64_t)GBA_OUT_RATE * GBA_FRAME_CYCLES_I / 2u) /
                           ((uint64_t)GBA_OUT_RATE * GBA_FRAME_CYCLES_I));
    frame_q16 = (uint32_t)(((uint64_t)100000000u << 16) / frame_mhz);
    frame_frac = 0;
    gba_timing_frame_tick();
    /* a firmware that only knows whole rates ignores this: the LCD keeps the
     * launcher's 60 Hz */
    lcd_set_refresh_rate(frame_mhz);
    printf("gba: audio %lu.%03lu Hz: frame rate %lu.%03lu Hz (real GBA 59.727)\n",
           (unsigned long)(rate_mhz / 1000u), (unsigned long)(rate_mhz % 1000u),
           (unsigned long)(frame_mhz / 1000u), (unsigned long)(frame_mhz % 1000u));
}

void gba_timing_frame_tick(void)
{
    frame_frac += frame_q16;
    common_emu_state.frame_time_10us = (uint16_t)(frame_frac >> 16);
    frame_frac &= 0xffffu;
}
