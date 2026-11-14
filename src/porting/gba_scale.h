#ifndef GBA_SCALE_H
#define GBA_SCALE_H

#include <stdint.h>

/* the scaler's row ring (AHB, 3.2 KB): 1, or 0 when there is no room (the
 * caller then keeps its nearest-neighbour scaler) */
int gba_scale_init(void);
/* the 240x160 picture to 320 x out_h rows at dst (the LCD buffer, the first
 * of the rows to write): the arcade core's 3:4 lane and DMA2D row blend
 * (gba_scale.c) */
void gba_scale_320(const uint16_t *src, uint16_t *dst, int out_h);

#endif
