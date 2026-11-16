/* gba_backup_bank.c -- the cart save in 64 KB of RAM.
 *
 * gamepak_backup holds 64 KB: the whole save of every cart type (EEPROM
 * <= 8 KB, SRAM 32 KB, 512 Kbit flash 64 KB) and, for a 1 Mbit flash
 * (Pokemon R/S/E/FR/LG, Mystery Dungeon, Super Mario Advance 4), the sectors
 * the game is using of its two 64 KB banks: such a chip shows one bank at a
 * time, the game switching with a command.
 *
 * The 64 KB are 16 slots of 4 KB, the chip's sector. Slot s holds sector s
 * of bank 0 or of bank 1 (backup_sect_owner, a bit per slot), and
 * backup_sect_dirty marks the slots whose sector is not in its bank's file.
 * The sectors not in RAM are in <save>.bank0 / <save>.bank1 (a bank with no
 * file is all 0xFF). gba_memory.c checks the slot at every access
 * (BACKUP_SECT): a sector of the other bank is taken in its place --
 * written out first if dirty, 4 KB at its offset (fopen "r+b": FatFs
 * FA_READ|FA_WRITE in the firmware's syscalls.c), then read, 4 KB -- and a
 * sector the game is about to erase is not read at all. So a Pokemon save
 * (erase then write each sector) does no reading, and a load reads 4 KB per
 * sector as the game reads it, not 64 KB at once: the whole-bank swaps were
 * 0.4-0.6 s stops on the device (Ruby, 2026-11-16).
 *
 * Every save file and recording keeps the 128 KB image it always had
 * (gba_backup_export / gba_backup_import: bank 0 then bank 1), so earlier
 * saves and recordings load unchanged. Savestates carry the flash state
 * (bank number included), not the data.
 *
 * This keeps 64 KB of the core's RAM free for every game: the second bank
 * had a permanent 64 KB in RAM for the 7 of 150 games that use it.
 *
 * Everything here runs at load, at a save and at a sector change: the flash
 * (GBA_COLD).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../gpsp/common.h"   /* gpSP's: another common.h is earlier in the path */
#include "gba_backup_bank.h"

#ifndef HOST_BUILD
#define BANK_TAIL_BSS __attribute__((section(".gba_tail_bss")))   /* no .bss shift */
#else
#define BANK_TAIL_BSS
#endif

#define BANK_BYTES (64u * 1024u)
#define SECT_BYTES (4u * 1024u)
#define SECTS      (BANK_BYTES / SECT_BYTES)

/* per slot: the bank its sector belongs to; not in that bank's file */
uint32_t backup_sect_owner BANK_TAIL_BSS;
uint32_t backup_sect_dirty BANK_TAIL_BSS;

/* whether each bank has a file (else the sectors not in RAM are all 0xFF),
 * and their names (<save path>.bank0 / .bank1) */
static u8 bank_valid[2] BANK_TAIL_BSS;
static char bank_name[2][192] BANK_TAIL_BSS;

int odroid_sdcard_mkdir(const char *path);   /* the firmware's (creates parents) */
extern u32 backup_type_reset, flash_bank_cnt;   /* gba_memory.c: the detected save type */

GBA_COLD void gba_backup_reset(const char *save_path)
{
  backup_sect_owner = 0;
  backup_sect_dirty = 0;
  bank_valid[0] = bank_valid[1] = 0;
  for (u32 b = 0; b < 2; b++) {
    if (save_path)
      snprintf(bank_name[b], sizeof bank_name[b], "%s.bank%lu", save_path, (unsigned long)b);
    else
      bank_name[b][0] = 0;
  }
}

/* A new bank file, for writing and reading back. The save folder may not
 * exist yet (a new game, no save written so far): then it is created and
 * the open retried. */
