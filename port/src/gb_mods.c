/*
 * Mods: the player's own replacement files, read in place of the disc's.
 *
 * A mod is a folder in the mods folder whose files mirror the disc's paths:
 *
 *   mods\<mod name>\fe\HUD.XSH          replaces D:\fe\HUD.XSH
 *   mods\<mod name>\database\xdbplyr.adf replaces D:\database\xdbplyr.adf
 *
 * One entry of one of the disc's BIG archives (the xf\ files, the courts'
 * .viv files) is replaced by a file in a folder named like the archive:
 *
 *   mods\<mod name>\xf\forest.viv\foresto.xsh   entry foresto.xsh
 *   mods\<mod name>\xf\xf000002\417.xsh         entry 417, counted from 0
 *                                               (the name before the first
 *                                               dot; entries there have no
 *                                               names)
 *
 * One item of one of the disc's C0FB packs (the menus' pictures, fonts and
 * layouts in fe\*.bin, the text in locale\*.eus) the same way, and one line
 * of a text entry as a UTF-8 file named by its number:
 *
 *   mods\<mod name>\fe\xafescrn.bin\0.shpx                  picture entry 0
 *   mods\<mod name>\locale\xloc_fe.eus\mainmenu.txt\1.txt   line 1 of the
 *                                                            entry mainmenu.txt
 *
 *   Package files  manifest.json, icon.png, README.md and the like at a mod's
 *                  top are for people and mod managers; the game skips them.
 *   Unknown files  a file the disc has no place for is left out, with a line
 *                  in the log saying so.
 *
 *   The folder     GB_MODS_DIR=<folder>, or "mods" beside the save folder
 *                  (a launcher install: <install>\mods).
 *   Which mods     mods\load_order.txt, one folder name a line, first line
 *                  first: a file in an earlier mod wins over the same file in
 *                  a later one. A line starting with # is off, and so is a
 *                  folder the file does not list. Without the file every
 *                  folder is on, in name order.
 *   GB_MODS=0      every mod off.
 *   Missing file   the disc's file is read, as without mods: a mod changes
 *                  only the files it has. An empty file is not used, and a
 *                  file that does not open falls back to the disc's.
 *   The log        logs\game.log names the mods in use and each replaced file
 *                  the first time the game reads it, so a bug report shows
 *                  them.
 *   Saves          the save folder is never redirected. When the mods in use
 *                  differ from the last start, the save is copied to
 *                  backups\<date time>_mods-changed\ first (gb_save.c),
 *                  because a mod's data (a roster, say) can end up in what
 *                  the game saves.
 *
 * With no mods folder, or none on, nothing here runs: the open calls are not
 * wrapped and no file is written.
 *
 * How: the title opens files through two kernel imports, NtCreateFile and
 * NtOpenFile. For a read of a disc path that a mod replaces, the wrapper
 * hands the toolkit the path relative to the mod's folder with that folder's
 * handle as the root directory (the toolkit's path for an open relative to a
 * directory), then puts the caller's structures back as they were. An
 * archive with replaced entries is first built once as a merged copy in
 * mods\.merged\ (see "single entries" below) and opened from there.
 *
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <xbox/xboxrecomp.h>   /* RECOMP_TLS */
#include "gb_log.h"
#include "gb_save.h"
#include "gb_mods.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_esp;
extern RECOMP_TLS uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

#define MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define MEM16(va) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define MEM8P(va) ((const char *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* Import slots (.rdata) of the two calls every file open goes through. */
#define GB_KTHUNK_NTOPENFILE   0x0022ACD8u
#define GB_KTHUNK_NTCREATEFILE 0x0022AD2Cu

#define MAX_MODS  64
#define MAX_FILES 65536
#define NAME_LEN  64

typedef struct {
    char   name[NAME_LEN];      /* folder name, UTF-8, for the log */
    HANDLE root;                /* the folder, open for the life of the program */
    int    files;
} gb_mod;

typedef struct {
    char         *path;         /* relative, lower case, backslashes */
    int           mod;          /* index in s_mods, or MERGED */
    volatile LONG told;         /* logged its first read */
} gb_mod_file;

static gb_mod      s_mods[MAX_MODS];
static int         s_nmods;
static WCHAR       s_dir[MAX_PATH];     /* the mods folder */
static WCHAR       s_disc[MAX_PATH];    /* the disc's files: <working folder>\game_files */
static HANDLE      s_merged_root;       /* mods\.merged, when an archive was merged */
static gb_mod_file *s_tab;      /* open addressing, s_cap a power of two */
static uint32_t    s_cap;
static int         s_nfiles;
static volatile LONG s_lines;   /* log lines about single files, capped */

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static gb_mod_file *find(const char *rel)
{
    uint32_t i;
    if (!s_tab)
        return NULL;
    for (i = hash(rel) & (s_cap - 1); s_tab[i].path; i = (i + 1) & (s_cap - 1))
        if (!strcmp(s_tab[i].path, rel))
            return &s_tab[i];
    return NULL;
}

static int log_line_ok(void)
{
    return InterlockedIncrement(&s_lines) <= 200;
}

/* ── the scan ─────────────────────────────────────────────── */

/* A whole file (entry NULL), or one entry of an archive on the disc: rel is
 * then the archive's path and entry the name the mod gave the entry. */
typedef struct { char *rel; char *entry; int mod; } pending;
static pending *s_pend, *s_epend;
static int      s_npend, s_cappend, s_nepend, s_capepend;

/* What the disc has at a relative path: 0 nothing, 1 a file, 2 a folder. */
static int disc_kind(const char *rel)
{
    WCHAR p[MAX_PATH], r[MAX_PATH];
    DWORD a;

    if (MultiByteToWideChar(CP_UTF8, 0, rel, -1, r, MAX_PATH) <= 0)
        return 0;
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\%s", s_disc, r);
    a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
}

/* Files a mod package keeps at its top for people and mod managers
 * (Thunderstore's manifest.json, icon.png and README.md among them). The
 * game never asks for these names, so they are passed over without a word. */
static int package_file(const char *rel)
{
    static const char *const names[] = {
        "manifest.json", "icon.png", "readme.md", "readme.txt", "changelog.md",
        "mod.json",
    };
    int i;

    if (strchr(rel, '\\'))
        return 0;
    if (!strncmp(rel, "license", 7))            /* LICENSE, LICENSE.txt, ... */
        return 1;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!strcmp(rel, names[i]))
            return 1;
    return 0;
}

static int push(pending **list, int *count, int *cap, char *rel, char *entry, int mod)
{
    if (*count >= MAX_FILES)
        return 0;
    if (*count == *cap) {
        int c = *cap ? *cap * 2 : 256;
        pending *p = (pending *)realloc(*list, (size_t)c * sizeof *p);
        if (!p)
            return 0;
        *list = p;
        *cap = c;
    }
    (*list)[*count].rel = rel;
    (*list)[*count].entry = entry;
    (*list)[*count].mod = mod;
    (*count)++;
    return 1;
}

