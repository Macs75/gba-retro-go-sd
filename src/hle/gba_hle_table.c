/* gba_hle_table.c -- which games have a native routine, and loading it.
 *
 * One line per game. On the device each routine is an overlay (ld/gba_core.ld
 * OVERLAY: linked to run at __gba_hle_window in ITCM, stored in gba.xip);
 * gba_hle_select copies the loaded game's into the window. The host links
 * every routine in place and copies nothing. Everything here runs once, at
 * game load: the flash (GBA_COLD).
 */
#include <string.h>
#include "../gpsp/common.h"   /* gpSP's: another common.h is earlier in the path */
#include "gba_hle.h"

#ifndef HOST_BUILD
#define HLE_OVERLAY(n) extern const uint8_t __load_start_gba_hle_##n[], __gba_hle_size_##n[];
HLE_OVERLAY(vrally3)
HLE_OVERLAY(spf2)
extern uint8_t __gba_hle_window[];
/* the size is an absolute linker symbol: its "address" is the value */
#define HLE_BYTES(n) __load_start_gba_hle_##n, (uint32_t)(uintptr_t)__gba_hle_size_##n
#else
#define HLE_BYTES(n) 0, 0
#endif

static const gba_hle_desc gba_hle_games[] = {
    /* V-Rally 3: its polygon rasterizer, 0x03003AB0-0x03003D04 in IWRAM */
    { { 'A', 'V', 'R', 'E' }, 0x03003AB0u, 0x258u, hle_vrally3_run, HLE_BYTES(vrally3) },
    /* Super Puzzle Fighter II: its sound mixing loops, 0x03000038-0x03000268,
     * and its line-interrupt dispatcher, 0x03002520-0x0300263C */
    { { 'A', 'Z', '8', 'E' }, 0x03000038u, 0x2608u, hle_spf2_run, HLE_BYTES(spf2) },
};

GBA_COLD void gba_hle_select(const char *game_code)
{
    game_hle_pc = 0;
    game_hle_size = 0;      /* (pc - game_hle_pc) < 0: never */
    game_hle_fn = 0;
    for (unsigned i = 0; i < sizeof gba_hle_games / sizeof gba_hle_games[0]; i++) {
        const gba_hle_desc *d = &gba_hle_games[i];
        if (memcmp(game_code, d->code, 4) != 0)
            continue;
#ifndef HOST_BUILD
        /* the overlay's bytes to the ITCM window it was linked for */
        memcpy(__gba_hle_window, d->load_start, d->load_size);
        __asm volatile("dsb\n\tisb" ::: "memory");
        /* an array, not a literal: a literal joins the shared string pool at
         * the front of the flash data and moves everything behind it */
        static const char msg[] = "gba: native routine for %.4s, %u bytes in ITCM\n";
        printf(msg, game_code, (unsigned)d->load_size);
#endif
        game_hle_fn = (unsigned long long (*)(u32, s32, u32))d->run;
        game_hle_pc = d->pc;
        game_hle_size = d->size;
        return;
    }
}
