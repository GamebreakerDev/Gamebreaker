/* Gamebreaker.exe: starts the launcher.
 *
 * The launcher is launcher\prototype\host.ps1 (readable PowerShell) showing
 * launcher\mockups\halo.html in Microsoft's WebView2. This program only:
 *   1. finds those files in the app folder next to itself (a release) or right
 *      next to itself (a development checkout), and Windows PowerShell 5.1;
 *   2. starts  powershell.exe -File launcher\prototype\host.ps1 -Live <its own arguments>
 *      with no console window;
 *   3. waits for it, and shows a plain message if anything is missing, if
 *      Windows refuses to start PowerShell, or if the launcher stops with an
 *      error it could not show itself.
 * Gamebreaker.cmd does steps 1 and 2 the same way and stays as the fallback.
 *
 * A single-program release (setup\make_release.ps1 -Single) is this same
 * program with the app folder appended as a zip and a trailer (see PAYLOAD_*).
 * When no launcher files are found beside it, such a program checks the zip's
 * SHA-256, unpacks it into app.partial, checks every file against the
 * release's own list (setup\manifest.json) and renames the folder to app.
 * It never writes into an existing app folder: an update may have left a
 * newer one there. Without an appended zip nothing of this runs.
 *
 * Built by launcher\starter\build.ps1 with the pinned llvm-mingw (no other
 * dependencies; only Windows' own DLLs). Exit code: the launcher's, or 2 when
 * it could not be started.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>
#include <string.h>
#include <bcrypt.h>

#define TITLE L"Gamebreaker"
#define APP_DIR L"app"
#define HOST_REL L"launcher\\prototype\\host.ps1"
#define LOG_REL L"launcher\\prototype\\last-run.txt"
#define PAGE_REL L"launcher\\mockups\\halo.html"
#define SETUP_REL L"setup\\setup.ps1"

/* Exit code host.ps1 uses after it has already shown its own message. */
#define HOST_REPORTED 3

static wchar_t g_dir[32768];

/* The trailer at the very end of a single-program release:
   "GBAPPZIP" (8 bytes), the zip's offset and length (little-endian 64-bit),
   the zip's SHA-256 (32 bytes), "GBAPPEND" (8 bytes). 64 bytes in all. */
#define PAYLOAD_TRAILER 64
#define PAYLOAD_MAX (256u * 1024u * 1024u)


static void say(const wchar_t *text)
{
    MessageBoxW(NULL, text, TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static void join(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *rel)
{
    _snwprintf(out, cap, L"%ls\\%ls", dir, rel);
    out[cap - 1] = 0;
}

/* The last few lines of the launcher's own log, for the message box. */
static void log_tail(wchar_t *out, size_t cap)
{
    wchar_t path[32768];
    out[0] = 0;
    join(path, 32768, g_dir, LOG_REL);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    static char buf[4096];
    DWORD got = 0;
    if (GetFileSizeEx(f, &size)) {
        LONGLONG start = size.QuadPart > (LONGLONG)sizeof(buf) - 1 ? size.QuadPart - (LONGLONG)sizeof(buf) + 1 : 0;
        LARGE_INTEGER at; at.QuadPart = start;
        SetFilePointerEx(f, at, NULL, FILE_BEGIN);
        ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    }
    CloseHandle(f);
    buf[got] = 0;
    char *p = buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    /* keep the last 8 lines */
    int lines = 0;
    char *q = buf + got;
    while (q > p) { if (*--q == '\n' && ++lines > 8) { p = q + 1; break; } }
    MultiByteToWideChar(CP_UTF8, 0, p, -1, out, (int)cap);
    out[cap - 1] = 0;
}

/* Single-program release: the appended zip, or NULL when there is none (a
   normal starter) or it is damaged (*bad set). The caller frees it. */
static unsigned char *read_payload(const wchar_t *self, DWORD *len, int *bad)
{
    *len = 0; *bad = 0;
    HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    unsigned char *data = NULL;
    LARGE_INTEGER size, at;
    unsigned char t[PAYLOAD_TRAILER];
    DWORD got = 0;
    if (!GetFileSizeEx(f, &size) || size.QuadPart < PAYLOAD_TRAILER) goto done;
    at.QuadPart = size.QuadPart - PAYLOAD_TRAILER;
    if (!SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !ReadFile(f, t, PAYLOAD_TRAILER, &got, NULL) || got != PAYLOAD_TRAILER) goto done;
    if (memcmp(t, "GBAPPZIP", 8) || memcmp(t + 56, "GBAPPEND", 8)) goto done;    /* a normal starter */
    *bad = 1;
    unsigned long long off = 0, n = 0;
    for (int i = 7; i >= 0; i--) { off = (off << 8) | t[8 + i]; n = (n << 8) | t[16 + i]; }
    if (n == 0 || n > PAYLOAD_MAX || off + n + PAYLOAD_TRAILER != (unsigned long long)size.QuadPart) goto done;
    data = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n);
    if (!data) goto done;
    at.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !ReadFile(f, data, (DWORD)n, &got, NULL) || got != (DWORD)n) goto fail;
    /* SHA-256 with Windows' own CNG (bcrypt.dll). */
    {
        BCRYPT_ALG_HANDLE alg = NULL;
        BCRYPT_HASH_HANDLE h = NULL;
        unsigned char sum[32];
        int ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
                 BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0 &&
                 BCryptHashData(h, data, (ULONG)n, 0) == 0 &&
                 BCryptFinishHash(h, sum, 32, 0) == 0;
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        if (!ok || memcmp(sum, t + 24, 32)) goto fail;
    }
    *len = (DWORD)n; *bad = 0;
    goto done;
fail:
    HeapFree(GetProcessHeap(), 0, data);
    data = NULL;
done:
    CloseHandle(f);
    return data;
}