static void add_file(const WCHAR *relw, int mod, ULONGLONG size)
{
    char rel[MAX_PATH], *cut;
    int i, n = WideCharToMultiByte(CP_UTF8, 0, relw, -1, rel, sizeof rel, NULL, NULL);

    if (n <= 0)
        return;
    for (i = 0; rel[i]; i++) {
        if ((uint8_t)rel[i] >= 0x80) {
            if (log_line_ok())
                gb_log("[MODS] %s: a file name with letters outside plain ASCII is not a game "
                       "file; not used\n", s_mods[mod].name);
            return;                         /* the disc's names are plain ASCII */
        }
        if (rel[i] >= 'A' && rel[i] <= 'Z')
            rel[i] = (char)(rel[i] - 'A' + 'a');
    }
    if (package_file(rel))
        return;
    /* A mod's textures\ folder holds picture replacements the renderer reads
     * by fingerprint (gb_d3d11.c); the disc has no such folder. */
    if (!strncmp(rel, "textures\\", 9))
        return;
    if (size == 0) {
        if (log_line_ok())
            gb_log("[MODS] %s\\%s is empty; not used\n", s_mods[mod].name, rel);
        return;
    }
    if (disc_kind(rel) == 1) {              /* a whole file of the disc */
        char *r = _strdup(rel);
        if (r && !push(&s_pend, &s_npend, &s_cappend, r, NULL, mod))
            free(r);
        return;
    }
    /* An entry of an archive: the longest leading part of the path that is a
     * file on the disc names the archive, and what follows names the entry. */
    for (cut = rel + strlen(rel); cut > rel; cut--) {
        if (*cut != '\\')
            continue;
        *cut = 0;
        if (disc_kind(rel) == 1) {
            char *a, *e, *sub = strchr(cut + 1, '\\');
            /* One file name under the archive's folder, or, for a line of
             * text in a pack, the entry's folder and the line's file. */
            if (sub && strchr(sub + 1, '\\')) {
                if (log_line_ok())
                    gb_log("[MODS] %s\\%s\\%s: an archive entry is named by one file name "
                           "under the archive's folder; not used\n", s_mods[mod].name, rel, cut + 1);
                return;
            }
            a = _strdup(rel);
            e = _strdup(cut + 1);
            if (!a || !e || !push(&s_epend, &s_nepend, &s_capepend, a, e, mod)) {
                free(a);
                free(e);
            }
            return;
        }
        *cut = '\\';
    }
    if (log_line_ok())
        gb_log("[MODS] %s\\%s: the disc has no such file and no archive by that name; "
               "not used\n", s_mods[mod].name, rel);
}

static void scan(const WCHAR *dir, const WCHAR *rel, int mod, int depth)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;

    if (depth > 8)
        return;
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s%s%s\\*", dir, rel[0] ? L"\\" : L"", rel);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.cFileName[0] == L'.')
            continue;
        _snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%s%s%s", rel, rel[0] ? L"\\" : L"", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            scan(dir, sub, mod, depth + 1);
        else
            add_file(sub, mod,
                     ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static int add_mod(const WCHAR *mods_dir, const WCHAR *folder)
{
    WCHAR dir[MAX_PATH];
    gb_mod *m;
    HANDLE h;
    DWORD a;
    int before = s_npend + s_nepend, i;

    if (s_nmods >= MAX_MODS)
        return 0;
    for (i = 0; i < s_nmods; i++) {
        WCHAR have[NAME_LEN];
        MultiByteToWideChar(CP_UTF8, 0, s_mods[i].name, -1, have, NAME_LEN);
        if (!_wcsicmp(have, folder))
            return 0;                       /* listed twice */
    }
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\%s", mods_dir, folder);
    a = GetFileAttributesW(dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        char n[NAME_LEN];
        WideCharToMultiByte(CP_UTF8, 0, folder, -1, n, sizeof n, NULL, NULL);
        gb_log("[MODS] %s is in load_order.txt but there is no such folder\n", n);
        return 0;
    }
    h = CreateFileW(dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    /* The toolkit keeps a guest handle in 32 bits and treats values with its
     * own tag (0x48000000) as its table's; a Windows handle is far below. */
    if ((uintptr_t)h >= 0x01000000u) {
        CloseHandle(h);
        return 0;
    }
    m = &s_mods[s_nmods];
    WideCharToMultiByte(CP_UTF8, 0, folder, -1, m->name, sizeof m->name, NULL, NULL);
    m->root = h;
    scan(dir, L"", s_nmods, 0);
    m->files = s_npend + s_nepend - before;
    s_nmods++;
    return 1;
}

static int cmp_names(const void *a, const void *b)
{
    return _wcsicmp((const WCHAR *)a, (const WCHAR *)b);
}

/* Every folder in the mods folder, in name order. */
static void add_all(const WCHAR *mods_dir)
{
    static WCHAR names[MAX_MODS][NAME_LEN];
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0, i;

    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*", mods_dir);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.'
                && n < MAX_MODS && wcslen(fd.cFileName) < NAME_LEN)
            wcscpy_s(names[n++], NAME_LEN, fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    for (i = 0; i < n; i++)
        add_mod(mods_dir, names[i]);
}

/* mods\load_order.txt: returns 0 when there is none. */
static int add_listed(const WCHAR *mods_dir)
{
    WCHAR file[MAX_PATH];
    char line[512];
    FILE *f;

    _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%s\\load_order.txt", mods_dir);
    if (_wfopen_s(&f, file, L"rb") || !f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        WCHAR w[NAME_LEN];
        char *s = line, *e;

        if (!strncmp(s, "\xEF\xBB\xBF", 3))
            s += 3;                         /* Notepad's byte-order mark */
        while (*s == ' ' || *s == '\t')
            s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        if (!*s || *s == '#' || *s == '.' || strpbrk(s, "\\/:"))
            continue;
        if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, NAME_LEN) > 0)
            add_mod(mods_dir, w);
    }
    fclose(f);
    return 1;
}

/* Into the table, first mod first; a later mod's copy of a path is left out. */
static void build_table(void)
{
    int i;

    s_cap = 1024;
    while (s_cap < (uint32_t)s_npend * 2)
        s_cap *= 2;
    s_tab = (gb_mod_file *)calloc(s_cap, sizeof *s_tab);
    if (!s_tab)
        return;
    for (i = 0; i < s_npend; i++) {
        gb_mod_file *have = find(s_pend[i].rel);
        uint32_t k;

        if (have) {
            if (log_line_ok())
                gb_log("[MODS] %s: %s comes first, so %s's copy is not used\n",
                       s_pend[i].rel, s_mods[have->mod].name, s_mods[s_pend[i].mod].name);
            free(s_pend[i].rel);
            continue;
        }
        for (k = hash(s_pend[i].rel) & (s_cap - 1); s_tab[k].path; k = (k + 1) & (s_cap - 1))
            ;
        s_tab[k].path = s_pend[i].rel;
        s_tab[k].mod = s_pend[i].mod;
        s_nfiles++;
    }
    free(s_pend);
    s_pend = NULL;
}

/* ── single entries of the disc's archives ────────────────── */

/* The disc's archives are EA "BIG" files (magic BIGF): a 16-byte header
 * (magic, a size field, then big-endian entry count and header size), then
 * per entry a big-endian offset and size and a NUL-terminated name. The game
 * reads the whole header when it opens an archive and finds entries through
 * it, so an archive whose header points at new data is all a replaced entry
 * needs. Such an archive is built once in mods\.merged\ (the disc's archive,
 * or another mod's whole replacement of it, with the new entries appended and
 * the header pointed at them), and the game's open of the archive is sent
 * there like any replaced file. It is built again only when a file it was
 * made from changes. */

#define MERGED      (-1)
#define BIG_ALIGN   2048u           /* new entries start on a 2 KB boundary */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void mod_path(int mod, const char *rel, WCHAR *out)
{
    WCHAR name[NAME_LEN], r[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, s_mods[mod].name, -1, name, NAME_LEN);
    MultiByteToWideChar(CP_UTF8, 0, rel, -1, r, MAX_PATH);
    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s", s_dir, name, r);
}

static int file_id(const WCHAR *p, ULONGLONG *size, ULONGLONG *time)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d))
        return 0;
    *size = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    *time = ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return 1;
}

/* Reads the header of a BIG archive: the whole header (16 + table) in a
 * malloc'd buffer, or NULL. */
