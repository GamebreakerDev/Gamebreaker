/* gb_pad_logic.c - the rules of the controller layer (see gb_pad_logic.h).
 * No device calls and no globals: everything works on the caller's structs. */
#include "gb_pad_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- presets and defaults ------------------------------------------------ */

void gb_pad_apply_preset(GbPadPlayerCfg *p, int preset)
{
    InputPadMap d;
    xbox_InputMap_PadMapDefaults(&d);           /* the identity layout: LB Black, RB White */
    memcpy(p->map.source, d.source, sizeof p->map.source);
    p->map.left_stick = INPUT_STICK_LEFT;
    p->map.right_stick = INPUT_STICK_RIGHT;
    if (preset == GB_PRESET_TURBO) {
        /* The game's third turbo is the left stick click, awkward to hold
         * while steering with the same stick: move it to LB, and Black
         * (cancel Gamebreaker) to the stick click. */
        p->map.source[INPUT_LEFT_THUMB] = INPUT_SRC_LEFT_SHOULDER;
        p->map.source[INPUT_BLACK] = INPUT_SRC_LEFT_STICK_CLICK;
    }
    p->preset = (uint8_t)preset;
}

int gb_pad_preset_from_name(const char *name)
{
    if (!name) return -1;
#ifdef _MSC_VER
    if (!_stricmp(name, "original")) return GB_PRESET_ORIGINAL;
    if (!_stricmp(name, "turbo")) return GB_PRESET_TURBO;
#else
    if (!strcasecmp(name, "original")) return GB_PRESET_ORIGINAL;
    if (!strcasecmp(name, "turbo")) return GB_PRESET_TURBO;
#endif
    return -1;
}

/* The default keyboard layout ("Turbo count"): W A S D move, the four face
 * buttons on I J K L in the pad's own diamond (I top = Y, J left = X,
 * K bottom = A, L right = B), and three turbo keys that say how many turbos:
 * the game counts held turbos ("any", "two", "all") and never asks which one,
 * so Left Shift presses one trigger, Space both, and the semicolon key both
 * triggers and the left stick click (the third turbo). At most four keys are
 * ever held for any move, which ordinary keyboards register. */
static const struct { int control; const char *keys; } k_default_keys[] = {
    { INPUT_DPAD_UP, "up" }, { INPUT_DPAD_DOWN, "down" },
    { INPUT_DPAD_LEFT, "left" }, { INPUT_DPAD_RIGHT, "right" },
    { INPUT_START, "enter" }, { INPUT_BACK, "backspace, escape" },
    { INPUT_LEFT_THUMB, "semicolon" }, { INPUT_RIGHT_THUMB, "U" },
    { INPUT_A, "K" }, { INPUT_B, "L" }, { INPUT_X, "J" }, { INPUT_Y, "I" },
    { INPUT_BLACK, "E" }, { INPUT_WHITE, "Q" },
    { INPUT_LEFT_TRIGGER, "lshift, space, semicolon" },
    { INPUT_RIGHT_TRIGGER, "space, semicolon" },
    { INPUT_LSTICK_UP, "W, numpad8" }, { INPUT_LSTICK_DOWN, "S, numpad2" },
    { INPUT_LSTICK_LEFT, "A, numpad4" }, { INPUT_LSTICK_RIGHT, "D, numpad6" },
    { INPUT_RSTICK_UP, "T" }, { INPUT_RSTICK_DOWN, "G" },
    { INPUT_RSTICK_LEFT, "F" }, { INPUT_RSTICK_RIGHT, "H" },
};

void gb_pad_keys_defaults(InputBindings *b)
{
    size_t i;
    memset(b, 0, sizeof *b);
    b->diagonal = INPUT_DIAGONAL_FULL;   /* as the toolkit's map */
    for (i = 0; i < sizeof k_default_keys / sizeof k_default_keys[0]; i++)
        xbox_InputMap_BindingsParse(b, k_default_keys[i].control, k_default_keys[i].keys);
}

void gb_pad_config_defaults(GbPadConfig *c)
{
    int i;
    memset(c, 0, sizeof *c);
    for (i = 0; i < GB_PAD_PLAYERS; i++) {
        GbPadPlayerCfg *p = &c->player[i];
        xbox_InputMap_PadMapDefaults(&p->map);
        gb_pad_apply_preset(p, GB_PRESET_ORIGINAL);
        /* The game zeroes the inner 25 % of each axis itself: a host dead zone
         * would stack on it. Microsoft's trigger threshold is 30 of 255. */
        p->map.deadzone_left = 0;
        p->map.deadzone_right = 0;
        p->map.trigger_threshold = 12;
        p->rumble_pct = 100;
        gb_pad_keys_defaults(&p->keys);
    }
    c->order = GB_ORDER_AUTO;
    c->keyboard_player = 1;
    c->background = 0;
    c->count = 4;
}

