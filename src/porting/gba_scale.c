/*
 * gba_scale.c -- 240x160 to the panel's 320 columns, FIT (213 rows) and
 * FULL/CUSTOM (240 rows) with the "hard" filter: the arcade core's ROT0 ring
 * (gw-arcade-core src/arcade/blit_handlers.c, rot0_d2_run + hs565_3to4).
 *
 * Horizontal, on the CPU: every 3 source pixels make 4, (s0, s1,
 * avg(s1,s2), s2) -- the arcade's 3:4 lane, the average in the expanded
 * 0x07E0F81F form so the channels cannot carry into each other.
 * Vertical, on DMA2D: each source row is scaled once into a ring of rows in
 * AHB (DMA2D cannot read DTCM; each row cleaned from the D-cache), and
 * every output row is DMA2D's blend of the two rows around it,
 *     out = r0 * (32 - wy)/32 + r1 * wy/32,
 * a plain copy where wy is 0. DMA2D writes the (uncached) LCD buffer while
 * the CPU scales the next source row. The cost is the output pixels, not
 * the ratio: the arcade's lanes measured 282-318K cycles a frame for
 * 320x240 (docs/ARCADE_BLIT.md), as the nearest-neighbour scaler here did.
 *
 * Host: the same lane, the vertical blend computed on the CPU.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "gw_malloc.h"
#ifndef HOST_BUILD
#include "stm32h7xx.h"
#include "gw_core_bridge.h"
#define GBA_ITCM __attribute__((section(".gba_itcm_text")))
#else
#define GBA_ITCM
#endif
#include "gba_scale.h"

#define SRC_W   240
#define SRC_H   160
#define OUT_W   320
#define SLOTS   5
#define XMASK   0x07E0F81Fu

static uint16_t *ring[SLOTS];
static int ready;

int gba_scale_init(void)
{
    if (ready)
        return ready > 0;
    uint8_t *m = (uint8_t *)ahb_calloc(SLOTS * OUT_W * 2 + 32, 1);
    if (!m) {
        printf("gba: no AHB for the scaler's row ring: nearest-neighbour scaler\n");
        ready = -1;
        return 0;
    }
    m = (uint8_t *)(((uintptr_t)m + 31u) & ~(uintptr_t)31u);
    for (int i = 0; i < SLOTS; i++)
        ring[i] = (uint16_t *)(m + (size_t)i * OUT_W * 2);
    ready = 1;
    return 1;
}

static inline uint32_t expand(uint32_t c) { return (c | (c << 16)) & XMASK; }
static inline uint32_t fold(uint32_t e) { return (e | (e >> 16)) & 0xFFFFu; }

/* one source row to 320 pixels: (s0, s1, avg(s1,s2), s2) per 3 */
GBA_ITCM static void lane_3to4(uint16_t *restrict d, const uint16_t *restrict s)
{
    uint32_t *d32 = (uint32_t *)d;
    for (int x = 0; x < SRC_W; x += 3, d32 += 2) {
        uint32_t b0 = s[x], b1 = s[x + 1], b2 = s[x + 2];
        uint32_t m = fold(((expand(b1) + expand(b2)) >> 1) & XMASK);
        d32[0] = b0 | (b1 << 16);
        d32[1] = m | (b2 << 16);
    }
}

#ifndef HOST_BUILD
GBA_ITCM static inline void d2_wait(void)
{
    while (DMA2D->CR & DMA2D_CR_START) { }
}

GBA_ITCM static void d2_frame_begin(void)
{
    d2_wait();
    DMA2D->CR = 0;
    DMA2D->BGPFCCR = (0xFFu << DMA2D_BGPFCCR_ALPHA_Pos) | DMA2D_BGPFCCR_AM_0 |
                     (2u << DMA2D_BGPFCCR_CM_Pos);            /* RGB565 */
    DMA2D->OPFCCR  = 2u << DMA2D_OPFCCR_CM_Pos;               /* RGB565 */
    DMA2D->FGOR = 0;
    DMA2D->BGOR = 0;
    DMA2D->OOR  = 0;
    DMA2D->NLR  = ((uint32_t)OUT_W << DMA2D_NLR_PL_Pos) | (1u << DMA2D_NLR_NL_Pos);
    DMA2D->IFCR = 0x3Fu;
}