static uint8_t *big_header(const WCHAR *p, uint32_t *hsize, uint32_t *count)
{
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    uint8_t h[16], *b = NULL;
    DWORD got;

    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    if (ReadFile(f, h, 16, &got, NULL) && got == 16 && !memcmp(h, "BIGF", 4)) {
        *count = be32(h + 8);
        *hsize = be32(h + 12);
        if (*hsize >= 16 + *count * 9u && *hsize < (64u << 20) && (b = (uint8_t *)malloc(*hsize))) {
            memcpy(b, h, 16);
            if (!ReadFile(f, b + 16, *hsize - 16, &got, NULL) || got != *hsize - 16) {
                free(b);
                b = NULL;
            }
        }
    }
    CloseHandle(f);
    return b;
}

/* The table's entries: offset and size fields and the name of each. */
typedef struct { uint8_t *field; const char *name; } big_entry;

static big_entry *big_entries(uint8_t *hdr, uint32_t hsize, uint32_t count)
{
    big_entry *e = (big_entry *)calloc(count ? count : 1, sizeof *e);
    uint32_t i, at = 16;

    if (!e)
        return NULL;
    for (i = 0; i < count; i++) {
        uint8_t *nul;
        if (at + 9 > hsize || !(nul = (uint8_t *)memchr(hdr + at + 8, 0, hsize - at - 8))) {
            free(e);
            return NULL;
        }
        e[i].field = hdr + at;
        e[i].name = (const char *)(hdr + at + 8);
        at = (uint32_t)(nul - hdr) + 1;
    }
    return e;
}

/* Which entry a mod's file names: the entry's own name (any case), or, for
 * any archive, its number in the archive's list counted from 0, as the
 * file's name before the first dot ("417.xsh", "0417", "417.o"). */
static int entry_index(const big_entry *e, uint32_t count, const char *want)
{
    uint32_t i, v = 0;
    const char *s;

    for (i = 0; i < count; i++)
        if (e[i].name[0] && !_stricmp(e[i].name, want))
            return (int)i;
    for (s = want; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 1000000)
            return -1;
    }
    if (s == want || (*s && *s != '.'))
        return -1;
    return v < count ? (int)v : -1;
}

static int copy_into(HANDLE out, const WCHAR *src, ULONGLONG size)
{
    static uint8_t buf[1 << 20];
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    ULONGLONG left = size;
    DWORD got, put;

    if (in == INVALID_HANDLE_VALUE)
        return 0;
    while (left) {
        DWORD want = left > sizeof buf ? (DWORD)sizeof buf : (DWORD)left;
        if (!ReadFile(in, buf, want, &got, NULL) || got != want
                || !WriteFile(out, buf, got, &put, NULL) || put != got)
            break;
        left -= got;
    }
    CloseHandle(in);
    return left == 0;
}

static void make_dirs(WCHAR *p)        /* every folder of a file's path */
{
    WCHAR *s;
    for (s = p + 3; *s; s++)
        if (*s == L'\\') {
            *s = 0;
            CreateDirectoryW(p, NULL);
            *s = L'\\';
        }
}

static void add_merged(const char *rel)
{
    gb_mod_file *have = find(rel);
    uint32_t k;

    if (have) {
        have->mod = MERGED;
        return;
    }
    for (k = hash(rel) & (s_cap - 1); s_tab[k].path; k = (k + 1) & (s_cap - 1))
        ;
    s_tab[k].path = _strdup(rel);
    s_tab[k].mod = MERGED;
    if (s_tab[k].path)
        s_nfiles++;
}

typedef struct { int index; int mod; WCHAR path[MAX_PATH]; ULONGLONG size, time; } replacement;

/* The folder of merged archives, opened once for the redirect. */
static int open_merged_root(void)
{
    WCHAR m[MAX_PATH];

    if (s_merged_root)
        return 1;
    _snwprintf_s(m, MAX_PATH, _TRUNCATE, L"%s\\.merged", s_dir);
    s_merged_root = CreateFileW(m, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (s_merged_root == INVALID_HANDLE_VALUE || (uintptr_t)s_merged_root >= 0x01000000u) {
        if (s_merged_root != INVALID_HANDLE_VALUE)
            CloseHandle(s_merged_root);
        s_merged_root = NULL;
        gb_log("[MODS] the folder of merged archives could not be opened; replaced "
               "entries are not used\n");
        return 0;
    }
    return 1;
}

/* The first two bytes of a file, big-endian (0xC0FB for a pack), or 0. */
static unsigned magic_of(const WCHAR *p)
{
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    uint8_t h[2];
    DWORD got = 0;

    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(f, h, 2, &got, NULL))
        got = 0;
    CloseHandle(f);
    return got == 2 ? ((unsigned)h[0] << 8) | h[1] : 0;
}

/* A whole file into a malloc'd buffer (at most max bytes), or NULL. */
static uint8_t *read_all(const WCHAR *p, uint32_t max, uint32_t *size)
{
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER li;
    uint8_t *b = NULL;
    DWORD got;

    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    if (GetFileSizeEx(f, &li) && li.QuadPart > 0 && li.QuadPart <= max
            && (b = (uint8_t *)malloc((size_t)li.QuadPart + 2))) {
        if (!ReadFile(f, b, (DWORD)li.QuadPart, &got, NULL) || got != (DWORD)li.QuadPart) {
            free(b);
            b = NULL;
        } else
            *size = (uint32_t)li.QuadPart;
    }
    CloseHandle(f);
    return b;
}

/* ── single items of the disc's C0FB packs ────────────────── */

/* The disc's other container is EA's small pack (first bytes C0 FB): the
 * menus' pictures (fe\xa*.bin), fonts, layouts and palettes, the text
 * (locale\*.eus), the credits and the speech headers. Its header is C0 FB, a
 * big-endian 16-bit header size (the bytes after the first four), a 16-bit
 * entry count, then per entry a 3-byte offset, a 3-byte size and a
 * NUL-terminated name (empty in every pack but the text and speech ones).
 * The data follows in entry order, each entry on the pack's alignment (4,
 * 64 or 128 bytes), zero-filled between. No entry on the disc is compressed,
 * and the game's lookup (guest 0x001A83C0) reads offsets and sizes from the
 * header of the pack it has read whole into memory, by name or by number.
 *
 * So a pack with replaced items is rebuilt whole in mods\.merged\: the same
 * header and entries in the same order, the replaced ones swapped in and
 * every offset worked out again on the same alignment. Unlike a BIG archive
 * (whose new entries are appended) nothing is left behind, because the game
 * holds the whole pack in memory: it grows only by what the mods add.
 *
 * Two kinds of item:
 *   mods\<mod>\<pack>\<entry>            the entry's whole data, by name or
 *                                        number, as for a BIG archive
 *   mods\<mod>\<pack>\<entry>\<n>.txt    line n of a text entry (LOCH), as
 *                                        UTF-8 text; a line break in the file
 *                                        is the game's \n
 * A replacement of a picture, text, or font entry must have the kind's
 * header and hold together, or it is refused with a line in the log. */

#define PACK_MAX     (16u << 20)        /* 3-byte offsets and sizes */
#define LINE_MAX_CH  2000

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void put_be24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

typedef struct { uint8_t *field; const char *name; uint32_t off, size; } pack_entry;

/* The table of a pack read whole; NULL when it does not hold together. */
static pack_entry *pack_entries(uint8_t *d, uint32_t n, uint32_t *count, uint32_t *table_end)
{
    pack_entry *e;
    uint32_t i, at = 6;

    if (n < 6 || d[0] != 0xC0 || d[1] != 0xFB)
        return NULL;
    *count = ((uint32_t)d[4] << 8) | d[5];
    e = (pack_entry *)calloc(*count ? *count : 1, sizeof *e);
    if (!e)
        return NULL;
    for (i = 0; i < *count; i++) {
        uint8_t *nul;
        if (at + 7 > n || !(nul = (uint8_t *)memchr(d + at + 6, 0, n - at - 6))) {
            free(e);
            return NULL;
        }
        e[i].field = d + at;
        e[i].off = be24(d + at);
        e[i].size = be24(d + at + 3);
        e[i].name = (const char *)(d + at + 6);
        at = (uint32_t)(nul - d) + 1;
        if (e[i].off > n || e[i].size > n - e[i].off) {
            free(e);
            return NULL;
        }
    }
    *table_end = at;
    return e;
}