/* ---- keys by position ---------------------------------------------------- */

/* The typing block of a US keyboard: scan code (set 1) and the virtual key a
 * US layout gives it. */
static const struct { uint8_t scan, vk; } k_us_block[] = {
    { 0x02, '1' }, { 0x03, '2' }, { 0x04, '3' }, { 0x05, '4' }, { 0x06, '5' },
    { 0x07, '6' }, { 0x08, '7' }, { 0x09, '8' }, { 0x0A, '9' }, { 0x0B, '0' },
    { 0x0C, XBOX_VK_OEM_MINUS }, { 0x0D, XBOX_VK_OEM_PLUS },
    { 0x10, 'Q' }, { 0x11, 'W' }, { 0x12, 'E' }, { 0x13, 'R' }, { 0x14, 'T' }, { 0x15, 'Y' },
    { 0x16, 'U' }, { 0x17, 'I' }, { 0x18, 'O' }, { 0x19, 'P' },
    { 0x1A, XBOX_VK_OEM_4 }, { 0x1B, XBOX_VK_OEM_6 },
    { 0x1E, 'A' }, { 0x1F, 'S' }, { 0x20, 'D' }, { 0x21, 'F' }, { 0x22, 'G' }, { 0x23, 'H' },
    { 0x24, 'J' }, { 0x25, 'K' }, { 0x26, 'L' },
    { 0x27, XBOX_VK_OEM_1 }, { 0x28, XBOX_VK_OEM_7 }, { 0x29, XBOX_VK_OEM_3 },
    { 0x2B, XBOX_VK_OEM_5 },
    { 0x2C, 'Z' }, { 0x2D, 'X' }, { 0x2E, 'C' }, { 0x2F, 'V' }, { 0x30, 'B' }, { 0x31, 'N' },
    { 0x32, 'M' }, { 0x33, XBOX_VK_OEM_COMMA }, { 0x34, XBOX_VK_OEM_PERIOD }, { 0x35, XBOX_VK_OEM_2 },
    { 0x56, 0xE2 },   /* VK_OEM_102: the ISO key left of Z (no name in controls.ini) */
};
#define US_BLOCK_COUNT (sizeof k_us_block / sizeof k_us_block[0])

int gb_keys_pos_vk(unsigned scan, int extended)
{
    size_t i;
    if (extended)
        return 0;        /* keypad / and the like: their own codes, same on every layout */
    for (i = 0; i < US_BLOCK_COUNT; i++)
        if (k_us_block[i].scan == scan)
            return k_us_block[i].vk;
    return 0;
}

int gb_keys_pos_scan(int vk)
{
    size_t i;
    for (i = 0; i < US_BLOCK_COUNT; i++)
        if (k_us_block[i].vk == vk)
            return k_us_block[i].scan;
    return 0;
}

/* A control's keys as parsed from a letter-written line: each typing-block
 * key moves to where that letter is on the player's layout. A letter the
 * layout lacks keeps its US place. Returns the number of keys that moved. */
static int keys_from_letters(InputBindings *b, int control, gb_keys_scan_fn scan_of)
{
    int i, moved = 0;
    if (!scan_of)
        return 0;
    for (i = 0; i < b->count[control]; i++) {
        int vk = b->key[control][i], s, pos;
        if (!gb_keys_pos_scan(vk))
            continue;
        s = scan_of(vk);
        pos = s ? gb_keys_pos_vk((unsigned)s & 0xFF, 0) : 0;
        if (pos && pos != vk) {
            b->key[control][i] = (uint8_t)pos;
            moved++;
        }
    }
    return moved;
}

/* ---- settings ------------------------------------------------------------ */

static int eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* A whole decimal number in lo..hi, or -1. */
static int number(const char *v, int lo, int hi)
{
    char *end;
    long n;
    if (!v || !*v) return -1;
    n = strtol(v, &end, 10);
    if (*end || n < lo || n > hi) return -1;
    return (int)n;
}

