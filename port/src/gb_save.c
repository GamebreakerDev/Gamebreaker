/*
 * Save files, the game's log, and how the program ends.
 *
 * The title keeps one save, U:\387E8D371AC2\nbastreet2.sav, written by its
 * own code (guest 0x000D78F0). The toolkit maps U: to <user root>\UDATA\
 * 45410027, where the user root is %LOCALAPPDATA%\xboxrecomp unless
 * RECOMP_SAVE_DIR names another folder. That default is shared by every
 * program built on the toolkit: every build and install of this port, and
 * other ports, would read and overwrite the same save. This file gives the
 * port a save folder of its own and makes it safe:
 *
 *   GB_SAVE_DIR=<folder>  where saves go. Unset: "saves" in the folder that
 *                         holds game_files (a launcher install passes
 *                         <install>\saves).
 *   GB_SAVE_BACKUPS=<n>   at start, copy the save to backups\<date time>\ in
 *                         the save folder when it differs from the newest
 *                         copy, and keep the last n (default 5; 0 = none).
 *   The write guard       closing the window while the game writes its save
 *                         waits for the write to finish (up to 10 s), so the
 *                         file is never left half written.
 *   GB_UNLOCK_ALL=1       also copy the save to backups\<date time>_before-unlock\
 *                         (kept outside the rotation), before "Unlock
 *                         everything" (recomp_manual.c) changes it.
 *   The log               logs\game.log beside game_files (GB_LOG_DIR to put
 *                         it elsewhere), the previous one kept as
 *                         game.prev.log; see "the log" below.
 *   The fatal error       when the game ends on its own fatal error, a message
 *                         box shows the game's error text and the log's path.
 */
/*
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>         /* _get_osfhandle */

#include "gb_save.h"
#include "gb_log.h"

void recomp_exit_hook_set(void (*fn)(const char *why));   /* toolkit, src/platform */
void xbox_missing_summary(const char *why);                /* toolkit, src/kernel */

#define GB_TITLE_DIR   "UDATA\\45410027"
#define GB_SAVE_SUBDIR "387E8D371AC2"       /* XCreateSaveGame's folder for "NBA STREET Vol. 2 Save" */
#define GB_SAVE_FILE   "nbastreet2.sav"

static char s_root[MAX_PATH];               /* the user root: holds UDATA */
static char s_log[MAX_PATH];                /* the log's path, if known */

/* ── small helpers ─────────────────────────────────────────── */