/* An entry by its own name (any case) or its number before the first dot. */
static int pack_index(const pack_entry *e, uint32_t count, const char *want, size_t len)
{
    uint32_t i, v = 0;
    size_t k;

    for (i = 0; i < count; i++)
        if (e[i].name[0] && strlen(e[i].name) == len && !_strnicmp(e[i].name, want, len))
            return (int)i;
    for (k = 0; k < len && want[k] >= '0' && want[k] <= '9'; k++) {
        v = v * 10 + (uint32_t)(want[k] - '0');
        if (v > 1000000)
            return -1;
    }
    if (k == 0 || (k < len && want[k] != '.'))
        return -1;
    return v < count ? (int)v : -1;
}

/* A text entry (LOCH): the LOCL part's offset and line count, or 0. */
static int loch_parse(const uint8_t *b, uint32_t n, uint32_t *locl, uint32_t *lines)
{
    uint32_t l, c, k;

    if (n < 20 || memcmp(b, "LOCH", 4))
        return 0;
    l = le32(b + 16);
    if (l > n || n - l < 16 || memcmp(b + l, "LOCL", 4))
        return 0;
    c = le32(b + l + 12);
    if (c > 65536 || (n - l - 16) / 4 < c)
        return 0;
    for (k = 0; k < c; k++) {             /* every line starts inside and ends */
        uint32_t p = le32(b + l + 16 + 4 * k), q;
        if (p & 1 || p >= n - l)
            return 0;
        for (q = l + p; q + 1 < n && (b[q] | b[q + 1]); q += 2)
            ;
        if (q + 1 >= n)
            return 0;
    }
    *locl = l;
    *lines = c;
    return 1;
}

/* Does a replacement hold together as the kind of entry it replaces? NULL if
 * so, else the reason in a few words. Kinds the game reads by a header are
 * checked: pictures (SHPX), text (LOCH), fonts (FNTX). */
static const char *entry_damage(const uint8_t *old, uint32_t on, const uint8_t *b, uint32_t n)
{
    uint32_t i, c, a, l;

    if (on >= 4 && !memcmp(old, "SHPX", 4)) {
        if (n < 16 || memcmp(b, "SHPX", 4))
            return "it is not a picture file (SHPX), as the entry it replaces is";
        if (le32(b + 4) != n)
            return "its size field does not match the file's size (cut short or padded?)";
        c = le32(b + 8);
        if (!c || c > 4096 || 16 + 8 * c > n)
            return "its list of images does not fit in the file";
        for (i = 0; i < c; i++) {
            a = le32(b + 16 + 8 * i + 4);
            if (a < 16 + 8 * c || a > n - 16 || !(b[a + 4] | b[a + 5]) || !(b[a + 6] | b[a + 7]))
                return "an image in it lies outside the file or has no size";
        }
    } else if (on >= 4 && !memcmp(old, "LOCH", 4)) {
        if (!loch_parse(b, n, &l, &c))
            return "it is not a text table (LOCH) that holds together";
    } else if (on >= 4 && !memcmp(old, "FNTX", 4)) {
        if (n < 0x80 || memcmp(b, "FNTX", 4))
            return "it is not a font file (FNTX), as the entry it replaces is";
    }
    return NULL;
}

/* A line of text from a mod's .txt file, as the game's UTF-16 (a malloc'd
 * array of *len units, no terminator), or NULL with *why set. */
static uint16_t *line_text(const uint8_t *b, uint32_t n, int *len, const char **why, int *wide)
{
    WCHAR *w;
    uint16_t *out;
    int wn, i, o = 0;

    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        b += 3;
        n -= 3;
    }
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r'))
        n--;
    if (!n) {
        *len = 0;
        *wide = 0;
        return (uint16_t *)calloc(1, sizeof(uint16_t));
    }
    wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)b, (int)n, NULL, 0);
    if (wn <= 0) {
        *why = "it is not UTF-8 text";
        return NULL;
    }
    if (wn > LINE_MAX_CH) {
        *why = "it is longer than 2000 characters";
        return NULL;
    }
    w = (WCHAR *)malloc((size_t)wn * sizeof *w);
    out = (uint16_t *)malloc((size_t)wn * 2 * sizeof *out);
    if (!w || !out) {
        free(w);
        free(out);
        *why = "out of memory";
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, (const char *)b, (int)n, w, wn);
    *wide = 0;
    for (i = 0; i < wn; i++) {
        WCHAR c = w[i];
        if (c == L'\r')
            continue;
        if (c == L'\n') {                   /* the game's line break is the two characters \n */
            out[o++] = L'\\';
            out[o++] = L'n';
            continue;
        }
        if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) {
            free(w);
            free(out);
            *why = c < 0x20 ? "it holds a control character" : "it holds a character outside "
                   "the game's 16-bit text (an emoji, say)";
            return NULL;
        }
        if (c > 0xFF)
            (*wide)++;
        out[o++] = (uint16_t)c;
    }
    free(w);
    *len = o;
    return out;
}

typedef struct {
    int index, line;                    /* line -1: the whole entry */
    int mod;
    char name[MAX_PATH];                /* as the mod named it, for the log */
    WCHAR path[MAX_PATH];
    ULONGLONG size, time;
    uint8_t *data;
    uint32_t n;
    uint16_t *text;
    int tlen;
} pack_item;

/* A text entry rebuilt with some of its lines replaced; a malloc'd blob. */
static uint8_t *loch_rebuild(const uint8_t *b, uint32_t n, const pack_item *it, int nit,
                             int index, uint32_t *out_n)
{
    uint32_t l, c, k, size, at;
    uint8_t *o;
    int j;

    if (!loch_parse(b, n, &l, &c))
        return NULL;
    size = l + 16 + 4 * c;
    for (k = 0; k < c; k++) {
        const uint16_t *s = NULL;
        uint32_t len = 0, p = l + le32(b + l + 16 + 4 * k);
        for (j = 0; j < nit; j++)
            if (it[j].index == index && it[j].line == (int)k)
                s = it[j].text, len = (uint32_t)it[j].tlen;
        if (!s)
            for (; b[p + 2 * len] | b[p + 2 * len + 1]; len++)
                ;
        size += len ? (len + 2) * 2 : 2;   /* as on the disc: two NULs after a line, one for an empty one */
    }
    size = (size + 3) & ~3u;
    if (!(o = (uint8_t *)calloc(size, 1)))
        return NULL;
    memcpy(o, b, l + 16);
    put_le32(o + l + 4, size - l);
    at = l + 16 + 4 * c;
    for (k = 0; k < c; k++) {
        const uint16_t *s = NULL;
        uint32_t len = 0, p = l + le32(b + l + 16 + 4 * k), q;
        for (j = 0; j < nit; j++)
            if (it[j].index == index && it[j].line == (int)k)
                s = it[j].text, len = (uint32_t)it[j].tlen;
        put_le32(o + l + 16 + 4 * k, at - l);
        if (s) {
            for (q = 0; q < len; q++) {
                o[at + 2 * q] = (uint8_t)s[q];
                o[at + 2 * q + 1] = (uint8_t)(s[q] >> 8);
            }
        } else {
            for (; b[p + 2 * len] | b[p + 2 * len + 1]; len++)
                ;
            memcpy(o + at, b + p, 2 * len);
        }
        at += len ? (len + 2) * 2 : 2;
    }
    *out_n = size;
    return o;
}