static int on_off(const char *v)
{
    if (eq(v, "1") || eq(v, "on") || eq(v, "yes") || eq(v, "true")) return 1;
    if (eq(v, "0") || eq(v, "off") || eq(v, "no") || eq(v, "false")) return 0;
    return -1;
}

/* Settings that belong to the whole layer, not to one player. */
static int set_global(GbPadConfig *c, const char *k, const char *v)
{
    int n;
    if (eq(k, "order")) {
        if (eq(v, "auto")) c->order = GB_ORDER_AUTO;
        else if (eq(v, "slots")) c->order = GB_ORDER_SLOTS;
        else return -1;
        return 0;
    }
    if (eq(k, "keyboard_player")) {
        if (eq(v, "off") || eq(v, "none")) { c->keyboard_player = 0; return 0; }
        if ((n = number(v, 0, 4)) < 0) return -1;
        c->keyboard_player = n;
        return 0;
    }
    if (eq(k, "background_input")) {
        if ((n = on_off(v)) < 0) return -1;
        c->background = n;
        return 0;
    }
    if (eq(k, "count")) {
        if (eq(v, "auto")) { c->count = 0; return 0; }
        if ((n = number(v, 1, 4)) < 0) return -1;
        c->count = n;
        return 0;
    }
    return 1;   /* not a global key */
}

static int is_global(const char *k)
{
    return eq(k, "order") || eq(k, "keyboard_player") || eq(k, "background_input")
        || eq(k, "count");
}

/* One player setting. Returns 0, -1 for a bad value, 1 for an unknown key.
 * `letters`: the line's section names keys by letter (see keys_from_letters);
 * `moved` counts the keys that moved. */
static int set_player_ex(GbPadPlayerCfg *p, const char *k, const char *v,
                         gb_keys_scan_fn letters, int *moved, int *key_control)
{
    int n, ctl, src;
    if (!strncmp(k, "key_", 4) || !strncmp(k, "KEY_", 4)) {
        InputBindings before = p->keys;
        ctl = xbox_InputMap_ControlFromName(k + 4);
        if (ctl < 0) return 1;
        if (xbox_InputMap_BindingsParse(&p->keys, ctl, v) != 0) {
            p->keys = before;            /* a name that is no key changes nothing */
            return -1;
        }
        n = keys_from_letters(&p->keys, ctl, letters);
        if (moved) *moved += n;
        if (key_control) *key_control = ctl;
        return 0;
    }
    if (eq(k, "preset")) {
        if ((n = gb_pad_preset_from_name(v)) < 0) return -1;
        gb_pad_apply_preset(p, n);
        return 0;
    }
    if (eq(k, "stick_deadzone")) {
        if ((n = number(v, 0, 95)) < 0) return -1;
        p->map.deadzone_left = p->map.deadzone_right = (uint8_t)n;
        return 0;
    }
    if (eq(k, "trigger_threshold")) {
        if ((n = number(v, 0, 95)) < 0) return -1;
        p->map.trigger_threshold = (uint8_t)n;
        return 0;
    }
    if (eq(k, "rumble")) {
        if (eq(v, "off")) n = 0;
        else if (eq(v, "on")) n = 100;
        else if ((n = number(v, 0, 100)) < 0) return -1;
        p->rumble_pct = (uint8_t)n;
        return 0;
    }
    if (eq(k, "left_stick") || eq(k, "right_stick")) {
        uint8_t *dst = eq(k, "left_stick") ? &p->map.left_stick : &p->map.right_stick;
        if (eq(v, "left")) *dst = INPUT_STICK_LEFT;
        else if (eq(v, "right")) *dst = INPUT_STICK_RIGHT;
        else if (eq(v, "none")) *dst = INPUT_STICK_NONE;
        else return -1;
        return 0;
    }
    ctl = xbox_InputMap_ControlFromName(k);
    if (ctl >= 0 && ctl < INPUT_PAD_CONTROL_COUNT) {
        if (eq(v, "none")) src = INPUT_SRC_NONE;
        else src = xbox_InputMap_SourceFromName(v);
        if (src < 0) return -1;
        p->map.source[ctl] = (uint8_t)src;
        return 0;
    }
    return 1;
}

static int set_player(GbPadPlayerCfg *p, const char *k, const char *v)
{
    return set_player_ex(p, k, v, NULL, NULL, NULL);
}

int gb_pad_config_set_all(GbPadConfig *c, const char *key, const char *value)
{
    int r = set_global(c, key, value), i;
    if (r <= 0)
        return r;
    for (i = 0; i < GB_PAD_PLAYERS; i++) {
        r = set_player(&c->player[i], key, value);
        if (r)
            return -1;
    }
    return 0;
}