static GBA_COLD FILE *bank_create(u32 bank)
{
  const char *p = bank_name[bank & 1];
  if (!p[0])
    return NULL;
  FILE *f = fopen(p, "w+b");
  if (f)
    return f;
  char dir[192];
  snprintf(dir, sizeof dir, "%s", p);
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir)
    return NULL;
  *slash = 0;
  odroid_sdcard_mkdir(dir);
  return fopen(p, "w+b");
}

/* A 4 KB sector of a bank to its file. A bank with no file yet gets one,
 * all 0xFF (what a bank without a file is) but this sector. 0 on failure. */
#ifdef HOST_BUILD
static u32 st_n_fetch, st_n_store;   /* the self-test's I/O counts */
#define ST_COUNT(v) (v)++
#else
#define ST_COUNT(v)
#endif

static GBA_COLD int sect_store(u32 bank, u32 s, const u8 *src)
{
  bank &= 1;
  ST_COUNT(st_n_store);
  FILE *f = bank_valid[bank] ? fopen(bank_name[bank], "r+b") : NULL;
  if (f) {
    int ok = fseek(f, (long)(s * SECT_BYTES), SEEK_SET) == 0 &&
             fwrite(src, 1, SECT_BYTES, f) == SECT_BYTES;
    ok &= fclose(f) == 0;
    if (ok)
      return 1;
  } else {
    f = bank_create(bank);
    if (f) {
      u8 ff[512];
      memset(ff, 0xFF, sizeof ff);
      int ok = 1;
      for (u32 t = 0; t < SECTS && ok; t++) {
        if (t == s)
          ok = fwrite(src, 1, SECT_BYTES, f) == SECT_BYTES;
        else
          for (u32 i = 0; i < SECT_BYTES && ok; i += sizeof ff)
            ok = fwrite(ff, 1, sizeof ff, f) == sizeof ff;
      }
      ok &= fclose(f) == 0;
      if (ok) {
        bank_valid[bank] = 1;
        return 1;
      }
    }
  }
  printf("gba: cannot write sector %lu of the save's bank %lu to %s\n",
         (unsigned long)s, (unsigned long)bank, bank_name[bank]);
  return 0;
}

/* A 4 KB sector of a bank from its file (all 0xFF if it has none). */
static GBA_COLD void sect_fetch(u32 bank, u32 s, u8 *dst)
{
  bank &= 1;
  memset(dst, 0xFF, SECT_BYTES);
  if (!bank_valid[bank])
    return;
  ST_COUNT(st_n_fetch);
  FILE *f = fopen(bank_name[bank], "rb");
  if (!f || fseek(f, (long)(s * SECT_BYTES), SEEK_SET) != 0 ||
      fread(dst, 1, SECT_BYTES, f) != SECT_BYTES)
    printf("gba: cannot read sector %lu of the save's bank %lu from %s\n",
           (unsigned long)s, (unsigned long)bank, bank_name[bank]);
  if (f)
    fclose(f);
}

/* gba_memory.c: the game accesses sector s of bank 'bank', whose slot holds
 * the other bank's. erase: the access is a sector erase (no read needed). */
GBA_COLD void gba_backup_take(uint32_t bank, uint32_t s, uint32_t erase)
{
  bank &= 1;
  s &= SECTS - 1;
  u32 bit = 1u << s;
  u8 *slot = &gamepak_backup[s * SECT_BYTES];
  u32 cur = (backup_sect_owner >> s) & 1;
  if (cur == bank)
    return;
  if ((backup_sect_dirty & bit) && !sect_store(cur, s, slot))
    printf("gba: sector %lu of the save's bank %lu could not be kept: its changes are lost\n",
           (unsigned long)s, (unsigned long)cur);
  if (erase)
    memset(slot, 0xFF, SECT_BYTES);     /* about to be erased anyway */
  else
    sect_fetch(bank, s, slot);
  backup_sect_owner = (backup_sect_owner & ~bit) | (bank << s);
  backup_sect_dirty &= ~bit;
}

