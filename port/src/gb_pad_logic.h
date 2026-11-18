/* gb_pad_logic.h - the rules of the controller layer, with no device calls.
 *
 * gb_pad.c reads the host pads (XInput, or a test file) and asks this file
 * what they mean: which player a pad belongs to, how its buttons map onto
 * the original Xbox pad, where the trigger threshold is, how strong the
 * rumble is. Keeping the rules here lets port\tests\pad_logic_test.c check
 * them without a game, a window or a controller.
 *
 * Built on the toolkit's mapping module (xboxrecomp\src\input\input_map.h),
 * whose control and source names are also the names in controls.ini. */
#ifndef GB_PAD_LOGIC_H
#define GB_PAD_LOGIC_H

#include <stdint.h>
#include "input_map.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GB_PAD_PLAYERS 4
#define GB_PAD_SLOTS   4

enum { GB_PRESET_ORIGINAL, GB_PRESET_TURBO };
enum { GB_ORDER_AUTO, GB_ORDER_SLOTS };

typedef struct GbPadPlayerCfg {
    InputPadMap map;          /* sources, sticks, dead zones, trigger threshold */
    InputBindings keys;       /* the keys that drive this player when the keyboard does */
    uint8_t     preset;       /* GB_PRESET_*, as last applied */
    uint8_t     rumble_pct;   /* 0 = off, 100 = the game's own strength */
} GbPadPlayerCfg;

typedef struct GbPadConfig {
    GbPadPlayerCfg player[GB_PAD_PLAYERS];
    int order;                /* GB_ORDER_* */
    int keyboard_player;      /* 1..4, or 0 for none */
    int background;           /* 1 = pads still count when the window is not in front */
    int count;                /* emulated pads presented at boot: 1..4, 0 = one per connected slot */
    int warnings;             /* unknown names and bad values met while loading */
} GbPadConfig;

/* The defaults: preset original, dead zone 0
 * (the game has its own), trigger threshold 12 % (30 of 255), rumble 100 %,
 * order auto, keyboard on player 1, no background input, four pads; every
 * player's keys are the toolkit's keyboard map (keyboard.c). */
void gb_pad_config_defaults(GbPadConfig *c);

/* Set one player's sources to a preset (the dead zones and thresholds are
 * left alone). Returns 0, or -1 for an unknown name. */
int  gb_pad_preset_from_name(const char *name);
void gb_pad_apply_preset(GbPadPlayerCfg *p, int preset);

/* Read an INI text ("[all]", "[player1]".."[player4]", "key = value",
 * ';' or '#' comments) on top of what *c holds. Unknown sections, keys and
 * values are counted in c->warnings and reported through `warn` if it is
 * not NULL. Within each player, [all] is applied first and the player's own
 * section second; within a section the preset comes before single controls,
 * whatever their order in the file. key_<control> = <keys> sets the keys for
 * one control (key_a = Z, key_start = Enter, key_left_stick_up = W, Numpad8;
 * none clears it); controls without such a line keep their default keys. */
typedef void (*gb_pad_warn_fn)(const char *msg);
void gb_pad_config_parse(GbPadConfig *c, const char *text, gb_pad_warn_fn warn);

/* One setting for every player (the launcher's environment overrides use
 * the same keys as [all]). Returns 0 or -1 for an unknown key or value. */
int  gb_pad_config_set_all(GbPadConfig *c, const char *key, const char *value);

/* ---- the player table (R2) ---------------------------------------------- */

typedef struct GbPadTable {
    int8_t slot[GB_PAD_PLAYERS];       /* host slot driving the player, -1 none */
    int8_t last_slot[GB_PAD_PLAYERS];  /* the slot it had last, -1 never */
    int    order;                      /* GB_ORDER_* */
} GbPadTable;

void gb_pad_table_init(GbPadTable *t, int order);
/* A slot appeared. Returns the player (0..3) it now drives, or -1 if every
 * player has a pad. Auto order: a player without a pad whose last slot was
 * this one gets it back; otherwise the lowest player without a pad. A slot
 * that is already assigned keeps its player. */
int  gb_pad_table_connect(GbPadTable *t, int slot);
/* A slot went away. Returns the player that lost it, or -1. The player keeps
 * the slot as its last one. */
int  gb_pad_table_disconnect(GbPadTable *t, int slot);
int  gb_pad_table_player_of(const GbPadTable *t, int slot);

/* ---- one read ----------------------------------------------------------- */

/* A host pad's raw values, XInput's shape: the XINPUT_GAMEPAD_* button bits,
 * triggers 0..255, sticks signed with Y up. */
typedef struct GbHostPad {
    uint16_t buttons;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
} GbHostPad;

/* XInput's button bits (xinput.h), here so the tests need no Windows SDK. */
#define GB_XI_DPAD_UP        0x0001
#define GB_XI_DPAD_DOWN      0x0002
#define GB_XI_DPAD_LEFT      0x0004
#define GB_XI_DPAD_RIGHT     0x0008
#define GB_XI_START          0x0010
#define GB_XI_BACK           0x0020
#define GB_XI_LEFT_THUMB     0x0040
#define GB_XI_RIGHT_THUMB    0x0080
#define GB_XI_LEFT_SHOULDER  0x0100
#define GB_XI_RIGHT_SHOULDER 0x0200
#define GB_XI_A              0x1000
#define GB_XI_B              0x2000
#define GB_XI_X              0x4000
#define GB_XI_Y              0x8000

/* The host pad as the toolkit's InputRaw (every source 0..32767, the stick
 * directions filled in as sources too, so they can be bound to buttons). */
void gb_pad_to_raw(const GbHostPad *h, InputRaw *raw);
/* The Xbox pad the player sees from this host pad. */
void gb_pad_map(const GbPadPlayerCfg *p, const GbHostPad *h, XBOX_GAMEPAD *out);
/* Exactly what the toolkit's xinput_device.c made of a pad (no threshold,
 * no dead zone, LB = Black, RB = White): the GB_PADS=0 path. */
void gb_pad_map_toolkit(const GbHostPad *h, XBOX_GAMEPAD *out);
/* A motor speed scaled by percent, clamped to 0..65535. */
uint16_t gb_pad_rumble_scale(uint16_t speed, int pct);

#ifdef __cplusplus
}
#endif

#endif /* GB_PAD_LOGIC_H */