/* One pack: every item the mods replace in it, earliest mod first. */
static void merge_pack(const char *arch, const int *which, int nwhich, const WCHAR *base,
                       ULONGLONG bsize, ULONGLONG btime, int base_mod)
{
    WCHAR out[MAX_PATH], tmp[MAX_PATH], keyf[MAX_PATH], r[MAX_PATH];
    char key[16384], old[sizeof key], shown[600];
    uint8_t *d = NULL, *nd = NULL, **blob = NULL;
    uint32_t n = 0, count = 0, table_end = 0, i, al, pos, *bn = NULL;
    pack_entry *ent = NULL;
    pack_item *it = NULL;
    int nit = 0, j, q, at, reuse = 0, ntext = 0, nwhole = 0, wide = 0;
    DWORD t0 = GetTickCount(), got, put;
    ULONGLONG osize, otime;
    HANDLE f;

    MultiByteToWideChar(CP_UTF8, 0, arch, -1, r, MAX_PATH);
    if (!(d = read_all(base, PACK_MAX, &n)) || !(ent = pack_entries(d, n, &count, &table_end))) {
        gb_log("[MODS] %s: its list of entries could not be read; its entries in the mods are "
               "not used\n", arch);
        goto done;
    }
    if (!(it = (pack_item *)calloc((size_t)nwhich, sizeof *it)))
        goto done;

    for (j = 0; j < nwhich; j++) {
        pending *p = &s_epend[which[j]];
        const char *sub = strchr(p->entry, '\\');
        size_t elen = sub ? (size_t)(sub - p->entry) : strlen(p->entry);
        int idx = pack_index(ent, count, p->entry, elen), line = -1;
        pack_item *x = &it[nit];
        const char *why = NULL;

        if (idx < 0) {
            if (log_line_ok())
                gb_log("[MODS] %s\\%s\\%.*s: %s has no entry by that name or number (it has %u, "
                       "numbered 0 to %u); not used\n", s_mods[p->mod].name, arch, (int)elen,
                       p->entry, arch, count, count ? count - 1 : 0);
            continue;
        }
        if (sub) {
            const char *s = sub + 1;
            uint32_t v = 0;
            for (; *s >= '0' && *s <= '9' && v < 1000000; s++)
                v = v * 10 + (uint32_t)(*s - '0');
            if (s == sub + 1 || (*s && *s != '.')) {
                if (log_line_ok())
                    gb_log("[MODS] %s\\%s\\%s: a line of text is named by its number (\"3.txt\"); "
                           "not used\n", s_mods[p->mod].name, arch, p->entry);
                continue;
            }
            line = (int)v;
        }
        for (q = 0; q < nit && !(it[q].index == idx && it[q].line == line); q++)
            ;
        if (q < nit) {
            if (log_line_ok()) {
                if (line < 0)
                    gb_log("[MODS] %s entry %d: %s comes first, so %s's copy (%s) is not used\n",
                           arch, idx, s_mods[it[q].mod].name, s_mods[p->mod].name, p->entry);
                else
                    gb_log("[MODS] %s entry %d line %d: %s comes first, so %s's copy (%s) is not "
                           "used\n", arch, idx, line, s_mods[it[q].mod].name,
                           s_mods[p->mod].name, p->entry);
            }
            continue;
        }
        x->index = idx;
        x->line = line;
        x->mod = p->mod;
        snprintf(x->name, sizeof x->name, "%s\\%s\\%s", s_mods[p->mod].name, arch, p->entry);
        {
            char er[MAX_PATH];
            snprintf(er, sizeof er, "%s\\%s", arch, p->entry);
            mod_path(p->mod, er, x->path);
        }
        if (!file_id(x->path, &x->size, &x->time)
                || !(x->data = read_all(x->path, PACK_MAX, &x->n))) {
            if (log_line_ok())
                gb_log("[MODS] %s did not open or is too large; not used\n", x->name);
            continue;
        }
        if (line < 0)
            why = entry_damage(d + ent[idx].off, ent[idx].size, x->data, x->n);
        else {
            int w = 0;
            x->text = line_text(x->data, x->n, &x->tlen, &why, &w);
            if (x->text && w && log_line_ok())
                gb_log("[MODS] %s: %d character%s past U+00FF, which the game's fonts do not "
                       "have; they may not be drawn\n", x->name, w, w == 1 ? "" : "s");
            wide += w;
        }
        if (why) {
            if (log_line_ok())
                gb_log("[MODS] %s is not used: %s\n", x->name, why);
            free(x->data);
            free(x->text);
            memset(x, 0, sizeof *x);
            continue;
        }
        nit++;
    }
    (void)wide;

    /* Each entry's new data: the mod's whole entry or the disc's, and then
     * any lines on top. */
    blob = (uint8_t **)calloc(count ? count : 1, sizeof *blob);
    bn = (uint32_t *)calloc(count ? count : 1, sizeof *bn);
    if (!blob || !bn)
        goto done;
    for (i = 0; i < count; i++) {
        blob[i] = d + ent[i].off;
        bn[i] = ent[i].size;
    }
    for (j = 0; j < nit; j++)
        if (it[j].line < 0) {
            blob[it[j].index] = it[j].data;
            bn[it[j].index] = it[j].n;
            nwhole++;
        }
    for (j = 0; j < nit; j++) {
        uint32_t l, c, nb;
        uint8_t *rb;
        int k;

        if (it[j].line < 0 || it[j].text == NULL)
            continue;
        for (k = 0; k < j; k++)              /* this entry already rebuilt */
            if (it[k].line >= 0 && it[k].text && it[k].index == it[j].index)
                break;
        if (k < j)
            continue;
        if (!loch_parse(blob[it[j].index], bn[it[j].index], &l, &c)) {
            for (k = j; k < nit; k++)
                if (it[k].line >= 0 && it[k].index == it[j].index && it[k].text) {
                    if (log_line_ok())
                        gb_log("[MODS] %s is not used: entry %d of %s is not text\n",
                               it[k].name, it[k].index, arch);
                    free(it[k].text);
                    it[k].text = NULL;
                }
            continue;
        }
        for (k = j; k < nit; k++)
            if (it[k].line >= 0 && it[k].index == it[j].index && it[k].text
                    && (uint32_t)it[k].line >= c) {
                if (log_line_ok())
                    gb_log("[MODS] %s is not used: entry %d of %s has lines 0 to %u\n",
                           it[k].name, it[k].index, arch, c ? c - 1 : 0);
                free(it[k].text);
                it[k].text = NULL;
            }
        if (!(rb = loch_rebuild(blob[it[j].index], bn[it[j].index], it, nit, it[j].index, &nb)))
            continue;
        blob[it[j].index] = rb;               /* freed with the items below */
        bn[it[j].index] = nb;
        for (k = j; k < nit; k++)
            if (it[k].line >= 0 && it[k].index == it[j].index && it[k].text)
                ntext++;
        it[j].tlen = -1;                      /* marks the owner of rb */
        it[j].data = (free(it[j].data), rb);
        it[j].n = nb;
    }
    if (!nwhole && !ntext)
        goto done;

    /* The new pack: same header, entries in order on the same alignment. */
    for (al = 2048; al > 4; al /= 2) {
        for (i = 0; i < count && ent[i].off % al == 0; i++)
            ;
        if (i == count)
            break;
    }
    pos = table_end;
    for (i = 0; i < count; i++) {
        pos = (pos + al - 1) & ~(al - 1);
        if ((ULONGLONG)pos + bn[i] >= PACK_MAX) {
            gb_log("[MODS] %s: with its replaced items it would pass 16 MB, the most a pack can "
                   "address; not used\n", arch);
            goto done;
        }
        pos += bn[i];
    }
    if (!(nd = (uint8_t *)calloc(pos ? pos : 1, 1)))
        goto done;
    memcpy(nd, d, table_end);
    {
        uint32_t at2 = table_end;
        for (i = 0; i < count; i++) {
            at2 = (at2 + al - 1) & ~(al - 1);
            memcpy(nd + at2, blob[i], bn[i]);
            put_be24(nd + (ent[i].field - d), at2);
            put_be24(nd + (ent[i].field - d) + 3, bn[i]);
            at2 += bn[i];
        }
    }

    /* The key: what the merged pack is made from. */
    at = snprintf(key, sizeof key, "gamebreaker merged pack 1\r\nbase %d %llu %llu\r\n",
                  base_mod, bsize, btime);
    for (j = 0; j < nit && at > 0 && at < (int)sizeof key - 400; j++) {
        char pa[MAX_PATH * 3];
        if (it[j].line >= 0 && !it[j].text && it[j].tlen != -1)
            continue;
        WideCharToMultiByte(CP_UTF8, 0, it[j].path, -1, pa, sizeof pa, NULL, NULL);
        at += snprintf(key + at, sizeof key - (size_t)at, "item %d %d %llu %llu %s\r\n",
                       it[j].index, it[j].line, it[j].size, it[j].time, pa);
    }
    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\.merged\\%s", s_dir, r);
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.building", out);
    _snwprintf_s(keyf, MAX_PATH, _TRUNCATE, L"%s.made-from.txt", out);
    f = CreateFileW(keyf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (ReadFile(f, old, sizeof old - 1, &got, NULL)) {
            old[got] = 0;
            reuse = !strcmp(old, key) && file_id(out, &osize, &otime) && osize == pos;
        }
        CloseHandle(f);
    }
    if (!reuse) {
        WCHAR m[MAX_PATH];
        make_dirs(out);
        _snwprintf_s(m, MAX_PATH, _TRUNCATE, L"%s\\.merged", s_dir);
        SetFileAttributesW(m, FILE_ATTRIBUTE_HIDDEN);
        DeleteFileW(keyf);
        f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE || !WriteFile(f, nd, pos, &put, NULL) || put != pos) {
            gb_log("[MODS] %s: the merged pack could not be written in the mods folder "
                   "(error %lu); its replaced items are not used\n", arch, GetLastError());
            if (f != INVALID_HANDLE_VALUE)
                CloseHandle(f);
            DeleteFileW(tmp);
            goto done;
        }
        CloseHandle(f);
        if (!MoveFileExW(tmp, out, MOVEFILE_REPLACE_EXISTING)) {
            gb_log("[MODS] %s: the merged pack could not be written (error %lu); its replaced "
                   "items are not used\n", arch, GetLastError());
            DeleteFileW(tmp);
            goto done;
        }
        f = CreateFileW(keyf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, key, (DWORD)strlen(key), &put, NULL);
            CloseHandle(f);
        }
    }
    if (!open_merged_root())
        goto done;
    add_merged(arch);
    at = snprintf(shown, sizeof shown, "[MODS] %s: ", arch);
    if (nwhole)
        at += snprintf(shown + at, sizeof shown - (size_t)at, "%d entr%s", nwhole,
                       nwhole == 1 ? "y" : "ies");
    if (ntext)
        at += snprintf(shown + at, sizeof shown - (size_t)at, "%s%d line%s of text",
                       nwhole ? " and " : "", ntext, ntext == 1 ? "" : "s");
    at += snprintf(shown + at, sizeof shown - (size_t)at, " replaced (");
    for (j = 0, q = 0; j < nit && at > 0 && at < (int)sizeof shown - 80; j++) {
        if (it[j].line >= 0 && !it[j].text && it[j].tlen != -1)
            continue;
        if (it[j].line < 0)
            at += snprintf(shown + at, sizeof shown - (size_t)at, "%s%d from %s", q++ ? ", " : "",
                           it[j].index, s_mods[it[j].mod].name);
        else
            at += snprintf(shown + at, sizeof shown - (size_t)at, "%s%d line %d from %s",
                           q++ ? ", " : "", it[j].index, it[j].line, s_mods[it[j].mod].name);
    }
    gb_log("%s); merged pack %s (%.1f KB, %+ld bytes, %lu ms)\n", shown,
           reuse ? "reused" : "built", (double)pos / 1024.0, (long)pos - (long)n,
           (unsigned long)(GetTickCount() - t0));