/* gba_memory.c: a chip erase -- both banks all 0xFF: RAM, and no file */
GBA_COLD void gba_backup_chip_erase(void)
{
  memset(gamepak_backup, 0xFF, BANK_BYTES);
  backup_sect_dirty = 0;                /* 0xFF is what a bank without a file holds */
  bank_valid[0] = bank_valid[1] = 0;
}

/* Every changed slot to its bank's file (a bank without a file gets one,
 * whole: its slots and 0xFF). At load, for a 1 Mbit flash: bank 0 lives in
 * RAM only after the import, and each of its sectors would otherwise be
 * written out, 4 KB at a time, when the game first takes its slot during a
 * save -- 12 writes on the first Pokemon save of a session. */
static GBA_COLD void backup_flush(void)
{
  for (u32 b = 0; b < 2; b++) {
    u32 mine = (b ? backup_sect_owner : ~backup_sect_owner) & 0xFFFFu;
    u32 dirty = backup_sect_dirty & mine;
    if (!dirty)
      continue;
    if (bank_valid[b]) {
      for (u32 s = 0; s < SECTS; s++)
        if (((dirty >> s) & 1) && sect_store(b, s, &gamepak_backup[s * SECT_BYTES]))
          backup_sect_dirty &= ~(1u << s);
      continue;
    }
    FILE *f = bank_create(b);
    if (!f) {
      printf("gba: cannot write the save's bank %lu to %s\n", (unsigned long)b, bank_name[b]);
      continue;
    }
    u8 ff[512];
    memset(ff, 0xFF, sizeof ff);
    int ok = 1;
    for (u32 t = 0; t < SECTS && ok; t++) {
      if ((mine >> t) & 1)
        ok = fwrite(&gamepak_backup[t * SECT_BYTES], 1, SECT_BYTES, f) == SECT_BYTES;
      else
        for (u32 i = 0; i < SECT_BYTES && ok; i += sizeof ff)
          ok = fwrite(ff, 1, sizeof ff, f) == sizeof ff;
    }
    ok &= fclose(f) == 0;
    if (ok) {
      bank_valid[b] = 1;
      backup_sect_dirty &= ~mine;
    }
  }
}

/* The 128 KB image (bank 0, bank 1) to a file: the save file, a recording's
 * cart save. */
GBA_COLD int gba_backup_export(FILE *f)
{
  u8 sect[512];
  for (u32 b = 0; b < 2; b++) {
    FILE *src = NULL;
    if (bank_valid[b] && ((backup_sect_owner ^ (b ? 0xFFFFu : 0)) & 0xFFFFu))
      src = fopen(bank_name[b], "rb");
    for (u32 s = 0; s < SECTS; s++) {
      if (((backup_sect_owner >> s) & 1) == b) {
        if (fwrite(&gamepak_backup[s * SECT_BYTES], 1, SECT_BYTES, f) != SECT_BYTES)
          goto fail;
        continue;
      }
      int at = src && fseek(src, (long)(s * SECT_BYTES), SEEK_SET) == 0;
      for (u32 i = 0; i < SECT_BYTES; i += sizeof sect) {
        memset(sect, 0xFF, sizeof sect);
        if (at)
          fread(sect, 1, sizeof sect, src);
        if (fwrite(sect, 1, sizeof sect, f) != sizeof sect)
          goto fail;
      }
    }
    if (src)
      fclose(src);
    continue;
fail:
    if (src)
      fclose(src);
    return 0;
  }
  return 1;
}

/* The 128 KB image from a file, up to len bytes of it or to its end (a
 * shorter save: the rest 0xFF): bank 0 into RAM, bank 1 to its file only if
 * it is not all 0xFF (most saves). Returns the bytes read; the stream is
 * left after them. */
