/* Mods: the player's replacement files, read in place of the disc's
 * (gb_mods.c). */
#ifndef GB_MODS_H
#define GB_MODS_H

#include <stdint.h>
#include <stddef.h>   /* wchar_t */

void gb_mods_init(void);                      /* main.c, after gb_save_init */

/* recomp_manual.c: the open calls' wrappers while a mod is on, else NULL. */
void (*gb_mods_lookup(uint32_t xbox_va))(void);

/* The mods in use, in load order (the first wins a clash); none when there
 * is no mods folder or GB_MODS=0. Valid after gb_mods_init. */
int gb_mods_count(void);
const char *gb_mods_name(int i);              /* the folder's name, UTF-8, as in the log */
const wchar_t *gb_mods_folder(int i, wchar_t *out, int cap);  /* full path; NULL out of range */

#endif