#define GB_INI_MAX 256
typedef struct { int section; char key[40]; char value[80]; int line; } IniEntry;

static void warnf(GbPadConfig *c, gb_pad_warn_fn warn, const char *fmt, int line,
                  const char *a, const char *b)
{
    char msg[200];
    c->warnings++;
    if (!warn) return;
    snprintf(msg, sizeof msg, fmt, line, a ? a : "", b ? b : "");
    warn(msg);
}

static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

void gb_pad_config_parse(GbPadConfig *c, const char *text, gb_pad_warn_fn warn)
{
    static IniEntry e[GB_INI_MAX];   /* one config load at a time (start-up, tests) */
    int ne = 0, section = 0, line = 0, p, s, pass, i;
    int by_letter[1 + GB_PAD_PLAYERS];
    const char *q = text;

    while (q && *q) {
        char buf[200], *eqs;
        size_t n = strcspn(q, "\r\n");
        line++;
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, q, n);
        buf[n] = 0;
        q += strcspn(q, "\r\n");
        if (*q == '\r') q++;
        if (*q == '\n') q++;
        buf[strcspn(buf, ";#")] = 0;
        trim(buf);
        if (!buf[0])
            continue;
        if (buf[0] == '[') {
            char *close = strchr(buf, ']');
            if (close) *close = 0;
            if (eq(buf + 1, "all") || eq(buf + 1, "pad")) section = 0;
            else if (!strncmp(buf + 1, "player", 6) && buf[7] >= '1' && buf[7] <= '4' && !buf[8])
                section = buf[7] - '0';
            else {
                section = -1;
                warnf(c, warn, "line %d: section [%s] is not read%s", line, buf + 1, NULL);
            }
            continue;
        }
        if (section < 0)
            continue;
        eqs = strchr(buf, '=');
        if (!eqs) {
            warnf(c, warn, "line %d: no '=' in \"%s\"%s", line, buf, NULL);
            continue;
        }
        *eqs = 0;
        if (ne >= GB_INI_MAX) {
            warnf(c, warn, "line %d: too many settings, the rest are ignored%s%s", line, NULL, NULL);
            break;
        }
        e[ne].section = section;
        e[ne].line = line;
        snprintf(e[ne].key, sizeof e[ne].key, "%s", buf);
        snprintf(e[ne].value, sizeof e[ne].value, "%s", eqs + 1);
        trim(e[ne].key);
        trim(e[ne].value);
        ne++;
    }

    /* Whole-layer settings, from [all] only. */
    for (i = 0; i < ne; i++) {
        if (!is_global(e[i].key))
            continue;
        if (e[i].section != 0)
            warnf(c, warn, "line %d: %s applies to every player; put it in [all]%s",
                  e[i].line, e[i].key, NULL);
        else if (set_global(c, e[i].key, e[i].value) < 0)
            warnf(c, warn, "line %d: %s = \"%s\" is not a valid value", e[i].line,
                  e[i].key, e[i].value);
    }
    /* How each section names its keys: by letter unless it says otherwise. */
    for (i = 0; i < 1 + GB_PAD_PLAYERS; i++)
        by_letter[i] = 1;
    for (i = 0; i < ne; i++) {
        if (!eq(e[i].key, "keys_by"))
            continue;
        if (eq(e[i].value, "position")) by_letter[e[i].section] = 0;
        else if (eq(e[i].value, "letter")) by_letter[e[i].section] = 1;
        else warnf(c, warn, "line %d: %s = \"%s\" is not a valid value", e[i].line,
                   e[i].key, e[i].value);
    }
    /* Per player: [all], then [playerN]; in each, the preset first. Warnings
     * are reported in the first player's pass only, so each is said once
     * (a [playerN]-only line is checked in its own player's pass). */
    for (p = 0; p < GB_PAD_PLAYERS; p++) {
        uint32_t listed = 0;         /* controls given keys by a line */
        int letter_lines = 0;        /* of them, lines written by letter */
        for (s = 0; s <= 1; s++) {
            int want = s ? p + 1 : 0;
            for (pass = 0; pass <= 1; pass++) {
                for (i = 0; i < ne; i++) {
                    int r, moved = 0, ctl = -1;
                    if (e[i].section != want || is_global(e[i].key) || eq(e[i].key, "keys_by"))
                        continue;
                    if ((pass == 0) != eq(e[i].key, "preset"))
                        continue;
                    r = set_player_ex(&c->player[p], e[i].key, e[i].value,
                                      by_letter[want] ? c->letter_scan : NULL, &moved, &ctl);
                    if (ctl >= 0) {
                        listed |= 1u << ctl;
                        letter_lines += by_letter[want];
                    }
                    if (s == 1 || p == 0)
                        c->letter_keys += moved;
                    if (r && (s == 1 || p == 0))
                        warnf(c, warn, r > 0 ? "line %d: unknown setting %s%s"
                                             : "line %d: %s = \"%s\" is not a valid value",
                              e[i].line, e[i].key, r > 0 ? NULL : e[i].value);
                }
            }
        }
        /* A player who rebound keys in a file written by letter keeps the
         * whole keyboard as it was: the controls the file does not list keep
         * their default letters too, each moved to that letter's place. */
        if (letter_lines && c->letter_scan) {
            int ctl;
            for (ctl = 0; ctl < INPUT_CONTROL_COUNT; ctl++)
                if (!(listed & (1u << ctl)))
                    c->letter_keys += keys_from_letters(&c->player[p].keys, ctl, c->letter_scan);
        }
    }
}