GBA_COLD uint32_t gba_backup_import(FILE *f, uint32_t len)
{
  u8 chunk[512], ffb[512];
  memset(ffb, 0xFF, sizeof ffb);
  u32 n0 = len < BANK_BYTES ? len : BANK_BYTES;
  memset(gamepak_backup, 0xFF, BANK_BYTES);
  backup_sect_owner = 0;
  backup_sect_dirty = 0xFFFFu;          /* bank 0 lives in RAM only */
  bank_valid[0] = bank_valid[1] = 0;
  u32 got = n0 ? (u32)fread(gamepak_backup, 1, n0, f) : 0;
  if (got < n0)
    return got;
  u32 n1 = len > BANK_BYTES ? len - BANK_BYTES : 0;
  if (n1 > BANK_BYTES)
    n1 = BANK_BYTES;
  FILE *out = NULL;
  u32 written = 0;          /* bytes of bank 1 in its file */
  u32 blank = 0;            /* leading all-0xFF bytes not written yet */
  for (u32 done = 0; done < n1; done += sizeof chunk) {
    u32 k = n1 - done < sizeof chunk ? n1 - done : (u32)sizeof chunk;
    memset(chunk, 0xFF, sizeof chunk);
    u32 r = (u32)fread(chunk, 1, k, f);
    got += r;
    u32 ff = 1;
    for (u32 i = 0; i < sizeof chunk; i++)
      if (chunk[i] != 0xFF) { ff = 0; break; }
    if (!out && ff) {
      blank += sizeof chunk;
    } else {
      if (!out) {
        out = bank_create(1);
        if (!out) {
          printf("gba: cannot write the save's bank 1 to %s: it is lost\n", bank_name[1]);
          break;
        }
        for (u32 i = 0; i < blank; i += sizeof ffb)
          written += (u32)fwrite(ffb, 1, sizeof ffb, out);
      }
      written += (u32)fwrite(chunk, 1, sizeof chunk, out);
    }
    if (r < k)
      break;                  /* the end of a shorter save */
  }
  if (out) {
    while (written < BANK_BYTES)
      written += (u32)fwrite(ffb, 1, sizeof ffb, out);
    fclose(out);
    bank_valid[1] = 1;
  }
  /* a 1 Mbit flash (detected by load_gamepak, before the save loads): bank
   * 0 to its file now, at load, not sector by sector at the first save */
  if (backup_type_reset == BACKUP_FLASH && flash_bank_cnt == FLASH_SIZE_128KB)
    backup_flush();
  return got;
}

/* Whether the image is all 0x00 (a blank save some frontends create): the
 * RAM slots, and with two_banks the sectors on file. */
GBA_COLD int gba_backup_all_zero(uint32_t two_banks)
{
  for (u32 i = 0; i < BANK_BYTES; i++)
    if (gamepak_backup[i])
      return 0;
  if (!two_banks)
    return 1;
  u8 chunk[512];
  for (u32 b = 0; b < 2; b++) {
    u32 on_file = (b ? ~backup_sect_owner : backup_sect_owner) & 0xFFFFu;   /* slots owned by the other bank */
    if (!on_file)
      continue;
    if (!bank_valid[b])
      return 0;                 /* all 0xFF, not 0x00 */
    FILE *f = fopen(bank_name[b], "rb");
    if (!f)
      return 0;
    int zero = 1;
    for (u32 s = 0; s < SECTS && zero; s++) {
      if (!((on_file >> s) & 1))
        continue;
      if (fseek(f, (long)(s * SECT_BYTES), SEEK_SET) != 0) { zero = 0; break; }
      for (u32 i = 0; i < SECT_BYTES && zero; i += sizeof chunk) {
        if (fread(chunk, 1, sizeof chunk, f) != sizeof chunk) { zero = 0; break; }
        for (u32 j = 0; j < sizeof chunk; j++)
          if (chunk[j]) { zero = 0; break; }
      }
    }
    fclose(f);
    if (!zero)
      return 0;
  }
  return 1;
}

#ifdef HOST_BUILD
/* Host: GBA_BACKUP_SELFTEST=<dir> gba_host -- drives read_backup/write_backup
 * with the flash command sequences of a 1 Mbit chip (ID, bank switch, byte
 * program, sector and chip erase) against a plain 128 KB model, and checks
 * the image import/export around them; files under <dir>. Exit status 0 =
 * every check passed. */