/* out row = a*(32-wy) + b*wy /32, written by DMA2D */
GBA_ITCM static void d2_kick(const uint16_t *a, const uint16_t *b, uint32_t wy, uint16_t *dst)
{
    uint32_t alpha = (wy * 255u + 16u) >> 5;
    d2_wait();
    DMA2D->FGPFCCR = (alpha << DMA2D_FGPFCCR_ALPHA_Pos) | DMA2D_FGPFCCR_AM_0 |
                     (2u << DMA2D_FGPFCCR_CM_Pos);
    DMA2D->FGMAR = (uint32_t)(uintptr_t)b;
    DMA2D->BGMAR = (uint32_t)(uintptr_t)a;
    DMA2D->OMAR  = (uint32_t)(uintptr_t)dst;
    DMA2D->CR    = DMA2D_CR_MODE_1 | DMA2D_CR_START;          /* M2M blend */
}
#else
static void d2_frame_begin(void) {}
static void d2_wait(void) {}
static void d2_kick(const uint16_t *a, const uint16_t *b, uint32_t wy, uint16_t *dst)
{
    for (int x = 0; x < OUT_W; x++) {
        uint32_t e = (expand(a[x]) * (32u - wy) + expand(b[x]) * wy) >> 5;
        dst[x] = (uint16_t)fold(e & XMASK);
    }
}
#endif

static inline void lane(uint16_t *d, const uint16_t *src, int r)
{
    lane_3to4(d, src + (size_t)r * SRC_W);
#ifndef HOST_BUILD
    SCB_CleanDCache_by_Addr((uint32_t *)d, OUT_W * 2);
#endif
}

GBA_ITCM void gba_scale_320(const uint16_t *src, uint16_t *dst, int out_h)
{
    const uint32_t ystep = ((uint32_t)(SRC_H - 1) << 16) / (uint32_t)(out_h - 1);
    uint16_t *A = NULL, *B = NULL;          /* ring slots holding r0 / r1 */
    int A_src = -1, B_src = -1;
    const uint16_t *ia = NULL, *ib = NULL;  /* slots the in-flight transfer reads */
    uint32_t sy = 0;

    d2_frame_begin();
    for (int dy = 0; dy < out_h; dy++, sy += ystep, dst += OUT_W) {
        int r0 = (int)(sy >> 16);
        uint32_t wy = (sy >> 11) & 0x1F;
        int r1 = (wy && r0 + 1 < SRC_H) ? r0 + 1 : r0;
        if (r1 == r0)
            wy = 0;

        if (A_src != r0) {
            if (B_src == r0) {              /* the usual advance: B becomes A */
                uint16_t *t = A; A = B; B = t;
                int ts = A_src; A_src = B_src; B_src = ts;
            } else {
                uint16_t *f = NULL;
                for (int k = 0; k < SLOTS && !f; k++) {
                    uint16_t *s = ring[k];
                    if (s != A && s != B && s != ia && s != ib) f = s;
                }
                A = f;
                lane(A, src, r0);
                A_src = r0;
            }
        }
        if (wy && B_src != r1) {
            uint16_t *f = NULL;
            for (int k = 0; k < SLOTS && !f; k++) {
                uint16_t *s = ring[k];
                if (s != A && s != B && s != ia && s != ib) f = s;
            }
            B = f;
            lane(B, src, r1);
            B_src = r1;
        }

        const uint16_t *lo = wy ? B : A;
        d2_kick(A, lo, wy, dst);
        ia = A;
        ib = lo;
    }
    d2_wait();      /* the overlay draws into the buffer with the CPU next */
}