done:
    if (it)
        for (j = 0; j < nwhich; j++) {
            free(it[j].data);
            free(it[j].text);
        }
    free(it);
    free(blob);
    free(bn);
    free(nd);
    free(ent);
    free(d);
}

/* One archive: every entry the mods replace in it, earliest mod first. */
static void merge_archive(const char *arch, const int *which, int nwhich)
{
    WCHAR base[MAX_PATH], out[MAX_PATH], tmp[MAX_PATH], keyf[MAX_PATH], r[MAX_PATH];
    char key[8192], old[sizeof key], shown[512];
    uint8_t *hdr;
    big_entry *ent;
    replacement *rep;
    uint32_t hsize = 0, count = 0, i, end = 0, field;
    ULONGLONG bsize, btime, osize, otime, total;
    gb_mod_file *whole = find(arch);
    int nrep = 0, j, at = 0, reuse = 0, base_mod = whole ? whole->mod : MERGED;
    DWORD t0 = GetTickCount(), got;
    HANDLE f;

    MultiByteToWideChar(CP_UTF8, 0, arch, -1, r, MAX_PATH);
    if (whole)
        mod_path(whole->mod, arch, base);
    else
        _snwprintf_s(base, MAX_PATH, _TRUNCATE, L"%s\\%s", s_disc, r);
    if (file_id(base, &bsize, &btime) && magic_of(base) == 0xC0FB) {
        merge_pack(arch, which, nwhich, base, bsize, btime, base_mod);
        return;
    }
    if (!file_id(base, &bsize, &btime) || !(hdr = big_header(base, &hsize, &count))) {
        gb_log("[MODS] %s is not an archive whose entries can be replaced (not a BIG file "
               "or C0FB pack); its entries in the mods are not used\n", arch);
        return;
    }
    if (!(ent = big_entries(hdr, hsize, count)) || !(rep = (replacement *)calloc((size_t)nwhich, sizeof *rep))) {
        gb_log("[MODS] %s: its list of entries could not be read; its entries in the mods are "
               "not used\n", arch);
        free(ent);
        free(hdr);
        return;
    }
    for (j = 0; j < nwhich; j++) {
        pending *p = &s_epend[which[j]];
        int idx = entry_index(ent, count, p->entry), q;

        if (strchr(p->entry, '\\')) {
            if (log_line_ok())
                gb_log("[MODS] %s\\%s\\%s: an entry of a BIG archive is one file under the "
                       "archive's folder (lines of text are for C0FB packs); not used\n",
                       s_mods[p->mod].name, arch, p->entry);
            continue;
        }
        if (idx < 0) {
            if (log_line_ok())
                gb_log("[MODS] %s\\%s\\%s: %s has no entry by that name or number (it has %u, "
                       "numbered 0 to %u); not used\n", s_mods[p->mod].name, arch, p->entry,
                       arch, count, count ? count - 1 : 0);
            continue;
        }
        for (q = 0; q < nrep && rep[q].index != idx; q++)
            ;
        if (q < nrep) {
            if (log_line_ok())
                gb_log("[MODS] %s entry %d: %s comes first, so %s's copy (%s) is not used\n",
                       arch, idx, s_mods[rep[q].mod].name, s_mods[p->mod].name, p->entry);
            continue;
        }
        rep[nrep].index = idx;
        rep[nrep].mod = p->mod;
        {
            char er[MAX_PATH];
            snprintf(er, sizeof er, "%s\\%s", arch, p->entry);
            mod_path(p->mod, er, rep[nrep].path);
        }
        if (!file_id(rep[nrep].path, &rep[nrep].size, &rep[nrep].time) || rep[nrep].size >= 0x7FFFFFFFu)
            continue;
        nrep++;
    }
    if (!nrep)
        goto done;
    if (whole && log_line_ok())
        gb_log("[MODS] %s: the entries are put into %s's whole copy of it\n",
               arch, s_mods[whole->mod].name);

    /* The key: what the merged archive is made from. */
    at = snprintf(key, sizeof key, "gamebreaker merged archive 1\r\nbase %d %llu %llu\r\n",
                  base_mod, bsize, btime);
    for (j = 0; j < nrep && at > 0 && at < (int)sizeof key - 200; j++) {
        char pa[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, rep[j].path, -1, pa, sizeof pa, NULL, NULL);
        at += snprintf(key + at, sizeof key - (size_t)at, "entry %d %llu %llu %s\r\n",
                       rep[j].index, rep[j].size, rep[j].time, pa);
    }
    /* The data after the old end: each new entry on a 2 KB boundary. */
    for (i = 0; i < count; i++) {
        uint32_t e = be32(ent[i].field) + be32(ent[i].field + 4);
        if (e > end)
            end = e;
    }
    total = bsize > end ? bsize : end;
    for (j = 0; j < nrep; j++) {
        total = (total + BIG_ALIGN - 1) & ~(ULONGLONG)(BIG_ALIGN - 1);
        total += rep[j].size;
    }
    if (total >= 0xFFFFFFFFu) {
        gb_log("[MODS] %s: with its replaced entries it would pass 4 GB; not used\n", arch);
        goto done;
    }

    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\.merged\\%s", s_dir, r);
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.building", out);
    _snwprintf_s(keyf, MAX_PATH, _TRUNCATE, L"%s.made-from.txt", out);
    f = CreateFileW(keyf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (ReadFile(f, old, sizeof old - 1, &got, NULL)) {
            old[got] = 0;
            reuse = !strcmp(old, key) && file_id(out, &osize, &otime) && osize == total;
        }
        CloseHandle(f);
    }
    if (!reuse) {
        ULONGLONG pos;
        LARGE_INTEGER li;
        DWORD put;
        int ok = 1;

        make_dirs(out);
        {
            WCHAR m[MAX_PATH];
            _snwprintf_s(m, MAX_PATH, _TRUNCATE, L"%s\\.merged", s_dir);
            SetFileAttributesW(m, FILE_ATTRIBUTE_HIDDEN);
        }
        DeleteFileW(keyf);
        if (!CopyFileW(base, tmp, FALSE)) {
            gb_log("[MODS] %s: the merged archive could not be written in the mods folder "
                   "(error %lu); its replaced entries are not used\n", arch, GetLastError());
            goto done;
        }
        SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
        f = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            gb_log("[MODS] %s: the merged archive could not be opened (error %lu); its "
                   "replaced entries are not used\n", arch, GetLastError());
            DeleteFileW(tmp);
            goto done;
        }
        pos = bsize > end ? bsize : end;
        for (j = 0; j < nrep && ok; j++) {
            pos = (pos + BIG_ALIGN - 1) & ~(ULONGLONG)(BIG_ALIGN - 1);
            li.QuadPart = (LONGLONG)pos;
            ok = SetFilePointerEx(f, li, NULL, FILE_BEGIN) && copy_into(f, rep[j].path, rep[j].size);
            put_be32(ent[rep[j].index].field, (uint32_t)pos);
            put_be32(ent[rep[j].index].field + 4, (uint32_t)rep[j].size);
            pos += rep[j].size;
        }
        /* The size field after the magic follows the data's end (its meaning
         * is not known; on the disc it is a little short of the file's
         * size), so it moves by as much as the data grew. */
        field = be32(hdr + 4);
        if (pos > end)
            put_be32(hdr + 4, field + (uint32_t)(pos - end));
        li.QuadPart = 0;
        ok = ok && SetFilePointerEx(f, li, NULL, FILE_BEGIN)
                && WriteFile(f, hdr, hsize, &put, NULL) && put == hsize;
        li.QuadPart = (LONGLONG)pos;
        ok = ok && SetFilePointerEx(f, li, NULL, FILE_BEGIN) && SetEndOfFile(f);
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp, out, MOVEFILE_REPLACE_EXISTING)) {
            gb_log("[MODS] %s: the merged archive could not be written (error %lu); its "
                   "replaced entries are not used\n", arch, GetLastError());
            DeleteFileW(tmp);
            goto done;
        }
        f = CreateFileW(keyf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, key, (DWORD)strlen(key), &put, NULL);
            CloseHandle(f);
        }
    }
    if (!s_merged_root) {
        WCHAR m[MAX_PATH];
        _snwprintf_s(m, MAX_PATH, _TRUNCATE, L"%s\\.merged", s_dir);
        s_merged_root = CreateFileW(m, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (s_merged_root == INVALID_HANDLE_VALUE || (uintptr_t)s_merged_root >= 0x01000000u) {
            if (s_merged_root != INVALID_HANDLE_VALUE)
                CloseHandle(s_merged_root);
            s_merged_root = NULL;
            gb_log("[MODS] the folder of merged archives could not be opened; replaced "
                   "entries are not used\n");
            goto done;
        }
    }
    add_merged(arch);
    at = snprintf(shown, sizeof shown, "[MODS] %s: %d entr%s replaced (", arch, nrep,
                  nrep == 1 ? "y" : "ies");
    for (j = 0; j < nrep && at > 0 && at < (int)sizeof shown - 80; j++)
        at += snprintf(shown + at, sizeof shown - (size_t)at, "%s%d from %s", j ? ", " : "",
                       rep[j].index, s_mods[rep[j].mod].name);
    gb_log("%s); merged archive %s (%.1f MB, %lu ms)\n", shown,
           reuse ? "reused" : "built", (double)total / (1024.0 * 1024.0),
           (unsigned long)(GetTickCount() - t0));
done:
    free(rep);
    free(ent);
    free(hdr);
}