extern u32 backup_type, flash_mode, flash_command_position, flash_bank_num;
u8 read_backup(u32 address);
void write_backup(u32 address, u32 value);

static u8 st_model[2 * BANK_BYTES];
static int st_fail;

static void st_cmd(u32 c)
{
  write_backup(0x5555, 0xAA);
  write_backup(0x2AAA, 0x55);
  write_backup(0x5555, c);
}
static void st_bank(u32 b) { st_cmd(0xB0); write_backup(0x0000, b); }
static void st_prog(u32 bank, u32 a, u8 v)
{
  st_bank(bank);
  st_cmd(0xA0);
  write_backup(a, v);
  st_model[bank * BANK_BYTES + a] = v;
}
static void st_check_read(const char *what)
{
  for (u32 b = 0; b < 2; b++) {
    st_bank(b);
    for (u32 a = 0; a < BANK_BYTES; a++)
      if (read_backup(a) != st_model[b * BANK_BYTES + a]) {
        printf("SELFTEST FAIL %s: bank %lu addr %05lx read %02x model %02x\n", what,
               (unsigned long)b, (unsigned long)a, read_backup(a), st_model[b * BANK_BYTES + a]);
        st_fail++;
        return;
      }
  }
  printf("SELFTEST ok  %s: both banks read back\n", what);
}
static void st_check_export(const char *dir, const char *what)
{
  char p[256];
  snprintf(p, sizeof p, "%s/export.sav", dir);
  FILE *f = fopen(p, "wb");
  if (!f || !gba_backup_export(f)) { printf("SELFTEST FAIL %s: export\n", what); st_fail++; if (f) fclose(f); return; }
  fclose(f);
  f = fopen(p, "rb");
  static u8 img[2 * BANK_BYTES];
  size_t n = fread(img, 1, sizeof img, f);
  fclose(f);
  if (n != sizeof img || memcmp(img, st_model, sizeof img)) {
    u32 i = 0;
    while (i < n && img[i] == st_model[i]) i++;
    printf("SELFTEST FAIL %s: exported image (%lu bytes) differs at %05lx\n", what, (unsigned long)n, (unsigned long)i);
    st_fail++;
    return;
  }
  printf("SELFTEST ok  %s: exported 128 KB image matches\n", what);
}

