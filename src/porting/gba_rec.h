#ifndef GBA_REC_H
#define GBA_REC_H

#include <stdint.h>

/* input record / replay (src/porting/gba_rec.c) */
enum { GBA_REC_OFF = 0, GBA_REC_RECORD, GBA_REC_REPLAY };

int gba_rec_mode(void);
/* the frames played back / recorded so far, and the .rec file's path */
uint32_t gba_rec_frame(void);
const char *gba_rec_file(void);
/* opens "<rom path without extension>.rec" and powers the machine on with
 * power_on(); 0 (logged) if the file cannot be used. rom_hash names the
 * game, setup_hash what else changes the emulation (the BIOS, the cheats) */
int gba_rec_start(int mode, const char *rom_path, uint32_t rom_hash, uint32_t setup_hash,
                  void (*power_on)(void));
/* closes the file (a recording gets its frame count); the game goes on */
void gba_rec_close(void);
/* the keys (KEYINPUT bits, set = held) the emulated frame gets: recorded,
 * or replayed in place of the live ones */
uint16_t gba_rec_input(uint16_t live);
/* the samples an emulated frame produced (stereo pairs), as read */
void gba_rec_audio(const int16_t *audio, int pairs);
/* after each emulated frame */
void gba_rec_frame_done(void);
/* 0 once a replay has run this session: the cart save in RAM is then the
 * recording's, not the player's, and must not be written to the SD */
int gba_rec_backup_is_players(void);
/* FNV-1a, for the callers' hashes */
uint32_t gba_rec_fnv(const void *p, uint32_t n, uint32_t h);
#define GBA_REC_FNV0 2166136261u

#endif
