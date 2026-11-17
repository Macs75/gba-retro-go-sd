/* gba_hle.h -- game-specific native routines (game HLE).
 *
 * A game whose frame is dominated by one guest routine can have that routine
 * run natively: the interpreter (cpu.cc) calls game_hle_fn when a jump lands
 * in [game_hle_pc, game_hle_pc + game_hle_size). The routine must leave the
 * machine exactly as the interpreter would (memory, registers, flags,
 * cycles, and the instruction it stops at), so REC/hash replays stay
 * identical; see src/hle/hle_vrally3.cc for the contract.
 *
 * Each game's routine is its own unit (src/hle/hle_<game>.cc). On the device
 * it is an overlay: linked to run at one ITCM address shared by all of them
 * (ld/gba_core.ld OVERLAY), stored in gba.xip, and copied to ITCM by
 * gba_hle_select() when that game is loaded; other games load nothing. The
 * host links every routine in place.
 */
#ifndef GBA_HLE_H
#define GBA_HLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* pc: where the jump landed; cycles: the slice left; nzcv: n<<3|z<<2|c<<1|v.
 * Returns the cycles in the low word; the high word is 0 (declined: the
 * interpreter runs the instruction at pc) or 1|n<<4|z<<3|c<<2|v<<1 with the
 * registers and reg[REG_PC] set. */
typedef uint64_t (*gba_hle_fn)(uint32_t pc, int32_t cycles, uint32_t nzcv);

typedef struct {
    char        code[4];      /* the cart's game code (header 0xAC) */
    uint32_t    pc, size;     /* the guest routine's range */
    gba_hle_fn  run;          /* its entry (in the ITCM window on the device) */
    /* the overlay's bytes in gba.xip, and how many: a size, not an end
     * address -- the last overlay ends exactly at gba.xip's end, which the
     * relocation of addresses into the blob (patch_gba_sentinels:
     * [start, end)) leaves unpatched */
    const uint8_t *load_start;
    uint32_t       load_size;
} gba_hle_desc;

/* Called by load_gamepak: arms the hook for this game, or disarms it. */
void gba_hle_select(const char *game_code);

/* The interpreter's own word store (cpu.cc), for the routines. */
uint32_t cpu_hle_store32(uint32_t address, uint32_t value);

uint64_t hle_vrally3_run(uint32_t pc, int32_t cycles, uint32_t nzcv);
uint64_t hle_spf2_run(uint32_t pc, int32_t cycles, uint32_t nzcv);
uint64_t hle_rzw_run(uint32_t pc, int32_t cycles, uint32_t nzcv);

#ifdef __cplusplus
}
#endif
#endif