/* ---- the player table ---------------------------------------------------- */

void gb_pad_table_init(GbPadTable *t, int order)
{
    int i;
    for (i = 0; i < GB_PAD_PLAYERS; i++) {
        t->slot[i] = -1;
        t->last_slot[i] = -1;
    }
    t->order = order;
}

int gb_pad_table_player_of(const GbPadTable *t, int slot)
{
    int i;
    for (i = 0; i < GB_PAD_PLAYERS; i++)
        if (t->slot[i] == slot)
            return i;
    return -1;
}

int gb_pad_table_connect(GbPadTable *t, int slot)
{
    int i = gb_pad_table_player_of(t, slot);
    if (slot < 0 || slot >= GB_PAD_SLOTS)
        return -1;
    if (i >= 0)
        return i;
    if (t->order == GB_ORDER_SLOTS) {
        i = slot;                        /* player n is slot n-1, the toolkit's rule */
    } else {
        for (i = 0; i < GB_PAD_PLAYERS; i++)
            if (t->slot[i] < 0 && t->last_slot[i] == slot)
                break;                   /* the pad's own player, back after a re-plug */
        if (i == GB_PAD_PLAYERS)
            for (i = 0; i < GB_PAD_PLAYERS; i++)
                if (t->slot[i] < 0)
                    break;               /* first come: the lowest player without a pad */
        if (i == GB_PAD_PLAYERS)
            return -1;
    }
    t->slot[i] = (int8_t)slot;
    t->last_slot[i] = (int8_t)slot;
    return i;
}

int gb_pad_table_disconnect(GbPadTable *t, int slot)
{
    int i = gb_pad_table_player_of(t, slot);
    if (i >= 0)
        t->slot[i] = -1;                 /* last_slot stays: it is the reservation */
    return i;
}

/* ---- one read ------------------------------------------------------------ */

static int16_t pos(int v) { return (int16_t)(v > 0 ? (v > 32767 ? 32767 : v) : 0); }

