/* gb_pad.c - the host controllers behind the emulated Xbox pads.
 *
 * The toolkit models the Xbox's USB controller chip with Xbox pads plugged
 * into it (xboxrecomp\src\usb\), and each emulated pad asks one function,
 * xbox_InputGetState(n), what is held; rumble comes back through
 * xbox_InputSetState(n). The toolkit's own versions (src\input\xinput_device.c)
 * map host XInput slot n to pad n. This file defines all five public names of
 * that file, so the linker never takes the toolkit's object out of its library
 * and these run instead. The toolkit is not changed.
 *
 * GB_PADS unset or 0: the toolkit's behaviour (slot n is pad n, raw triggers,
 * keyboard on pad 1), with the keys of controls.ini if it sets any. GB_PADS=1: the controller layer: four pads
 * presented, pads become players in
 * the order they arrive and keep them, a remapping file, a trigger
 * threshold, rumble strength, the keyboard on any player, pads ignored while
 * the window is not in front, unplug and re-plug at any time.
 *
 * Threads: xbox_InputGetState and xbox_InputSetState are called only from the
 * toolkit's USB thread, which owns every table here. The window thread only
 * hands over its window and a "devices changed" flag; gb_pad_shutdown may run
 * on any thread at exit and only switches motors off. */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_log.h"
#include "gb_pad_logic.h"
#include "keyboard.h"      /* the held-key table behind the keyboard pad (RECOMP_KEYBOARD) */
#include "xinput_xbox.h"

void gb_pad_boot(void);
void gb_pad_window(HWND hwnd);
void gb_pad_devices_changed(void);
void gb_pad_shutdown(void);
unsigned gb_pad_notice(char *buf, int size);

static int         s_mode;               /* 0 = the toolkit's behaviour, 1 = this layer */
static int         s_booted;
static GbPadConfig s_cfg;
static GbPadTable  s_table;
static LARGE_INTEGER s_qpf;

typedef struct {
    int       connected;
    ULONGLONG read_at;       /* ms of the last read, so a slot is read at most once a ms */
    GbHostPad pad;
} Slot;
static Slot      s_slot[GB_PAD_SLOTS];
static int       s_probe_next;
static ULONGLONG s_probe_at;
static ULONGLONG s_update_at = (ULONGLONG)-1;
static volatile LONG s_devchange;

static HWND volatile s_hwnd;
static int       s_focus = 1;            /* until a window is handed over */
static ULONGLONG s_focus_at;

static XBOX_VIBRATION s_want[GB_PAD_PLAYERS];  /* per player, scaled */
static XBOX_VIBRATION s_sent[GB_PAD_SLOTS];    /* per slot, as last sent */
static DWORD     s_packet[GB_PAD_PLAYERS];

static volatile LONG s_notice_gen;
static char      s_notice[160];

static ULONGLONG now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (ULONGLONG)(t.QuadPart * 1000 / s_qpf.QuadPart);
}



/* ======================================================================== */
/* Host slots                                                               */
/* ======================================================================== */

static int read_slot(int slot, GbHostPad *pad)
{
    XINPUT_STATE xs;
    if (XInputGetState((DWORD)slot, &xs) != ERROR_SUCCESS) {
        memset(pad, 0, sizeof *pad);
        return 0;
    }
    pad->buttons = xs.Gamepad.wButtons;
    pad->lt = xs.Gamepad.bLeftTrigger;
    pad->rt = xs.Gamepad.bRightTrigger;
    pad->lx = xs.Gamepad.sThumbLX;
    pad->ly = xs.Gamepad.sThumbLY;
    pad->rx = xs.Gamepad.sThumbRX;
    pad->ry = xs.Gamepad.sThumbRY;
    return 1;
}

static void send_motor(int slot, const XBOX_VIBRATION *v)
{
    if (s_sent[slot].wLeftMotorSpeed == v->wLeftMotorSpeed
        && s_sent[slot].wRightMotorSpeed == v->wRightMotorSpeed)
        return;
    s_sent[slot] = *v;
    {
        XINPUT_VIBRATION xv;
        xv.wLeftMotorSpeed = v->wLeftMotorSpeed;
        xv.wRightMotorSpeed = v->wRightMotorSpeed;
        XInputSetState((DWORD)slot, &xv);
    }
}