static void merge_all(void)
{
    int *which, i, j, n;
    char *done_flag;

    if (!s_nepend || !s_tab)
        goto out;
    which = (int *)malloc((size_t)s_nepend * sizeof *which);
    done_flag = (char *)calloc((size_t)s_nepend, 1);
    if (which && done_flag)
        for (i = 0; i < s_nepend; i++) {
            if (done_flag[i])
                continue;
            for (j = i, n = 0; j < s_nepend; j++)
                if (!done_flag[j] && !strcmp(s_epend[j].rel, s_epend[i].rel)) {
                    which[n++] = j;
                    done_flag[j] = 1;
                }
            merge_archive(s_epend[i].rel, which, n);
        }
    free(which);
    free(done_flag);
out:
    for (i = 0; i < s_nepend; i++) {
        free(s_epend[i].rel);
        free(s_epend[i].entry);
    }
    free(s_epend);
    s_epend = NULL;
    s_nepend = 0;
}

/* The mods in use, one name a line, in order; compared with the last start's
 * so a change can protect the save. */
static void note_mods_in_use(void)
{
    char file[MAX_PATH], now[MAX_MODS * (NAME_LEN + 2) + 1], before[sizeof now];
    size_t at = 0, got = 0;
    FILE *f;
    int i;

    now[0] = 0;
    for (i = 0; i < s_nmods; i++)
        at += (size_t)snprintf(now + at, sizeof now - at, "%s\r\n", s_mods[i].name);
    snprintf(file, sizeof file, "%s\\mods_in_use.txt", gb_save_root());
    before[0] = 0;
    if (!fopen_s(&f, file, "rb") && f) {
        got = fread(before, 1, sizeof before - 1, f);
        before[got] = 0;
        fclose(f);
    }
    if (!strcmp(now, before))
        return;
    if (gb_save_backup_tagged("mods-changed"))
        gb_log("[MODS] the mods in use changed since the last start; the save was copied "
               "to backups first (folder ending _mods-changed)\n");
    if (s_nmods) {
        if (!fopen_s(&f, file, "wb") && f) {
            fwrite(now, 1, strlen(now), f);
            fclose(f);
        }
    } else
        DeleteFileA(file);
}

