/* gba_backup_bank.h -- the cart save in 64 KB of RAM, a 1 Mbit flash's
 * sectors not in RAM on file (gba_backup_bank.c). */
#ifndef GBA_BACKUP_BANK_H
#define GBA_BACKUP_BANK_H
#include <stdio.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* the save image in save files and recordings: two 64 KB banks */
#define GBA_BACKUP_IMAGE_BYTES (128u * 1024u)
/* per 4 KB slot of gamepak_backup: its sector's bank; not in that bank's file */
extern uint32_t backup_sect_owner, backup_sect_dirty;
void gba_backup_reset(const char *save_path);
void gba_backup_take(uint32_t bank, uint32_t sector, uint32_t erase);
void gba_backup_chip_erase(void);
int gba_backup_export(FILE *f);
uint32_t gba_backup_import(FILE *f, uint32_t len);
int gba_backup_all_zero(uint32_t two_banks);
#ifdef __cplusplus
}
#endif
#endif