/* PowerShell's single quotes: ' becomes ''. */
static void ps_quote(wchar_t *out, size_t cap, const wchar_t *in)
{
    size_t j = 0;
    for (; *in && j + 2 < cap; in++) { if (*in == L'\'') out[j++] = L'\''; out[j++] = *in; }
    out[j] = 0;
}

/* Unpacks the appended zip into <dir>\app (via app.partial). 0 when done;
   otherwise a message for the player is in msg. */
static int unpack_app(const wchar_t *dir, const wchar_t *ps, const unsigned char *zip, DWORD len, wchar_t *msg, size_t cap)
{
    static wchar_t zp[32768], ep[32768], q[32768], cmd[40000];
    static char err[2048];
    _snwprintf(zp, 32768, L"%ls\\app.partial.zip", dir); zp[32767] = 0;
    _snwprintf(ep, 32768, L"%ls\\app.partial.err", dir); ep[32767] = 0;
    DeleteFileW(ep);
    HANDLE f = CreateFileW(zp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, zip, len, &put, NULL) || put != len) {
        if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); DeleteFileW(zp); }
        _snwprintf(msg, cap, L"Gamebreaker could not write its files in this folder:\n%ls\n\n"
                             L"Put Gamebreaker.exe in a folder of its own where you can save files "
                             L"(for example a new folder in Documents) and start it from there.", dir);
        msg[cap - 1] = 0;
        return 1;
    }
    CloseHandle(f);
    ps_quote(q, 32768, dir);
    /* Unpack, check every file of the release's list, then rename. A folder
       left by an earlier attempt (app.partial) is ours and is replaced. */
    _snwprintf(cmd, 40000,
        L"\"%ls\" -NoLogo -NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue';"
        L" $d='%ls'; $p=Join-Path $d 'app.partial';"
        L" try { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force };"
        L" Expand-Archive -LiteralPath (Join-Path $d 'app.partial.zip') -DestinationPath $p;"
        L" $m = Get-Content -LiteralPath (Join-Path $p 'setup\\manifest.json') -Raw | ConvertFrom-Json;"
        L" foreach ($f in @($m.files)) { if ($f.path -ieq 'Gamebreaker.exe') { continue };"
        L" $x = Join-Path $p ($f.path -replace '/', '\\');"
        L" if (-not (Test-Path -LiteralPath $x) -or (Get-FileHash -LiteralPath $x -Algorithm SHA256).Hash.ToLowerInvariant() -ne $f.sha256) { throw ('A file did not unpack correctly: ' + $f.path) } };"
        L" Remove-Item -LiteralPath (Join-Path $d 'app.partial.zip') -Force;"
        L" Rename-Item -LiteralPath $p 'app' }"
        L" catch { [IO.File]::WriteAllText((Join-Path $d 'app.partial.err'), $_.Exception.Message); exit 1 }\"",
        ps, q);
    cmd[39999] = 0;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_FORCEONFEEDBACK;
    DWORD code = 1;
    if (CreateProcessW(ps, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
    }
    DeleteFileW(zp);
    if (code == 0) return 0;
    err[0] = 0;
    HANDLE e = CreateFileW(ep, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (e != INVALID_HANDLE_VALUE) {
        DWORD g = 0;
        ReadFile(e, err, sizeof err - 1, &g, NULL);
        err[g] = 0;
        CloseHandle(e);
        DeleteFileW(ep);
    }
    static wchar_t why[2048];
    MultiByteToWideChar(CP_UTF8, 0, err, -1, why, 2048);
    why[2047] = 0;
    _snwprintf(msg, cap, L"Gamebreaker could not unpack its files into:\n%ls\\app\n\n%ls\n\n"
                         L"Put Gamebreaker.exe in a folder of its own where you can save files and start it again.",
               dir, why[0] ? why : L"(no reason given)");
    msg[cap - 1] = 0;
    return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    (void)inst; (void)prev; (void)show;
    static wchar_t self[32768], host[32768], page[32768], setup[32768], ps[MAX_PATH + 64], cmd[40000], msg[8192];

    DWORD n = GetModuleFileNameW(NULL, self, 32768);
    if (n == 0 || n >= 32768) { say(L"Gamebreaker could not find its own folder."); return 2; }
    wcscpy(g_dir, self);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;

    UINT sl = GetSystemDirectoryW(ps, MAX_PATH);
    if (sl == 0 || sl >= MAX_PATH) { say(L"Gamebreaker could not find the Windows folder."); return 2; }
    wcscat(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    int have_ps = exists(ps);

    /* A development checkout has the files right here; a release keeps
       everything but this program in its app folder. */
    join(host, 32768, g_dir, HOST_REL);
    if (!exists(host)) {
        static wchar_t main_dir[32768], app_dir[32768];
        wcscpy(main_dir, g_dir);
        wcscat(g_dir, L"\\" APP_DIR);
        join(host, 32768, g_dir, HOST_REL);
        join(app_dir, 32768, main_dir, APP_DIR);
        /* A single-program release with no app folder yet: unpack it. */
        if (!exists(app_dir)) {
            DWORD zlen = 0; int bad = 0;
            unsigned char *zip = read_payload(self, &zlen, &bad);
            if (bad) {
                say(L"This copy of Gamebreaker.exe is damaged (its files do not match their checksum).\n\n"
                    L"Download it again.");
                return 2;
            }
            if (zip) {
                int r = have_ps ? unpack_app(main_dir, ps, zip, zlen, msg, 8192) : -1;
                HeapFree(GetProcessHeap(), 0, zip);
                if (r > 0) { say(msg); return 2; }
            }
        } else {
            /* Left over if a first start was stopped between its last two steps. */
            static wchar_t left[32768];
            join(left, 32768, main_dir, L"app.partial.zip"); DeleteFileW(left);
            join(left, 32768, main_dir, L"app.partial.err"); DeleteFileW(left);
        }
    }
    join(page, 32768, g_dir, PAGE_REL);
    join(setup, 32768, g_dir, SETUP_REL);
    if (!exists(host) || !exists(page) || !exists(setup)) {
        _snwprintf(msg, 8192,
            L"The launcher files are missing next to Gamebreaker.exe.\n\n"
            L"Unpack the whole zip into a folder, keeping the app folder beside Gamebreaker.exe, "
            L"then start Gamebreaker.exe from that folder.\n\n"
            L"Missing: %ls",
            !exists(host) ? host : !exists(page) ? page : setup);
        msg[8191] = 0;
        say(msg);
        return 2;
    }

    if (!have_ps) {
        _snwprintf(msg, 8192,
            L"Windows PowerShell was not found:\n%ls\n\n"
            L"The launcher needs it. It is part of Windows 10 and 11; if it was removed, "
            L"turn it back on under Windows' optional features.", ps);
        msg[8191] = 0;
        say(msg);
        return 2;
    }

    _snwprintf(cmd, 40000, L"\"%ls\" -NoLogo -NoProfile -ExecutionPolicy Bypass -File \"%ls\" -Live %ls",
               ps, host, args ? args : L"");
    cmd[39999] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_FORCEONFEEDBACK;    /* busy cursor until the launcher's window shows */
    if (!CreateProcessW(ps, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi)) {
        DWORD e = GetLastError();
        wchar_t why[1024] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, why, 1024, NULL);
        if (e == ERROR_ACCESS_DISABLED_BY_POLICY || e == 4551 /* ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION */)
            _snwprintf(msg, 8192, L"Windows blocked Windows PowerShell, which the launcher needs (error %lu).\n\n%ls\n"
                                  L"A security policy on this PC does not allow it.", e, why);
        else
            _snwprintf(msg, 8192, L"Windows PowerShell could not be started (error %lu).\n\n%ls", e, why);
        msg[8191] = 0;
        say(msg);
        return 2;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    if (code != 0 && code != HOST_REPORTED) {
        static wchar_t tail[4096];
        log_tail(tail, 4096);
        _snwprintf(msg, 8192, L"The launcher stopped with exit code %lu.\n\n"
                              L"Last lines of %ls:\n\n%ls", code, LOG_REL, tail[0] ? tail : L"(no log)");
        msg[8191] = 0;
        say(msg);
    }
    return (int)code;
}