void gb_mods_init(void)
{
    WCHAR dir[MAX_PATH];
    char shown_a[MAX_PATH], shown[MAX_PATH], line[1024];
    const char *e = getenv("GB_MODS_DIR");
    const char *off = getenv("GB_MODS");
    DWORD a;
    int at, i;

    if (e && e[0]) {
        char full[MAX_PATH];
        GetFullPathNameA(e, sizeof full, full, NULL);
        MultiByteToWideChar(CP_ACP, 0, full, -1, dir, MAX_PATH);
    } else {
        char base[MAX_PATH], *cut;
        snprintf(base, sizeof base, "%s", gb_save_root());
        cut = strrchr(base, '\\');
        if (cut)
            *cut = 0;
        snprintf(shown_a, sizeof shown_a, "%s\\mods", base);
        MultiByteToWideChar(CP_ACP, 0, shown_a, -1, dir, MAX_PATH);
    }
    a = GetFileAttributesW(dir);
    WideCharToMultiByte(CP_ACP, 0, dir, -1, shown_a, sizeof shown_a, NULL, NULL);
    gb_log_user_path(shown_a, shown, sizeof shown);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        /* No mods folder: nothing to do, unless the last start had mods
         * (its record is in the save folder), when the save is protected. */
        char rec[MAX_PATH];
        snprintf(rec, sizeof rec, "%s\\mods_in_use.txt", gb_save_root());
        if (GetFileAttributesA(rec) != INVALID_FILE_ATTRIBUTES)
            note_mods_in_use();
        return;
    }
    if (off && off[0] == '0') {
        gb_log("[MODS] mods folder %s; every mod off (GB_MODS=0)\n", shown);
        note_mods_in_use();
        return;
    }
    wcscpy_s(s_dir, MAX_PATH, dir);
    {
        WCHAR cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);    /* main.c has moved to the folder holding game_files */
        _snwprintf_s(s_disc, MAX_PATH, _TRUNCATE, L"%s\\game_files", cwd);
    }
    if (!add_listed(dir))
        add_all(dir);
    build_table();
    at = snprintf(line, sizeof line, "[MODS] mods folder %s; ", shown);
    if (!s_nmods)
        snprintf(line + at, sizeof line - (size_t)at, "none on");
    for (i = 0; i < s_nmods && at > 0 && at < (int)sizeof line - 2; i++)
        at += snprintf(line + at, sizeof line - (size_t)at, "%s%s (%d file%s)",
                       i ? ", " : "on, first first: ", s_mods[i].name,
                       s_mods[i].files, s_mods[i].files == 1 ? "" : "s");
    gb_log("%s\n", line);
    note_mods_in_use();
    merge_all();
}

/* ── the open calls ───────────────────────────────────────── */

/* The disc path the title asked for, relative and lower case, or 0 when it
 * is not one a mod can replace. *skip is how many bytes of the title's own
 * string the relative path starts after. */
static int disc_relative(const char *p, int len, char *rel, int size, int *skip)
{
    static const char *const prefix[] = {
        "\\??\\D:\\", "D:\\", "\\Device\\CdRom0\\",
    };
    int k = 0, i, n;

    for (i = 0; i < (int)(sizeof prefix / sizeof prefix[0]); i++) {
        n = (int)strlen(prefix[i]);
        if (len > n && !_strnicmp(p, prefix[i], (size_t)n)) {
            k = n;
            break;
        }
    }
    if (!k || len - k >= size)
        return 0;
    for (i = k, n = 0; i < len; i++, n++) {
        char c = p[i];
        if (c == '/')
            c = '\\';
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (!c || c == ':' || (c == '\\' && n == 0))
            return 0;
        rel[n] = c;
    }
    rel[n] = 0;
    if (strstr(rel, ".."))
        return 0;
    *skip = k;
    return 1;
}

static void open_through_mods(uint32_t slot, int create)
{
    recomp_func_t real = recomp_lookup_kernel(MEM32(slot));
    uint32_t access = MEM32(g_esp + 8);
    uint32_t oa = MEM32(g_esp + 12);
    uint32_t disp = create ? MEM32(g_esp + 32) : 1;
    uint32_t opts = create ? MEM32(g_esp + 36) : MEM32(g_esp + 24);
    uint32_t str, buf, old_root, saved[10];
    int words = create ? 10 : 7;          /* return address and arguments */
    uint16_t len, max;
    char rel[MAX_PATH], asked[MAX_PATH];
    gb_mod_file *hit;
    const char *from;
    int skip, i;

    if (!real)
        return;
    /* Reads of files only: FILE_OPEN, no write, append, delete or generic
     * write or all access, not a directory. */
    if (!oa || disp != 1 || (access & 0x50010006u) || (opts & 1u)
            || (MEM32(oa) && MEM32(oa) != 0xFFFFFFFDu)) {
        real();
        return;
    }
    str = MEM32(oa + 4);
    if (!str || !(buf = MEM32(str + 4))) {
        real();
        return;
    }
    len = MEM16(str);
    max = MEM16(str + 2);
    if (len >= sizeof asked || len > max) {
        real();
        return;
    }
    memcpy(asked, MEM8P(buf), len);
    asked[len] = 0;
    if (!disc_relative(asked, len, rel, sizeof rel, &skip) || !(hit = find(rel))) {
        real();
        return;
    }

    /* The arguments, for a second try from the disc: the call pops them. */
    for (i = 0; i < words; i++)
        saved[i] = MEM32(g_esp + 4 * i);
    old_root = MEM32(oa);
    from = hit->mod == MERGED ? "the merged archives (mods\\.merged)" : s_mods[hit->mod].name;
    MEM32(oa) = (uint32_t)(uintptr_t)(hit->mod == MERGED ? s_merged_root : s_mods[hit->mod].root);
    MEM16(str) = (uint16_t)(len - skip);
    MEM16(str + 2) = (uint16_t)(max - skip);
    MEM32(str + 4) = buf + (uint32_t)skip;
    real();
    MEM32(oa) = old_root;
    MEM16(str) = len;
    MEM16(str + 2) = max;
    MEM32(str + 4) = buf;

    if (g_eax & 0x80000000u) {
        uint32_t st = g_eax;
        if (log_line_ok())
            gb_log("[MODS] %s from %s did not open (status 0x%08X); the disc's file was used\n",
                   asked, from, st);
        g_esp -= 4u * (uint32_t)words;      /* the call popped them; put them back */
        for (i = 0; i < words; i++)
            MEM32(g_esp + 4 * i) = saved[i];
        real();
        return;
    }
    if (!InterlockedExchange(&hit->told, 1) && log_line_ok())
        gb_log("[MODS] %s from %s\n", asked, from);
}

static void gb_NtOpenFile(void)
{
    open_through_mods(GB_KTHUNK_NTOPENFILE, 0);
}

static void gb_NtCreateFile(void)
{
    open_through_mods(GB_KTHUNK_NTCREATEFILE, 1);
}

void (*gb_mods_lookup(uint32_t xbox_va))(void)
{
    if (!s_nfiles)
        return (recomp_func_t)0;
    if (xbox_va == MEM32(GB_KTHUNK_NTCREATEFILE))
        return gb_NtCreateFile;
    if (xbox_va == MEM32(GB_KTHUNK_NTOPENFILE))
        return gb_NtOpenFile;
    return (recomp_func_t)0;
}

/* ── the mods in use, for other parts of the port ─────────── */

int gb_mods_count(void)
{
    return s_nmods;
}

const char *gb_mods_name(int i)
{
    return i >= 0 && i < s_nmods ? s_mods[i].name : NULL;
}

const wchar_t *gb_mods_folder(int i, wchar_t *out, int cap)
{
    WCHAR name[NAME_LEN];

    if (i < 0 || i >= s_nmods || !out || cap <= 0)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s_mods[i].name, -1, name, NAME_LEN);
    _snwprintf_s(out, (size_t)cap, _TRUNCATE, L"%s\\%s", s_dir, name);
    return out;
}
