/* gb_sticky.c - Windows' Sticky Keys shortcut while the game is in front.
 *
 * Pressing Shift five times opens Windows' "Sticky Keys" box, which takes
 * the keyboard from the game; a turbo key on Shift, tapped for dives and
 * sprints, can do that in a match. So while the game's window has the
 * keyboard, the shortcut (the hotkey flag of Windows' Sticky Keys setting)
 * is switched off, and it is switched back on when the window loses the
 * keyboard and when the game ends. Nothing else is touched:
 *   - when Sticky Keys itself is on, the player uses it, and it is left alone;
 *   - when the shortcut is already off, nothing is changed;
 *   - Filter Keys and Toggle Keys are not touched.
 * The change is made for this sign-in only (never written to the player's
 * profile), so it cannot outlive a restart of Windows.
 *
 * A game that ends without passing through its exit (a crash, a killed
 * process) would leave the shortcut off until the player signs out. So the
 * setting to come back to is written to a small file beside the program
 * before the shortcut is switched off, and the next start puts it back. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_log.h"

void gb_sticky_window(int background);
void gb_sticky_focus(int in_front);
void gb_sticky_shutdown(void);


typedef BOOL (*sk_get_fn)(DWORD *flags);
typedef BOOL (*sk_set_fn)(DWORD flags);

static BOOL real_get(DWORD *flags)
{
    STICKYKEYS sk;
    memset(&sk, 0, sizeof sk);
    sk.cbSize = sizeof sk;
    if (!SystemParametersInfoA(SPI_GETSTICKYKEYS, sizeof sk, &sk, 0))
        return FALSE;
    *flags = sk.dwFlags;
    return TRUE;
}

static BOOL real_set(DWORD flags)
{
    STICKYKEYS sk;
    memset(&sk, 0, sizeof sk);
    sk.cbSize = sizeof sk;
    sk.dwFlags = flags;
    /* fWinIni 0: for this sign-in only, not saved to the profile. */
    return SystemParametersInfoA(SPI_SETSTICKYKEYS, sizeof sk, &sk, 0);
}

static sk_get_fn s_get = real_get;
static sk_set_fn s_set = real_set;
static char      s_marker[MAX_PATH];   /* <program>.stickykeys: the flags to come back to */
static int       s_enabled;            /* the window is shown and may change the setting */
static int       s_off;                /* this run switched the shortcut off */
static int       s_said;               /* bit per log line already written this run */
static CRITICAL_SECTION s_lock;
static LONG      s_lock_ready;

static void lock_init(void)
{
    if (!InterlockedExchange(&s_lock_ready, 1))
        InitializeCriticalSection(&s_lock);
}

static void say_once(int bit, const char *line)
{
    if (s_said & bit)
        return;
    s_said |= bit;
    gb_log("[GB] Sticky Keys: %s\n", line);
}

/* Written through a temporary name, so a crash never leaves half a number. */
static int marker_write(DWORD flags)
{
    char tmp[MAX_PATH + 4], text[16];
    HANDLE f;
    DWORD n = 0;
    int len = snprintf(text, sizeof text, "%08lX\n", (unsigned long)flags);
    BOOL ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", s_marker);
    f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(f, text, (DWORD)len, &n, NULL) && n == (DWORD)len;
    CloseHandle(f);
    return ok && MoveFileExA(tmp, s_marker, MOVEFILE_REPLACE_EXISTING);
}

static int marker_read(DWORD *flags)
{
    char text[16];
    DWORD n = 0;
    char *end;
    unsigned long v;
    HANDLE f = CreateFileA(s_marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(f, text, sizeof text - 1, &n, NULL))
        n = 0;
    CloseHandle(f);
    text[n] = 0;
    v = strtoul(text, &end, 16);
    if (end == text)
        return 0;
    *flags = (DWORD)v;
    return 1;
}

/* The shortcut back on, if it is off now. Returns 1 when it is on. */
static int shortcut_on(void)
{
    DWORD now = 0;
    if (!s_get(&now))
        return 0;
    if (now & SKF_HOTKEYACTIVE)
        return 1;
    return s_set(now | SKF_HOTKEYACTIVE) != 0;
}

/* A run that ended with the shortcut off left its file: put it back. */
static void repair(void)
{
    DWORD was = 0;
    if (!s_marker[0] || GetFileAttributesA(s_marker) == INVALID_FILE_ATTRIBUTES)
        return;
    if (marker_read(&was) && (was & SKF_HOTKEYACTIVE)) {
        DWORD now = 0;
        int was_off = s_get(&now) && !(now & SKF_HOTKEYACTIVE);
        if (!shortcut_on()) {
            gb_log("[GB] Sticky Keys: the shortcut could not be put back on\n");
            return;                      /* the file stays, for the next start */
        }
        if (was_off)
            gb_log("[GB] Sticky Keys: the shortcut was left off by a game that did not close normally; "
                   "it is back on\n");
    }
    DeleteFileA(s_marker);
}

/* The window is shown. `background`: a scripted test run, which never
 * changes the player's setting (or repairs it). */
void gb_sticky_window(int background)
{
    DWORD n;
    lock_init();
    EnterCriticalSection(&s_lock);
    n = GetModuleFileNameA(NULL, s_marker, MAX_PATH - 16);
    if (!n || n >= MAX_PATH - 16)
        s_marker[0] = 0;
    else
        strcat(s_marker, ".stickykeys");
    s_enabled = !background;
    if (s_enabled) {
        repair();
        atexit(gb_sticky_shutdown);
    }
    LeaveCriticalSection(&s_lock);
}

void gb_sticky_focus(int in_front)
{
    DWORD now = 0;
    lock_init();
    EnterCriticalSection(&s_lock);
    if (!s_enabled) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    if (in_front && !s_off) {
        if (!s_get(&now))
            say_once(1, "the setting cannot be read; nothing changed");
        else if (now & SKF_STICKYKEYSON)
            say_once(2, "Sticky Keys is on: left as it is");
        else if (!(now & SKF_HOTKEYACTIVE))
            say_once(4, "the shortcut is already off: nothing changed");
        else if (!s_marker[0] || !marker_write(now))
            say_once(8, "no file to come back from: nothing changed");
        else if (s_set(now & ~(DWORD)SKF_HOTKEYACTIVE)) {
            s_off = 1;
            say_once(16, "the Shift shortcut is off while the game is in front");
        } else {
            DeleteFileA(s_marker);
            say_once(32, "the shortcut could not be switched off; nothing changed");
        }
    } else if (!in_front && s_off) {
        if (shortcut_on()) {
            s_off = 0;
            DeleteFileA(s_marker);
            say_once(64, "the Shift shortcut is back on while the game is not in front");
        }
    }
    LeaveCriticalSection(&s_lock);
}

/* The shortcut back before the program ends. Safe to call more than once,
 * from any thread. */
void gb_sticky_shutdown(void)
{
    lock_init();
    EnterCriticalSection(&s_lock);
    if (s_off && shortcut_on()) {
        s_off = 0;
        DeleteFileA(s_marker);
    }
    LeaveCriticalSection(&s_lock);
}