static void mkdirs(const char *path)
{
    char tmp[MAX_PATH];
    size_t n = strlen(path);
    if (n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 3; i < n; i++) {
        if (tmp[i] == '\\' || tmp[i] == '/') {
            char c = tmp[i];
            tmp[i] = 0;
            CreateDirectoryA(tmp, NULL);
            tmp[i] = c;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static int is_dir(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_file(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Copy the files of one folder (no subfolders: a save folder has none). */
static int copy_files(const char *from, const char *to)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    HANDLE f;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", from);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return -1;
    mkdirs(to);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        snprintf(a, sizeof a, "%s\\%s", from, fd.cFileName);
        snprintf(b, sizeof b, "%s\\%s", to, fd.cFileName);
        if (CopyFileA(a, b, FALSE))
            n++;
        else
            n = -1000;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return n;
}

static void remove_files_and_dir(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH], p[MAX_PATH];
    HANDLE f;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;               /* backups hold files only; leave anything else */
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            DeleteFileA(p);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryA(dir);
}

/* 1 when the two files have the same bytes. */
static int same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = fa && fb;
    while (same) {
        char x[4096], y[4096];
        size_t m = fread(x, 1, sizeof x, fa), n = fread(y, 1, sizeof y, fb);
        if (m != n || memcmp(x, y, m))
            same = 0;
        if (m == 0)
            break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* ── the log ───────────────────────────────────────────────── */

/* logs\game.log is for bug reports. gb_log() writes the lines that belong
 * there: the version, the settings in effect, the graphics card, errors and
 * how the game ended. Everything else the program and the toolkit write goes
 * to stderr (several thousand lines a run).
 *
 * A player's build keeps game.log to the gb_log() lines and sends stderr
 * nowhere, unless whoever started the program redirected it
 * (gamebreaker.exe 2> run.txt): then stderr is left alone and gb_log()
 * writes there as well, so the full output is one redirection away. */
static FILE *s_short;                       /* game.log, when it is not stderr */

void gb_log_init(void)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD type = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    const char *dir = getenv("GB_LOG_DIR");
    char d[MAX_PATH], prev[MAX_PATH];

    if (type == FILE_TYPE_DISK) {           /* redirected by whoever started it: name it, change nothing */
        if (!GetFinalPathNameByHandleA(h, s_log, sizeof s_log, FILE_NAME_NORMALIZED))
            s_log[0] = 0;
        else if (!strncmp(s_log, "\\\\?\\", 4))
            memmove(s_log, s_log + 4, strlen(s_log + 4) + 1);
        return;
    }
    if (type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR)
        return;                             /* someone reads it live */
    if (dir && dir[0])
        snprintf(d, sizeof d, "%s", dir);
    else {
        GetCurrentDirectoryA(sizeof d, d);  /* main.c has moved to the folder holding game_files */
        strncat(d, "\\logs", sizeof d - strlen(d) - 1);
    }
    mkdirs(d);
    snprintf(s_log, sizeof s_log, "%s\\game.log", d);
    snprintf(prev, sizeof prev, "%s\\game.prev.log", d);
    if (is_file(s_log))
        MoveFileExA(s_log, prev, MOVEFILE_REPLACE_EXISTING);
    s_short = fopen(s_log, "w");
    if (!s_short)
        s_log[0] = 0;
    if (freopen("NUL", "w", stderr))
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
}

void gb_log(const char *fmt, ...)
{
    va_list ap;
    if (s_short) {
        va_start(ap, fmt);
        vfprintf(s_short, fmt, ap);
        va_end(ap);
        fflush(s_short);                    /* a few dozen lines a run: never lose the last */
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

const char *gb_log_user_path(const char *path, char *out, size_t size)
{
    static const char *const vars[] = { "LOCALAPPDATA", "APPDATA", "USERPROFILE" };
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA(vars[i], base, sizeof base);
        if (n && n < sizeof base && !_strnicmp(path, base, n) && (path[n] == '\\' || path[n] == 0)) {
            snprintf(out, size, "%%%s%%%s", vars[i], path + n);
            return out;
        }
    }
    snprintf(out, size, "%s", path);
    return out;
}

/* The first lines of every log: what a bug report needs to know about the
 * program, the PC and the settings. */
void gb_log_header(void)
{
    static const char *const player[] = {
        "GB_D3D11_FULLSCREEN", "GB_D3D11_MONITOR", "GB_D3D11_RES", "GB_D3D11_VSYNC",
        "GB_ASPECT", "GB_ASPECT_2D", "GB_D3D11_ANISO", "GB_D3D11_MSAA", "GB_D3D11_ADAPTER",
        "GB_SHOW_FPS", "GB_GAME_HZ", "GB_SKIP_INTRO", "GB_PAUSE_BACKGROUND", "GB_RUMBLE",
        "GB_SONG_ROTATION", "GB_AUTO_REPLAY", "GB_DEMO_DELAY", "GB_AUDIO_VOLUME",
        "GB_VOL_MUSIC", "GB_VOL_SFX", "GB_VOL_CROWD", "GB_VOL_PLAYERS", "GB_VOL_COMMENTARY",
        "GB_COMMENTARY", "GB_AUDIO_MONO", "GB_AUDIO_TICK", "GB_AUDIO_LEAD_MS",
        "GB_AUDIO_WAIT", "GB_AUDIO_GUARD", "GB_PADS", "GB_PAD_PRESET", "GB_PAD_TRIGGER",
        "GB_PAD_RUMBLE", "GB_PAD_ORDER", "GB_PAD_KEYBOARD_PLAYER", "GB_PAD_BACKGROUND",
        "GB_PAD_DEADZONE", "GB_CHEATS", "GB_UNLOCK_ALL", "GB_MODS", "GB_MODS_DIR",
    };
    char line[1024], cpu[128] = "unknown processor";
    int at = 0;
    SYSTEMTIME t;
    OSVERSIONINFOW v = { sizeof v };
    LONG (WINAPI *rtl_get_version)(OSVERSIONINFOW *) =
        (LONG (WINAPI *)(OSVERSIONINFOW *))(void (*)(void))GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    DWORD size = sizeof cpu;

    GetLocalTime(&t);
#if defined(__clang__)
    gb_log("[GB] Gamebreaker %s (clang %d.%d), started %04u-%02u-%02u %02u:%02u:%02u\n", GB_VERSION_STR,
           __clang_major__, __clang_minor__, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
#else
    gb_log("[GB] Gamebreaker %s (MSVC %d), started %04u-%02u-%02u %02u:%02u:%02u\n", GB_VERSION_STR,
           _MSC_VER, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
#endif
    if (rtl_get_version)
        rtl_get_version(&v);
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                 "ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &size);
    for (size_t n = strlen(cpu); n > 0 && cpu[n - 1] == ' '; n--)
        cpu[n - 1] = 0;                     /* the registry pads it with spaces */
    gb_log("[GB] Windows %lu.%lu.%lu; %s, %lu logical processors\n",
           (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber,
           cpu, (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    at = snprintf(line, sizeof line, "[GB] settings:");
    for (size_t i = 0; i < sizeof player / sizeof player[0] && at > 0 && at < (int)sizeof line - 2; i++) {
        const char *e = getenv(player[i]);
        if (e && e[0])
            at += snprintf(line + at, sizeof line - (size_t)at, " %s=%s", player[i], e);
    }
    if (at == (int)strlen("[GB] settings:"))
        snprintf(line + at, sizeof line - (size_t)at, " the program's defaults");
    gb_log("%s\n", line);
}

const char *gb_log_path(void)
{
    return s_log;
}

/* ── the save folder and its backups ──────────────────────── */

static void gb_save_backup(void)
{
    const char *e = getenv("GB_SAVE_BACKUPS");
    int keep = e && e[0] ? atoi(e) : 5;
    char save[MAX_PATH], file[MAX_PATH], bdir[MAX_PATH], pat[MAX_PATH], newest[MAX_PATH] = "";
    char names[64][32];
    int count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE f;

    if (keep <= 0) {
        return;
    }
    snprintf(save, sizeof save, "%s\\" GB_TITLE_DIR "\\" GB_SAVE_SUBDIR, s_root);
    snprintf(file, sizeof file, "%s\\" GB_SAVE_FILE, save);
    snprintf(bdir, sizeof bdir, "%s\\backups", s_root);
    if (!is_file(file)) {
        return;
    }
    /* Backup folders are named yyyy-mm-dd_hhmmss, so the names sort by age. */
    snprintf(pat, sizeof pat, "%s\\????-??-??_??????", bdir);
    f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < 64)
                snprintf(names[count++], sizeof names[0], "%s", fd.cFileName);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    qsort(names, (size_t)count, sizeof names[0], (int (*)(const void *, const void *))strcmp);
    if (count) {
        snprintf(newest, sizeof newest, "%s\\%s\\" GB_SAVE_FILE, bdir, names[count - 1]);
        if (same_file(file, newest)) {
            return;
        }
    }
    {
        SYSTEMTIME t;
        char to[MAX_PATH];
        int n;
        GetLocalTime(&t);
        snprintf(names[count < 64 ? count : 63], sizeof names[0], "%04u-%02u-%02u_%02u%02u%02u",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        snprintf(to, sizeof to, "%s\\%s", bdir, names[count < 64 ? count : 63]);
        n = copy_files(save, to);
        if (n <= 0)
            gb_log("[SAVE] backups: could not copy the save to the backups folder\n");
        if (n > 0 && count < 64)
            count++;
    }
    for (int i = 0; i < count - keep; i++) {
        char old[MAX_PATH];
        snprintf(old, sizeof old, "%s\\%s", bdir, names[i]);
        remove_files_and_dir(old);
    }
}

/* A copy of the save kept outside the rotation above (its folder name does
 * not match yyyy-mm-dd_hhmmss): backups\<date time>_<why>\. Made before a
 * start that changes the save on purpose (GB_UNLOCK_ALL), so restoring it
 * undoes that change. Nothing is copied when there is no save yet. */
static void gb_save_backup_named(const char *why)
{
    char save[MAX_PATH], file[MAX_PATH], to[MAX_PATH];
    SYSTEMTIME t;
    int n;

    snprintf(save, sizeof save, "%s\\" GB_TITLE_DIR "\\" GB_SAVE_SUBDIR, s_root);
    snprintf(file, sizeof file, "%s\\" GB_SAVE_FILE, save);
    if (!is_file(file)) {
        return;
    }
    GetLocalTime(&t);
    snprintf(to, sizeof to, "%s\\backups\\%04u-%02u-%02u_%02u%02u%02u_%s", s_root,
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, why);
    n = copy_files(save, to);
    (void)n;
}

/* For gb_mods.c: the same copy, saying whether there was a save to copy. */
int gb_save_backup_tagged(const char *why)
{
    char file[MAX_PATH];

    snprintf(file, sizeof file, "%s\\" GB_TITLE_DIR "\\" GB_SAVE_SUBDIR "\\" GB_SAVE_FILE, s_root);
    if (!is_file(file))
        return 0;
    gb_save_backup_named(why);
    return 1;
}

void gb_save_init(void)
{
    const char *gs = getenv("GB_SAVE_DIR");
    const char *how;
    char shown[MAX_PATH];

    {
        char want[MAX_PATH];
        if (gs && gs[0])
            snprintf(want, sizeof want, "%s", gs);
        else
            snprintf(want, sizeof want, "saves");
        GetFullPathNameA(want, sizeof s_root, s_root, NULL);
        mkdirs(s_root);
        /* The toolkit moves UDATA and TDATA there; its partition images stay
         * in its own folder (kernel_path.c, RECOMP_SAVE_DIR). */
        _putenv_s("RECOMP_SAVE_DIR", s_root);
        how = gs && gs[0] ? "GB_SAVE_DIR" : "the default, beside game_files";

        /* A save made before this change sits in the shared folder: copy it
         * across once (it is left where it was). Not for a folder the caller
         * chose, such as a test's own. */
        if (!(gs && gs[0])) {
            char base[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
            DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
            if (n && n < sizeof base) {
                snprintf(from, sizeof from, "%s\\xboxrecomp\\" GB_TITLE_DIR "\\" GB_SAVE_SUBDIR, base);
                snprintf(to, sizeof to, "%s\\" GB_TITLE_DIR "\\" GB_SAVE_SUBDIR, s_root);
                if (is_dir(from) && !is_dir(to)) {
                    int c = copy_files(from, to);
                    gb_log("[SAVE] copied the save from the shared folder %s (%d files)\n",
                           gb_log_user_path(from, shown, sizeof shown), c);
                }
            }
        }
    }
    gb_log("[SAVE] save folder: %s\\" GB_TITLE_DIR "\\" GB_SAVE_SUBDIR " (%s)\n",
           gb_log_user_path(s_root, shown, sizeof shown), how);
    gb_save_backup();
    {
        /* "Unlock everything" (recomp_manual.c) writes every reward bit into
         * the profile, which the game's next save makes permanent: keep the
         * save as it was first. */
        const char *u = getenv("GB_UNLOCK_ALL");
        if (u && u[0] == '1')
            gb_save_backup_named("before-unlock");
    }
}

const char *gb_save_root(void)
{
    return s_root;
}

/* ── the write guard ──────────────────────────────────────── */

static volatile LONG s_writing;
static HANDLE s_idle;                       /* manual reset; set while no write runs */

static void gb_save_exit_hook(const char *why)
{
    gb_save_before_exit(why);
    xbox_missing_summary(why);              /* what the toolkit's own hook did */
}

void gb_save_write_begin(void)
{
    if (!s_idle) {
        HANDLE e = CreateEventA(NULL, TRUE, TRUE, NULL);
        if (InterlockedCompareExchangePointer((PVOID volatile *)&s_idle, e, NULL) != NULL)
            CloseHandle(e);
    }
    /* Both windows (gb_d3d11.c and the toolkit's own) run this hook before
     * they end the process. The toolkit sets it once, lazily, to its
     * missing-file summary; taking it over here, at every write, keeps the
     * summary and adds the wait. */
    recomp_exit_hook_set(gb_save_exit_hook);
    if (InterlockedIncrement(&s_writing) == 1)
        ResetEvent(s_idle);
}

void gb_save_write_end(void)
{
    if (InterlockedDecrement(&s_writing) == 0)
        SetEvent(s_idle);
}

int gb_save_writing(void)
{
    return s_writing > 0;
}

void gb_save_before_exit(const char *why)
{
    if (s_writing > 0 && s_idle) {
        DWORD t0 = GetTickCount(), r;
        gb_log("[SAVE] %s closed during a save: waiting for the write to finish\n", why);
        fflush(stderr);
        r = WaitForSingleObject(s_idle, 10000);
        gb_log("[SAVE] %s after %lu ms\n",
               r == WAIT_OBJECT_0 ? "write finished" : "GAVE UP waiting for the write",
               (unsigned long)(GetTickCount() - t0));
    }
    fflush(stderr);
}

/* ── the game's fatal error ───────────────────────────────── */

void gb_fatal_report(const char *text)
{
    char msg[1200];

    gb_log("[GB] the game stopped on its own fatal error: %s\n", text);
    fflush(stderr);
    snprintf(msg, sizeof msg,
             "The game stopped on an error.\n\n%s\n\n%s%s",
             text && text[0] ? text : "(no message)",
             s_log[0] ? "Log: " : "No log was kept.",
             s_log[0] ? s_log : "");
    MessageBoxA(NULL, msg, "Gamebreaker", MB_ICONERROR | MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
}
