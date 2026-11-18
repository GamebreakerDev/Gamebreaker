/* Save folder, backups, write guard, log file and fatal-error message
 * (gb_save.c). */
#ifndef GB_SAVE_H
#define GB_SAVE_H

void gb_log_init(void);                     /* main.c, once in the folder holding game_files */
const char *gb_log_path(void);              /* "" when the log goes nowhere known */

void gb_save_init(void);                    /* main.c, before xbox_path_init */
const char *gb_save_root(void);             /* the folder holding UDATA */

void gb_save_write_begin(void);             /* the save writer's wrapper (recomp_manual.c) */
void gb_save_write_end(void);
int  gb_save_writing(void);                 /* 1 while the save writer runs */
void gb_save_before_exit(const char *why);  /* a window is about to end the process */

void gb_fatal_report(const char *text);     /* the game's fatal error: log it, show the box */

#endif