void gb_pad_to_raw(const GbHostPad *h, InputRaw *raw)
{
    static const struct { uint16_t bit; uint8_t src; } b[] = {
        { GB_XI_A, INPUT_SRC_SOUTH }, { GB_XI_B, INPUT_SRC_EAST },
        { GB_XI_X, INPUT_SRC_WEST }, { GB_XI_Y, INPUT_SRC_NORTH },
        { GB_XI_BACK, INPUT_SRC_BACK }, { GB_XI_START, INPUT_SRC_START },
        { GB_XI_LEFT_THUMB, INPUT_SRC_LEFT_STICK_CLICK },
        { GB_XI_RIGHT_THUMB, INPUT_SRC_RIGHT_STICK_CLICK },
        { GB_XI_LEFT_SHOULDER, INPUT_SRC_LEFT_SHOULDER },
        { GB_XI_RIGHT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER },
        { GB_XI_DPAD_UP, INPUT_SRC_DPAD_UP }, { GB_XI_DPAD_DOWN, INPUT_SRC_DPAD_DOWN },
        { GB_XI_DPAD_LEFT, INPUT_SRC_DPAD_LEFT }, { GB_XI_DPAD_RIGHT, INPUT_SRC_DPAD_RIGHT },
    };
    size_t i;
    memset(raw, 0, sizeof *raw);
    for (i = 0; i < sizeof b / sizeof b[0]; i++)
        if (h->buttons & b[i].bit)
            raw->src[b[i].src] = 32767;
    /* 0..255 to 0..32767 such that the mapping module's ">> 7" gives the
     * byte back exactly (b * 32767 / 255 >= b * 128 for every b). */
    raw->src[INPUT_SRC_LEFT_TRIGGER] = (int16_t)(h->lt * 32767 / 255);
    raw->src[INPUT_SRC_RIGHT_TRIGGER] = (int16_t)(h->rt * 32767 / 255);
    raw->src[INPUT_SRC_LSTICK_UP] = pos(h->ly);
    raw->src[INPUT_SRC_LSTICK_DOWN] = pos(-(int)h->ly);
    raw->src[INPUT_SRC_LSTICK_LEFT] = pos(-(int)h->lx);
    raw->src[INPUT_SRC_LSTICK_RIGHT] = pos(h->lx);
    raw->src[INPUT_SRC_RSTICK_UP] = pos(h->ry);
    raw->src[INPUT_SRC_RSTICK_DOWN] = pos(-(int)h->ry);
    raw->src[INPUT_SRC_RSTICK_LEFT] = pos(-(int)h->rx);
    raw->src[INPUT_SRC_RSTICK_RIGHT] = pos(h->rx);
    raw->lx = h->lx;
    raw->ly = h->ly;
    raw->rx = h->rx;
    raw->ry = h->ry;
}

void gb_pad_map(const GbPadPlayerCfg *p, const GbHostPad *h, XBOX_GAMEPAD *out)
{
    InputRaw raw;
    gb_pad_to_raw(h, &raw);
    xbox_InputMap_PadMap(&p->map, &raw, out);
    /* The mapping module's radial dead zone also clips the stick to a circle,
     * even at 0 % (a full diagonal, 32767 both ways, comes out as 23170).
     * With no host dead zone the stick goes to the game untouched, as the
     * toolkit's own path sends it. */
    if (p->map.deadzone_left == 0) {
        out->sThumbLX = p->map.left_stick == INPUT_STICK_LEFT ? h->lx
                      : p->map.left_stick == INPUT_STICK_RIGHT ? h->rx : 0;
        out->sThumbLY = p->map.left_stick == INPUT_STICK_LEFT ? h->ly
                      : p->map.left_stick == INPUT_STICK_RIGHT ? h->ry : 0;
    }
    if (p->map.deadzone_right == 0) {
        out->sThumbRX = p->map.right_stick == INPUT_STICK_LEFT ? h->lx
                      : p->map.right_stick == INPUT_STICK_RIGHT ? h->rx : 0;
        out->sThumbRY = p->map.right_stick == INPUT_STICK_LEFT ? h->ly
                      : p->map.right_stick == INPUT_STICK_RIGHT ? h->ry : 0;
    }
}

void gb_pad_map_toolkit(const GbHostPad *h, XBOX_GAMEPAD *out)
{
    /* xinput_device.c:102-124 at 343b6d8, line for line. */
    memset(out, 0, sizeof *out);
    out->wButtons = (WORD)(h->buttons & 0x00FF);
    out->bAnalogButtons[XBOX_BUTTON_A] = (h->buttons & GB_XI_A) ? 255 : 0;
    out->bAnalogButtons[XBOX_BUTTON_B] = (h->buttons & GB_XI_B) ? 255 : 0;
    out->bAnalogButtons[XBOX_BUTTON_X] = (h->buttons & GB_XI_X) ? 255 : 0;
    out->bAnalogButtons[XBOX_BUTTON_Y] = (h->buttons & GB_XI_Y) ? 255 : 0;
    out->bAnalogButtons[XBOX_BUTTON_BLACK] = (h->buttons & GB_XI_LEFT_SHOULDER) ? 255 : 0;
    out->bAnalogButtons[XBOX_BUTTON_WHITE] = (h->buttons & GB_XI_RIGHT_SHOULDER) ? 255 : 0;
    out->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = h->lt;
    out->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = h->rt;
    out->sThumbLX = h->lx;
    out->sThumbLY = h->ly;
    out->sThumbRX = h->rx;
    out->sThumbRY = h->ry;
}

uint16_t gb_pad_rumble_scale(uint16_t speed, int pct)
{
    long v;
    if (pct <= 0) return 0;
    v = (long)speed * pct / 100;
    return (uint16_t)(v > 65535 ? 65535 : v);
}