static void notice_update(void)
{
    char buf[sizeof s_notice];
    int p, n = 0;
    buf[0] = 0;
    for (p = 0; p < GB_PAD_PLAYERS; p++)
        if (s_table.slot[p] < 0 && s_table.last_slot[p] >= 0)
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%sController %d disconnected",
                          n ? ", " : "", p + 1);
    if (strcmp(buf, s_notice)) {
        memcpy(s_notice, buf, sizeof buf);
        InterlockedIncrement(&s_notice_gen);
    }
}


static void slot_connected(int slot)
{
    int p;
    s_slot[slot].connected = 1;
    memset(&s_sent[slot], 0xFF, sizeof s_sent[slot]);   /* unknown: the next value is sent */
    p = gb_pad_table_connect(&s_table, slot);
    if (p < 0) {
        return;
    }
    if (s_focus || s_cfg.background)
        send_motor(slot, &s_want[p]);
    notice_update();
}

static void slot_gone(int slot)
{
    int p = gb_pad_table_disconnect(&s_table, slot);   /* the player it was */
    (void)p;
    s_slot[slot].connected = 0;
    memset(&s_slot[slot].pad, 0, sizeof s_slot[slot].pad);
    {
        XBOX_VIBRATION off = { 0, 0 };
        send_motor(slot, &off);           /* a pad that comes back must not shake */
    }
    notice_update();
}

static void focus_check(ULONGLONG now)
{
    HWND h = s_hwnd;
    int f, p;
    if (!h || now - s_focus_at < 100)
        return;
    s_focus_at = now;
    f = GetForegroundWindow() == h;
    if (f == s_focus)
        return;
    s_focus = f;
    if (s_cfg.background)
        return;
    for (p = 0; p < GB_PAD_PLAYERS; p++) {
        int slot = s_table.slot[p];
        if (slot >= 0) {
            XBOX_VIBRATION off = { 0, 0 };
            send_motor(slot, f ? &s_want[p] : &off);
        }
    }
}

/* Read connected slots, probe one empty slot every 500 ms (all of them after
 * a device change), notice arrivals and departures. Once a ms at most. */
static void update(void)
{
    ULONGLONG now = now_ms();
    int i;
    if (now == s_update_at)
        return;
    s_update_at = now;
    focus_check(now);
    for (i = 0; i < GB_PAD_SLOTS; i++) {
        if (!s_slot[i].connected)
            continue;
        if (read_slot(i, &s_slot[i].pad))
            s_slot[i].read_at = now;
        else
            slot_gone(i);
    }
    if (InterlockedExchange(&s_devchange, 0)) {
        for (i = 0; i < GB_PAD_SLOTS; i++)
            if (!s_slot[i].connected && read_slot(i, &s_slot[i].pad))
                slot_connected(i);
        s_probe_at = now + 500;
    } else if (now >= s_probe_at) {
        for (i = 0; i < GB_PAD_SLOTS; i++) {
            int slot = (s_probe_next + i) % GB_PAD_SLOTS;
            if (s_slot[slot].connected)
                continue;
            s_probe_next = slot + 1;
            if (read_slot(slot, &s_slot[slot].pad))
                slot_connected(slot);
            break;
        }
        s_probe_at = now + 500;
    }
}

/* ======================================================================== */
/* Start-up                                                                  */
/* ======================================================================== */

static void warn_line(const char *m) { gb_log("[PAD] controls file: %s\n", m); }

static int file_beside_exe(const char *name, char *out, size_t size)
{
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)size);
    if (!n || n >= size) return 0;
    slash = strrchr(out, '\\');
    if (!slash || (size_t)(slash + 1 - out) + strlen(name) + 1 > size) return 0;
    strcpy(slash + 1, name);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

