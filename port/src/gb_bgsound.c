/* gb_bgsound.c - the game's sound while its window is not in front.
 *
 * GB_BG_SOUND=0: while another window is in front, this process's audio
 * session volume in Windows' mixer is set to 0; it comes back when the game's
 * window is in front again and when the game ends. Unset or any other value:
 * the sound keeps playing, and nothing here runs.
 *
 * Only the mixer's level for this program changes: the emulated audio chip
 * and the game go on as before, so the music is where it should be on return.
 * The mixer's mute switch is left alone.
 *
 * Windows remembers a program's mixer level by its path, so a run that ends
 * while silent (a crash, a killed process) would start the next run silent.
 * The level to come back to is therefore written to a small file beside the
 * program before it is lowered, and the next start puts it back, whatever
 * GB_BG_SOUND says then.
 *
 * COM stays on a thread of its own, so no game or window thread ever waits
 * for the audio service. If the default output device changes while the game
 * is in the background, the new device's session is not lowered. */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_log.h"

void gb_bgsound_boot(void);
void gb_bgsound_window(HWND hwnd);
void gb_bgsound_shutdown(void);

/* The SDK headers declare these without defining them; their published values. */
static const GUID bg_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID bg_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID bg_IID_IAudioSessionManager =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

static char   s_marker[MAX_PATH];      /* <program>.bgvolume: the level to come back to */
static int    s_on;                    /* GB_BG_SOUND=0 */
static HWND volatile s_hwnd;
static HANDLE s_stop, s_thread;


/* This process's session on the default output device, or NULL. */
static ISimpleAudioVolume *session_volume(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioSessionManager *mgr = NULL;
    ISimpleAudioVolume *vol = NULL;
    HRESULT hr = CoCreateInstance(&bg_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &bg_IID_IMMDeviceEnumerator, (void **)&en);
    if (SUCCEEDED(hr))
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr))
        hr = IMMDevice_Activate(dev, &bg_IID_IAudioSessionManager, CLSCTX_ALL, NULL, (void **)&mgr);
    if (SUCCEEDED(hr))
        hr = IAudioSessionManager_GetSimpleAudioVolume(mgr, NULL, FALSE, &vol);
    if (FAILED(hr))
        vol = NULL;
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return vol;
}

/* Written through a temporary name, so a crash never leaves half a number. */
static int marker_write(float level)
{
    char tmp[MAX_PATH + 4], text[32];
    HANDLE f;
    DWORD n = 0;
    int len = snprintf(text, sizeof text, "%.4f\n", level);
    BOOL ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", s_marker);
    f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(f, text, (DWORD)len, &n, NULL) && n == (DWORD)len;
    CloseHandle(f);
    return ok && MoveFileExA(tmp, s_marker, MOVEFILE_REPLACE_EXISTING);
}

static int marker_read(float *level)
{
    char text[32];
    DWORD n = 0;
    double v;
    char *end;
    HANDLE f = CreateFileA(s_marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(f, text, sizeof text - 1, &n, NULL))
        n = 0;
    CloseHandle(f);
    text[n] = 0;
    v = strtod(text, &end);
    if (end == text || v < 0.0 || v > 1.0)
        return 0;
    *level = (float)v;
    return 1;
}

/* Lower the session. Returns 1 when it is silent and *saved holds the level. */
static int silence(float *saved)
{
    ISimpleAudioVolume *vol = session_volume();
    float now = 0.0f;
    int ok = 0;
    if (!vol)
        return 0;
    if (SUCCEEDED(ISimpleAudioVolume_GetMasterVolume(vol, &now))) {
        /* The file first: the level is never lowered without a way back. */
        if (marker_write(now)
            && SUCCEEDED(ISimpleAudioVolume_SetMasterVolume(vol, 0.0f, NULL))) {
            *saved = now;
            ok = 1;
        } else {
            DeleteFileA(s_marker);
        }
    }
    ISimpleAudioVolume_Release(vol);
    return ok;
}

/* Bring the level back, unless the player moved the mixer's slider meanwhile. */
static void restore(float saved)
{
    ISimpleAudioVolume *vol = session_volume();
    float now = -1.0f;
    if (vol) {
        if (SUCCEEDED(ISimpleAudioVolume_GetMasterVolume(vol, &now)) && now == 0.0f)
            ISimpleAudioVolume_SetMasterVolume(vol, saved, NULL);
        ISimpleAudioVolume_Release(vol);
    }
    DeleteFileA(s_marker);
}

static int window_in_front(void)
{
    HWND h = s_hwnd;
    if (!h)
        return 1;                      /* no window yet: nothing to be behind */
    return GetForegroundWindow() == h;
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
    float saved = 0.0f;
    int silent = 0;
    (void)arg;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
        return 0;
    for (;;) {
        int front = window_in_front();
        if (!front && !silent)
            silent = silence(&saved);
        else if (front && silent) {
            restore(saved);
            silent = 0;
        }
        if (WaitForSingleObject(s_stop, 100) == WAIT_OBJECT_0)
            break;
    }
    if (silent)
        restore(saved);
    CoUninitialize();
    return 0;
}

/* A run that ended while silent left its file: the level comes back first. */
static DWORD WINAPI recover_thread(LPVOID arg)
{
    float level = 1.0f;
    int have = marker_read(&level);
    (void)arg;
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        restore(have ? level : 1.0f);
        CoUninitialize();
    }
    return 0;
}

void gb_bgsound_boot(void)
{
    const char *v = getenv("GB_BG_SOUND");
    DWORD n = GetModuleFileNameA(NULL, s_marker, MAX_PATH - 12);
    if (!n || n >= MAX_PATH - 12) {
        s_marker[0] = 0;
        return;
    }
    strcat(s_marker, ".bgvolume");
    if (GetFileAttributesA(s_marker) != INVALID_FILE_ATTRIBUTES) {
        /* Before the game starts its sound, so the first note is at the right level. */
        HANDLE t = CreateThread(NULL, 0, recover_thread, NULL, 0, NULL);
        if (t) {
            WaitForSingleObject(t, 3000);
            CloseHandle(t);
        }
    }
    s_on = v && v[0] == '0' && !v[1];
    if (s_on)
        gb_log("[GB] sound in the background: off\n");
}

void gb_bgsound_window(HWND hwnd)
{
    if (!s_on || !s_marker[0] || s_thread)
        return;
    s_hwnd = hwnd;
    s_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (s_stop)
        s_thread = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    if (s_thread)
        atexit(gb_bgsound_shutdown);
}

/* The level comes back before the program ends. Safe to call more than once,
 * from any thread. */
void gb_bgsound_shutdown(void)
{
    static volatile LONG once;
    if (!s_thread || InterlockedExchange(&once, 1))
        return;
    SetEvent(s_stop);
    WaitForSingleObject(s_thread, 1500);
}
