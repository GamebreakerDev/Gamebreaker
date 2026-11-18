/* gb_log.h - the log for bug reports (logs\game.log; gb_save.c).
 *
 * gb_log() writes one line that a player's log keeps: the version, the
 * settings in effect, the graphics card, errors, and how the game ended.
 * Everything else the program writes goes to stderr, which a player's build
 * does not keep unless whoever started the program redirected it. */
#pragma once
#include <stddef.h>

void gb_log(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* path with the profile folders written as %LOCALAPPDATA%, %APPDATA% or
 * %USERPROFILE%, so a log pasted into a public report does not carry the
 * Windows user name. Returns out. */
const char *gb_log_user_path(const char *path, char *out, size_t size);

/* The first lines of the log: version, compiler, Windows, processor, and the
 * player settings that are set. */
void gb_log_header(void);