int gba_backup_selftest(const char *dir)
{
  char save[256], p[256];
  snprintf(save, sizeof save, "%s/test.sav", dir);
  /* 1. a 1 Mbit save with two different banks, imported */
  for (u32 i = 0; i < 2 * BANK_BYTES; i++)
    st_model[i] = i < BANK_BYTES ? (u8)((i * 13) ^ 0x5A) : (u8)((i * 7) + 3);
  FILE *f = fopen(save, "wb"); fwrite(st_model, 1, sizeof st_model, f); fclose(f);
  backup_type_reset = BACKUP_FLASH; flash_bank_cnt = FLASH_SIZE_128KB;   /* as load_gamepak detects it */
  gba_backup_reset(save);
  f = fopen(save, "rb");
  u32 got = gba_backup_import(f, GBA_BACKUP_IMAGE_BYTES);
  fclose(f);
  if (got != GBA_BACKUP_IMAGE_BYTES) { printf("SELFTEST FAIL import read %lu\n", (unsigned long)got); st_fail++; }
  backup_type = BACKUP_FLASH; flash_bank_cnt = FLASH_SIZE_128KB;
  flash_mode = FLASH_BASE_MODE; flash_command_position = 0; flash_bank_num = 0;
  st_check_read("import");
  st_check_export(dir, "import");
  /* 2. programs alternating between the banks, a sector erase in each */
  for (u32 k = 0; k < 600; k++)
    st_prog(k & 1, (k * 977) % BANK_BYTES, (u8)(k * 31));
  st_bank(1); st_cmd(0x80); write_backup(0x5555, 0xAA); write_backup(0x2AAA, 0x55); write_backup(0x3000, 0x30);
  memset(&st_model[BANK_BYTES + 0x3000], 0xFF, 0x1000);
  st_bank(0); st_cmd(0x80); write_backup(0x5555, 0xAA); write_backup(0x2AAA, 0x55); write_backup(0xF000, 0x30);
  memset(&st_model[0xF000], 0xFF, 0x1000);
  st_check_read("program+erase");
  st_check_export(dir, "program+erase");
  /* 3. the exported image imported again (a reload) */
  snprintf(p, sizeof p, "%s/export.sav", dir);
  gba_backup_reset(save);
  f = fopen(p, "rb"); gba_backup_import(f, GBA_BACKUP_IMAGE_BYTES); fclose(f);
  flash_bank_num = 1;                 /* a savestate taken in bank 1: the access selects it */
  st_check_read("reload");
  /* 4. chip erase from bank 1 */
  st_bank(1); st_cmd(0x80); st_cmd(0x10);
  memset(st_model, 0xFF, sizeof st_model);
  st_check_read("chip erase");
  st_check_export(dir, "chip erase");
  /* 5. a short (64 KB, one bank) save: the rest 0xFF, no bank file needed */
  for (u32 i = 0; i < BANK_BYTES; i++) st_model[i] = (u8)(i ^ 0xC3);
  memset(st_model + BANK_BYTES, 0xFF, BANK_BYTES);
  f = fopen(save, "wb"); fwrite(st_model, 1, BANK_BYTES, f); fclose(f);
  gba_backup_reset(save);
  f = fopen(save, "rb"); got = gba_backup_import(f, GBA_BACKUP_IMAGE_BYTES); fclose(f);
  printf("SELFTEST %s short save: read %lu bytes, bank-1 file %s\n", got == BANK_BYTES ? "ok  " : "FAIL",
         (unsigned long)got, bank_valid[1] ? "written (unexpected)" : "not needed");
  if (got != BANK_BYTES || bank_valid[1]) st_fail++;
  flash_bank_num = 0;
  st_check_read("short save");
  st_check_export(dir, "short save");
  /* 6. a Pokemon-like save after a load: erase then program each sector of
   * slot B (sectors 14-27: bank 0 14-15, bank 1 0-11). Erase first: no
   * sector is read from a file. */
  for (u32 i = 0; i < 2 * BANK_BYTES; i++)
    st_model[i] = (u8)(i * 29 + 1);
  f = fopen(save, "wb"); fwrite(st_model, 1, sizeof st_model, f); fclose(f);
  gba_backup_reset(save);
  f = fopen(save, "rb"); gba_backup_import(f, GBA_BACKUP_IMAGE_BYTES); fclose(f);
  st_n_fetch = st_n_store = 0;
  for (u32 sec = 14; sec < 28; sec++) {
    u32 bank = sec >> 4, base = (sec & 15) * 0x1000;
    st_bank(bank); st_cmd(0x80); write_backup(0x5555, 0xAA); write_backup(0x2AAA, 0x55); write_backup(base, 0x30);
    memset(&st_model[bank * BANK_BYTES + base], 0xFF, 0x1000);
    for (u32 a = 0; a < 0x1000; a += 61)
      st_prog(bank, base + a, (u8)(a ^ sec));
  }
  /* bank 0 went to its file at the import: the save itself writes nothing
   * out (the slots it takes are clean) and reads nothing (erase first) */
  printf("SELFTEST %s save sequence: %lu sector reads, %lu sector writes (want 0, 0)\n",
         (st_n_fetch || st_n_store) ? "FAIL" : "ok  ", (unsigned long)st_n_fetch, (unsigned long)st_n_store);
  if (st_n_fetch || st_n_store) st_fail++;
  st_check_export(dir, "save sequence");
  st_check_read("save sequence");
  printf("SELFTEST %s (%d failures)\n", st_fail ? "FAILED" : "PASSED", st_fail);
  return st_fail ? 1 : 0;
}
#endif