static void load_config(void)
{
    static const struct { const char *env, *key; } ov[] = {
        { "GB_PAD_PRESET", "preset" }, { "GB_PAD_TRIGGER", "trigger_threshold" },
        { "GB_PAD_DEADZONE", "stick_deadzone" }, { "GB_PAD_RUMBLE", "rumble" },
        { "GB_PAD_ORDER", "order" }, { "GB_PAD_KEYBOARD_PLAYER", "keyboard_player" },
        { "GB_PAD_BACKGROUND", "background_input" },
    };
    char path[MAX_PATH];
    const char *cfg = getenv("GB_PAD_CONFIG");
    size_t i;

    gb_pad_config_defaults(&s_cfg);
    if (cfg && *cfg)
        snprintf(path, sizeof path, "%s", cfg);
    else if (!file_beside_exe("controls.ini", path, sizeof path))
        path[0] = 0;
    if (path[0]) {
        FILE *f = fopen(path, "rb");
        if (f) {
            static char text[16384];
            size_t n = fread(text, 1, sizeof text - 1, f);
            fclose(f);
            text[n] = 0;
            gb_pad_config_parse(&s_cfg, text, warn_line);
        } else {
        }
    } else {
    }
    /* The launcher's overrides, for every player, after the file. */
    for (i = 0; i < sizeof ov / sizeof ov[0]; i++) {
        const char *v = getenv(ov[i].env);
        if (v && *v && gb_pad_config_set_all(&s_cfg, ov[i].key, v) != 0)
            gb_log("[PAD] %s=%s is not a valid value: ignored\n", ov[i].env, v);
    }
}


void gb_pad_boot(void)
{
    const char *v = getenv("GB_PADS");
    int i, n = 0;

    if (s_booted)
        return;
    s_booted = 1;
    QueryPerformanceFrequency(&s_qpf);
    s_mode = v && *v && *v != '0';
    load_config();                       /* the keyboard's keys count with the layer off too */
    if (!s_mode) {
        return;
    }
    gb_pad_table_init(&s_table, s_cfg.order);

    /* The pads already there, in slot order: the lowest is player 1. */
    for (i = 0; i < GB_PAD_SLOTS; i++)
        if (read_slot(i, &s_slot[i].pad)) {
            slot_connected(i);
            n = s_cfg.order == GB_ORDER_SLOTS ? i + 1 : n + 1;
        }
    s_probe_at = now_ms() + 500;

    /* How many emulated pads the toolkit plugs in at boot: it cannot plug
     * one later, so four lets a pad connected at any time join (R1). */
    {
        char num[4];
        int count = s_cfg.count ? s_cfg.count : (n ? n : 1);
        snprintf(num, sizeof num, "%d", count);
        _putenv_s("RECOMP_USB_PADS", num);
    }
    {
        char kb[16];
        if (s_cfg.keyboard_player)
            snprintf(kb, sizeof kb, "player %d", s_cfg.keyboard_player);
        else
            snprintf(kb, sizeof kb, "off");
    }
    atexit(gb_pad_shutdown);
}

void gb_pad_window(HWND hwnd)
{
    s_hwnd = hwnd;
}

void gb_pad_devices_changed(void)
{
    InterlockedExchange(&s_devchange, 1);
}

void gb_pad_shutdown(void)
{
    static volatile LONG once;
    int i;
    if (!s_mode || InterlockedExchange(&once, 1))
        return;
    for (i = 0; i < GB_PAD_SLOTS; i++) {
        XINPUT_VIBRATION off = { 0, 0 };
        XInputSetState((DWORD)i, &off);
    }
}

unsigned gb_pad_notice(char *buf, int size)
{
    /* Read on the window thread, written on the USB thread: a torn read only
     * shows for one frame, and the generation moves again with each change. */
    unsigned gen = (unsigned)s_notice_gen;
    if (buf && size > 0)
        snprintf(buf, (size_t)size, "%s", s_notice);
    return gen;
}

/* ======================================================================== */
/* The toolkit's five functions (xinput_xbox.h)                             */
/* ======================================================================== */

/* The keyboard as a pad for one player, through that player's own keys
 * (controls.ini, key_a = Z ...). With no such lines these are the toolkit's
 * keys, resolved the way xbox_InputKeyboardPoll resolves them. */
static int keyboard_poll(int player, XBOX_INPUT_STATE *out)
{
    static DWORD packet;
    uint8_t down[INPUT_KEY_COUNT];
    const char *v = getenv("RECOMP_KEYBOARD");
    int vk;
    if (!v || !*v || *v == '0')
        return 0;
    for (vk = 0; vk < INPUT_KEY_COUNT; vk++)
        down[vk] = (uint8_t)(xbox_FramebufferKeyDown(vk) != 0);
    memset(out, 0, sizeof *out);
    xbox_InputMap_KeysResolve(&s_cfg.player[player].keys, down, &out->Gamepad);
    out->dwPacketNumber = ++packet;      /* the title looks for edges by this number */
    return 1;
}

static BOOL s_toolkit_connected[XBOX_MAX_CONTROLLERS];

