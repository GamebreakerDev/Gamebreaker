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

#define TITLE L"Gamebreaker"
#define APP_DIR L"app"
#define HOST_REL L"launcher\\prototype\\host.ps1"
#define LOG_REL L"launcher\\prototype\\last-run.txt"
#define PAGE_REL L"launcher\\mockups\\halo.html"
#define SETUP_REL L"setup\\setup.ps1"

/* Exit code host.ps1 uses after it has already shown its own message. */
#define HOST_REPORTED 3

static wchar_t g_dir[32768];


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

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    (void)inst; (void)prev; (void)show;
    static wchar_t self[32768], host[32768], page[32768], setup[32768], ps[MAX_PATH + 64], cmd[40000], msg[8192];

    DWORD n = GetModuleFileNameW(NULL, self, 32768);
    if (n == 0 || n >= 32768) { say(L"Gamebreaker could not find its own folder."); return 2; }
    wcscpy(g_dir, self);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;

    /* A development checkout has the files right here; a release keeps
       everything but this program in its app folder. */
    join(host, 32768, g_dir, HOST_REL);
    if (!exists(host)) {
        wcscat(g_dir, L"\\" APP_DIR);
        join(host, 32768, g_dir, HOST_REL);
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

    UINT sl = GetSystemDirectoryW(ps, MAX_PATH);
    if (sl == 0 || sl >= MAX_PATH) { say(L"Gamebreaker could not find the Windows folder."); return 2; }
    wcscat(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    if (!exists(ps)) {
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