/* GB_PADS=0: xinput_device.c's Windows path at 343b6d8, without RECOMP_MKB. */
static DWORD toolkit_get_state(DWORD port, XBOX_INPUT_STATE *st)
{
    XINPUT_STATE xs;
    GbHostPad h;
    if (XInputGetState(port, &xs) != ERROR_SUCCESS) {
        s_toolkit_connected[port] = FALSE;
        if (port == 0) {
            /* The keyboard stands in for an absent pad. */
            memset(st, 0, sizeof *st);
            if (keyboard_poll(0, st))
                return ERROR_SUCCESS;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    s_toolkit_connected[port] = TRUE;
    memset(st, 0, sizeof *st);
    st->dwPacketNumber = xs.dwPacketNumber;
    h.buttons = xs.Gamepad.wButtons;
    h.lt = xs.Gamepad.bLeftTrigger;
    h.rt = xs.Gamepad.bRightTrigger;
    h.lx = xs.Gamepad.sThumbLX;
    h.ly = xs.Gamepad.sThumbLY;
    h.rx = xs.Gamepad.sThumbRX;
    h.ry = xs.Gamepad.sThumbRY;
    gb_pad_map_toolkit(&h, &st->Gamepad);
    if (port == 0) {
        XBOX_INPUT_STATE kb;
        if (keyboard_poll(0, &kb)) {
            int i;
            st->Gamepad.wButtons |= kb.Gamepad.wButtons;
            for (i = 0; i < 8; i++)
                if (kb.Gamepad.bAnalogButtons[i] > st->Gamepad.bAnalogButtons[i])
                    st->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
            if (kb.Gamepad.sThumbLX) st->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
            if (kb.Gamepad.sThumbLY) st->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
            if (kb.Gamepad.sThumbRX) st->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
            if (kb.Gamepad.sThumbRY) st->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
            st->dwPacketNumber = kb.dwPacketNumber;
        }
    }
    return ERROR_SUCCESS;
}

void xbox_InputInit(void)
{
    gb_pad_boot();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    int player, slot, got = 0;
    XBOX_GAMEPAD g;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!s_booted)
        gb_pad_boot();                    /* main.c calls it first; this is a fallback */
    if (!s_mode)
        return toolkit_get_state(dwPort, pState);

    update();
    player = (int)dwPort;
    memset(&g, 0, sizeof g);
    slot = s_table.slot[player];
    if (slot >= 0) {
        if (s_focus || s_cfg.background)
            gb_pad_map(&s_cfg.player[player], &s_slot[slot].pad, &g);
        got = 1;                          /* connected; neutral while gated */
    }
    if (s_cfg.keyboard_player == player + 1) {
        XBOX_INPUT_STATE kb;              /* only filled while the window has focus */
        if (keyboard_poll(player, &kb)) {
            xbox_InputMap_PadMerge(&g, &kb.Gamepad);
            got = 1;
        }
    }
    if (!got)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pState, 0, sizeof *pState);
    pState->dwPacketNumber = ++s_packet[player];
    pState->Gamepad = g;
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    int player, slot;
    XBOX_VIBRATION v;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!s_mode) {
        XINPUT_VIBRATION xv;
        xv.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
        xv.wRightMotorSpeed = pVibration->wRightMotorSpeed;
        return XInputSetState(dwPort, &xv);
    }
    player = (int)dwPort;
    v.wLeftMotorSpeed = gb_pad_rumble_scale(pVibration->wLeftMotorSpeed,
                                            s_cfg.player[player].rumble_pct);
    v.wRightMotorSpeed = gb_pad_rumble_scale(pVibration->wRightMotorSpeed,
                                             s_cfg.player[player].rumble_pct);
    s_want[player] = v;
    slot = s_table.slot[player];
    if (slot < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (s_focus || s_cfg.background)
        send_motor(slot, &v);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    if (!s_mode)
        return s_toolkit_connected[dwPort];
    return s_table.slot[dwPort] >= 0;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xc;
    int slot;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    slot = s_mode ? s_table.slot[dwPort] : (int)dwPort;
    if (slot < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (XInputGetCapabilities((DWORD)slot, dwFlags, &xc) != ERROR_SUCCESS)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof *pCaps);
    pCaps->Type = xc.Type;
    pCaps->SubType = xc.SubType;
    pCaps->Flags = xc.Flags;
    return ERROR_SUCCESS;
}
