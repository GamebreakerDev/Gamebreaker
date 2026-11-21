/*
 * The mouse in menus: GB_MOUSE_MENUS=1 (off by default).
 *
 * The game's menus were made for a controller. With this switch the mouse
 * works in them the way a controller does, by pressing the same buttons a
 * player would; the port never writes the game's own state, so a click can
 * only lead somewhere a controller can reach.
 *
 *   Known screens   the Main Menu and Play It. Pointing at an item moves
 *                   the highlight to it (hover), a left click moves the
 *                   highlight there and presses A, the wheel moves it up
 *                   and down. A click outside every item does nothing.
 *   Other screens   with no 3D scene (the title, every other menu): the
 *                   wheel is the D-pad's up and down, a left click is A.
 *   Everywhere      a right click is B (back), as Esc and Backspace are.
 *   In a match      (any frame with a 3D scene: the court, the pause menu
 *                   over it) the mouse does nothing and the pointer is
 *                   hidden.
 *   The pointer     shown over the game's picture while the mouse is in
 *                   use on a menu; hidden after a controller or key press
 *                   until the mouse moves again, in a window and in
 *                   fullscreen alike.
 *
 * How the port knows where things are. The disc's layout pack
 * (game_files\fe\xlay_fe.bin, the player's own file: nothing is shipped)
 * gives each screen's items: their point on the 640x480 menu screen and
 * which item is above and below. The game keeps no plain "highlighted item"
 * value in its memory that the port can read (memory snapshots taken with
 * the highlight on different items show none, and the layouts are freed once
 * a screen is built), so the port reads what is drawn: each frame the
 * Direct3D 11 back end hands over the small flat quads of the menu (the
 * labels' letters, and the star the game draws left of the highlighted
 * label). From them:
 *   - which known screen is up: the rows that have a label match the
 *     layout's items exactly (six rows for the Main Menu, four for Play It);
 *   - which item is highlighted: the row the star is on.
 * A click is then a closed loop: one move, wait until the star has moved
 * to the next row, the next move, and A only once the star sits on the
 * clicked row. If the screen stops being recognised, or the star does not
 * move in time, the rest is dropped (never queued): a click during a
 * transition does nothing.
 *
 * Presses go into pad 1 (gb_pad.c merges them in xbox_InputGetState), each
 * held for 50 ms and released for 50 ms so the menu sees every one.
 */
#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gb_log.h"

extern ptrdiff_t g_xbox_mem_offset;

int gb_d3d11_menu_point(int cx, int cy, float *gx, float *gy);   /* gb_d3d11.c */

void gb_mouse_init(void);
int  gb_mouse_enabled(void);
int  gb_mouse_window_msg(HWND h, UINT m, WPARAM w, LPARAM l);
int  gb_mouse_pointer_shown(void);
void gb_mouse_timer(HWND h);
void gb_mouse_key_pressed(void);
void gb_mouse_other_input(void);
int  gb_mouse_pad(uint16_t *buttons, uint8_t *analog_a, uint8_t *analog_b);
void gb_mouse_note_batch(const void *vertices, uint32_t count, uint32_t stride,
                         uint32_t surf_w, uint32_t surf_h, uint32_t aa_x, uint32_t aa_y);
void gb_mouse_frame_end(int scene3d);

static int s_on;                        /* GB_MOUSE_MENUS */
static LARGE_INTEGER s_qpf;

static double now_ms(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)s_qpf.QuadPart;
}

/* ── The layouts, read from the player's own disc ─────────────────── */

#define MAX_LAYOUTS 128
#define MAX_ITEMS   48

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

typedef struct {
    int n;                  /* element number (id >> 12): what links name */
    int index;              /* tag 0x22: the item's number in its menu */
    int x, y;               /* tag 0x03: the item's point, top left of its row */
    int link[4];            /* slot of the item each way, or -1 */
} Item;

typedef struct {
    int nitems;
    Item it[MAX_ITEMS];
} Layout;

static Layout s_layouts[MAX_LAYOUTS];
static int s_nlayouts;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }

/* One layout: u32 version, u16 count, (count + 1) x {u16 properties,
 * u32 offset} (the first is not an element), then per element its
 * properties as {u16 tag, u32 value}. Tag 0x00 the id (element number << 12
 * | type; types 0x0A0 to 0x0AF are items), 0x03 the point (x, y as two
 * u16), 0x22 the item's index, 0x19/0x1A up (kind, target), 0x1B/0x1C down,
 * 0x1D/0x1E left, 0x1F/0x20 right; kind 2 = go to element `target`. */
static void layout_parse(const uint8_t *e, uint32_t size, Layout *L)
{
    uint32_t cnt, k, j;
    int nel[MAX_ITEMS], linkn[MAX_ITEMS][4];
    memset(L, 0, sizeof *L);
    if (size < 6)
        return;
    cnt = rd16(e + 4);
    for (k = 0; k < cnt; k++) {
        uint32_t d = 6 + 6 * (k + 1), np, off, id = 0;
        int ln[4] = { -1, -1, -1, -1 }, lk[4] = { 0, 0, 0, 0 }, d4;
        Item it;
        if (d + 6 > size)
            break;
        np = rd16(e + d);
        off = rd32(e + d + 2);
        if (off > size || 6 * np > size - off)
            continue;
        memset(&it, 0, sizeof it);
        it.index = -1;
        for (j = 0; j < np; j++) {
            uint32_t t = rd16(e + off + 6 * j), v = rd32(e + off + 6 * j + 2);
            switch (t) {
            case 0x00: id = v; break;
            case 0x03: it.x = (int)(v & 0xFFFF); it.y = (int)(v >> 16); break;
            case 0x22: it.index = (int)v; break;
            case 0x19: lk[DIR_UP] = (int)v; break;
            case 0x1a: ln[DIR_UP] = (int)v; break;
            case 0x1b: lk[DIR_DOWN] = (int)v; break;
            case 0x1c: ln[DIR_DOWN] = (int)v; break;
            case 0x1d: lk[DIR_LEFT] = (int)v; break;
            case 0x1e: ln[DIR_LEFT] = (int)v; break;
            case 0x1f: lk[DIR_RIGHT] = (int)v; break;
            case 0x20: ln[DIR_RIGHT] = (int)v; break;
            }
        }
        if ((id & 0xFF0) != 0x0A0 || it.index < 0 || L->nitems >= MAX_ITEMS)
            continue;
        it.n = (int)(id >> 12);
        nel[L->nitems] = it.n;
        for (d4 = 0; d4 < 4; d4++) {
            linkn[L->nitems][d4] = lk[d4] == 2 ? ln[d4] : -1;
            it.link[d4] = -1;
        }
        L->it[L->nitems++] = it;
    }
    for (k = 0; k < (uint32_t)L->nitems; k++)
        for (j = 0; j < 4; j++) {
            int s;
            for (s = 0; s < L->nitems; s++)
                if (linkn[k][j] >= 0 && nel[s] == linkn[k][j])
                    L->it[k].link[j] = s;
        }
}

/* The layout pack: C0FB, big-endian u16 header size, u16 count, then per
 * entry a 3-byte offset, a 3-byte size and a NUL-terminated name (empty). */
static int pack_parse(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long len;
    uint32_t sz, n, p, k;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 6 || len > (16 << 20)) {
        fclose(f);
        return 0;
    }
    sz = (uint32_t)len;
    d = (uint8_t *)malloc(sz);
    if (!d || fread(d, 1, sz, f) != sz || d[0] != 0xC0 || d[1] != 0xFB) {
        fclose(f);
        free(d);
        return 0;
    }
    fclose(f);
    n = (uint32_t)d[4] << 8 | d[5];
    p = 6;
    for (k = 0; k < n && k < MAX_LAYOUTS && p + 6 <= sz; k++) {
        uint32_t off = (uint32_t)d[p] << 16 | (uint32_t)d[p + 1] << 8 | d[p + 2];
        uint32_t len2 = (uint32_t)d[p + 3] << 16 | (uint32_t)d[p + 4] << 8 | d[p + 5];
        p += 6;
        while (p < sz && d[p])
            p++;
        p++;
        if (off <= sz && len2 <= sz - off)
            layout_parse(d + off, len2, &s_layouts[k]);
    }
    free(d);
    return (int)k;
}

/* ── Known screens ─────────────────────────────────────────────────── */

/* A screen the port knows: a layout whose items form one column, each
 * item's row running from its point down to the next item's (44 pixels on
 * both screens). The label starts at the item's x, to the right of the
 * star. */
typedef struct {
    const char *name;
    int layout;             /* entry in fe\xlay_fe.bin */
    int nrows;              /* items, top to bottom */
    int x, y[8];            /* the column's x; each row's top */
    int row_h;
    int index[8];           /* the item's index (tag 0x22), per row */
} Screen;

static Screen s_screens[] = {
    { "Main Menu", 0 },
    { "Play It",   2 },
};
#define NSCREENS ((int)(sizeof s_screens / sizeof s_screens[0]))

#define LABEL_W     230     /* hit box: from the star's left to past the longest label */
#define STAR_LEFT   26      /* the star sits up to this far left of the item's x */

/* Sort the layout's items into rows. Only a single column whose items link
 * up and down to their neighbours is taken; anything else is left unknown. */
static void screens_build(void)
{
    int s;
    for (s = 0; s < NSCREENS; s++) {
        Screen *S = &s_screens[s];
        const Layout *L = &s_layouts[S->layout];
        int order[8], i, j, n = L->nitems, ok = n >= 2 && n <= 8;
        S->nrows = 0;
        for (i = 0; ok && i < n; i++) {
            order[i] = i;
            if (L->it[i].x != L->it[0].x)
                ok = 0;
        }
        for (i = 0; ok && i < n; i++)
            for (j = i + 1; j < n; j++)
                if (L->it[order[j]].y < L->it[order[i]].y) {
                    int t = order[i]; order[i] = order[j]; order[j] = t;
                }
        for (i = 0; ok && i + 1 < n; i++)
            if (L->it[order[i]].link[DIR_DOWN] != order[i + 1]
                || L->it[order[i + 1]].link[DIR_UP] != order[i])
                ok = 0;
        if (!ok) {
            gb_log("[MOUSE] %s: layout %d is not the column expected; screen not known\n",
                   S->name, S->layout);
            continue;
        }
        S->x = L->it[0].x;
        for (i = 0; i < n; i++) {
            S->y[i] = L->it[order[i]].y;
            S->index[i] = L->it[order[i]].index;
        }
        S->row_h = S->y[1] - S->y[0];
        S->nrows = n;
    }
}

/* ── What the back end saw drawn ───────────────────────────────────── */

/* Per frame: for each 8-pixel band of the menu screen's height, whether a
 * label letter (a small flat quad starting in the label column) was drawn,
 * and the rows the star was seen in. Built on the render thread, published
 * at the end of a frame. */
#define BANDS 60                         /* 480 / 8 */

typedef struct {
    uint32_t seq;
    int scene3d;                         /* the frame drew 3D */
    int screen;                          /* known screen recognised, or -1 */
    int star_row;                        /* the row the star is on, or -1 */
    double at;                           /* when it was published */
} Seen;

static CRITICAL_SECTION s_lock;
static Seen s_seen;                      /* the latest frame's, under s_lock */

static uint8_t s_text[BANDS];            /* building: label letters per band, per known column x */
static uint8_t s_star[BANDS];            /* building: star quads per band */
static int s_col_x = -1;                 /* the column all known screens share, or -1 */

typedef struct { float x, y, z, rhw; } VertexHead;   /* the start of Nv2aVertex */

void gb_mouse_note_batch(const void *vertices, uint32_t count, uint32_t stride,
                         uint32_t surf_w, uint32_t surf_h, uint32_t aa_x, uint32_t aa_y)
{
    uint32_t i, k;
    float sx, sy;
    if (!s_on || s_col_x < 0 || !count || !surf_w || !surf_h || !aa_x || !aa_y)
        return;
    /* only the screen (640x480 as the game sees it, in real pixels with its
     * anti-aliasing factor); other surfaces are textures */
    if (surf_w / aa_x != 640 || surf_h / aa_y != 480)
        return;
    sx = 640.0f / (float)surf_w;
    sy = 480.0f / (float)surf_h;
    for (i = 0; i < count; i++)
        if (((const VertexHead *)((const uint8_t *)vertices + (size_t)i * stride))->rhw != 1.0f)
            return;                      /* 3D: not a menu */
    for (i = 0; i + 2 < count; i += 3) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, w, h;
        int b0, b1;
        for (k = 0; k < 3; k++) {
            const VertexHead *v = (const VertexHead *)((const uint8_t *)vertices + (size_t)(i + k) * stride);
            float x = v->x * sx, y = v->y * sy;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        w = x1 - x0;
        h = y1 - y0;
        if (w <= 0.0f || h <= 0.0f || w > 40.0f || h > 40.0f || y0 < 0.0f || y1 >= 480.0f)
            continue;                    /* not a letter or a star */
        b0 = (int)(y0 / 8.0f);
        b1 = (int)(y1 / 8.0f);
        if (b1 >= BANDS) b1 = BANDS - 1;
        if (x1 <= (float)s_col_x + 2.0f && x0 >= (float)(s_col_x - STAR_LEFT)) {
            for (k = (uint32_t)b0; k <= (uint32_t)b1; k++)
                if (s_star[k] < 255) s_star[k]++;
        } else if (x0 >= (float)s_col_x - 2.0f && x0 <= (float)(s_col_x + LABEL_W)) {
            for (k = (uint32_t)b0; k <= (uint32_t)b1; k++)
                if (s_text[k] < 255) s_text[k]++;
        }
    }
}

/* A row "has a label" when letters were drawn in its upper half (the
 * labels' height is about half the row). */
static int row_has(const uint8_t *bands, int top, int h)
{
    int b0 = (top + 2) / 8, b1 = (top + h / 2) / 8, b, n = 0;
    for (b = b0; b <= b1 && b < BANDS; b++)
        n += bands[b];
    return n >= 2;
}

void gb_mouse_frame_end(int scene3d)
{
    Seen now;
    int s, r;
    if (!s_on)
        return;
    memset(&now, 0, sizeof now);
    now.scene3d = scene3d;
    now.screen = -1;
    now.star_row = -1;
    if (!scene3d && s_col_x >= 0) {
        for (s = 0; s < NSCREENS; s++) {
            const Screen *S = &s_screens[s];
            int ok = S->nrows > 0, star = -1, stars = 0;
            for (r = 0; ok && r < S->nrows; r++)
                if (!row_has(s_text, S->y[r], S->row_h))
                    ok = 0;
            /* the row below the last must be empty, so six rows are not
             * taken for four; not down at the help line (y 420), which
             * every menu has */
            if (ok && S->y[S->nrows - 1] + S->row_h + S->row_h / 2 < 410
                && row_has(s_text, S->y[S->nrows - 1] + S->row_h, S->row_h))
                ok = 0;
            if (!ok)
                continue;
            for (r = 0; r < S->nrows; r++)
                if (row_has(s_star, S->y[r], S->row_h)) {
                    star = r;
                    stars++;
                }
            now.screen = s;
            now.star_row = stars == 1 ? star : -1;
            break;
        }
    }
    now.at = now_ms();
    memset(s_text, 0, sizeof s_text);
    memset(s_star, 0, sizeof s_star);
    EnterCriticalSection(&s_lock);
    now.seq = s_seen.seq + 1;
    s_seen = now;
    LeaveCriticalSection(&s_lock);
}

/* ── The mouse, from the window ────────────────────────────────────── */

typedef struct {
    int has_point;                       /* the pointer is over the menu picture */
    float gx, gy;                        /* on the 640x480 menu screen */
    double moved_at;                     /* last real movement */
    int clicks_l, clicks_r, wheel;       /* not yet taken by the game side */
    int click_in;                        /* where the last left click was */
    float click_gx, click_gy;
} Mouse;

static Mouse s_mouse;                    /* under s_lock */
static volatile LONG s_pointer_hidden;   /* after a pad or key press */
static volatile LONG s_in_scene;         /* the last frame drew 3D */
static int s_last_cx = -100000, s_last_cy;

static void pointer_show(const char *why)
{
    if (InterlockedExchange(&s_pointer_hidden, 0)) {
        (void)why;
    }
}

int gb_mouse_enabled(void)
{
    return s_on;
}

int gb_mouse_pointer_shown(void)
{
    return s_on && !s_pointer_hidden && !s_in_scene;
}

int gb_mouse_window_msg(HWND h, UINT m, WPARAM w, LPARAM l)
{
    float gx = 0, gy = 0;
    int in;
    (void)h;
    if (!s_on)
        return 0;
    if (m == WM_MOUSELEAVE) {
        EnterCriticalSection(&s_lock);
        s_mouse.has_point = 0;
        LeaveCriticalSection(&s_lock);
        return 1;
    }
    if (m == WM_MOUSEWHEEL) {
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        EnterCriticalSection(&s_lock);
        s_mouse.wheel += notches;
        LeaveCriticalSection(&s_lock);
        return 1;
    }
    {
        int cx = GET_X_LPARAM(l), cy = GET_Y_LPARAM(l);
        in = gb_d3d11_menu_point(cx, cy, &gx, &gy);
        EnterCriticalSection(&s_lock);
        s_mouse.has_point = in;
        s_mouse.gx = gx;
        s_mouse.gy = gy;
        if (m == WM_MOUSEMOVE) {
            /* Windows sends a move when the window under a still pointer
             * changes: only a real move of more than a few pixels counts. */
            if (abs(cx - s_last_cx) + abs(cy - s_last_cy) > 3) {
                s_mouse.moved_at = now_ms();
                pointer_show("the mouse moved");
            }
            s_last_cx = cx;
            s_last_cy = cy;
        } else if (m == WM_LBUTTONDOWN) {
            s_mouse.clicks_l++;
            s_mouse.click_in = in;
            s_mouse.click_gx = gx;
            s_mouse.click_gy = gy;
            pointer_show("a click");
        } else if (m == WM_RBUTTONDOWN) {
            s_mouse.clicks_r++;
            pointer_show("a click");
        }
        LeaveCriticalSection(&s_lock);
    }
    return 1;
}

/* Every 50 ms on the window's thread: keep the pointer's look in step with
 * the game (a match starts, a pad was pressed). */
void gb_mouse_timer(HWND h)
{
    static int shown = -1;
    POINT p;
    RECT r;
    int now;
    if (!s_on)
        return;
    now = gb_mouse_pointer_shown();
    if (now == shown)
        return;
    shown = now;
    if (GetCursorPos(&p) && ScreenToClient(h, &p) && GetClientRect(h, &r) && PtInRect(&r, p)
        && WindowFromPoint(p) == h)
        SetCursor(now ? LoadCursor(NULL, IDC_ARROW) : NULL);
}

static void pointer_hide(const char *why)
{
    if (s_on && !InterlockedExchange(&s_pointer_hidden, 1)) {
        (void)why;
    }
}

void gb_mouse_key_pressed(void)
{
    pointer_hide("a key");
}

void gb_mouse_other_input(void)
{
    pointer_hide("a controller");
}

/* ── Presses, on the game's side ───────────────────────────────────── */

#define BTN_UP    0x0001                 /* XBOX_GAMEPAD_DPAD_UP, xinput_xbox.h */
#define BTN_DOWN  0x0002
#define BTN_A     0x1000                 /* not a real bit: the A analog button */
#define BTN_B     0x2000                 /* the B analog button */
#define HOLD_MS   50.0
#define GAP_MS    50.0
#define STAR_WAIT_MS 500.0               /* a move the star has not followed by then: drop the rest */
#define STEADY_MS 400.0                  /* a screen must be what it is this long to take a click */
#define AFTER_AB_MS 600.0                /* and this long after the mouse's own A or B */

typedef enum { IDLE, HOLDING, GAP, WAIT_STAR } Phase;

static struct {
    Phase phase;
    int button;                          /* held or last sent */
    double since;
    int target_row;                      /* the row a move sequence goes to, or -1 */
    int then_a;                          /* press A once there */
    int screen;                          /* the screen the sequence was made for */
    int from_row;                        /* the star's row when the move was sent */
    double screen_since;                 /* the current recognition started */
    int screen_last;                     /* what the screen was at the last poll */
    double hover_served;                 /* the movement hover last acted on */
    double ab_at;                        /* the mouse's last A or B */
} P = { IDLE, 0, 0, -1, 0, -1, -1, 0, -2, 0, -1e9 };

static void start_press(int button, double t)
{
    if (button == BTN_A || button == BTN_B)
        P.ab_at = t;
    P.button = button;
    P.phase = HOLDING;
    P.since = t;
}

static void drop_sequence(const char *why)
{
    (void)why;
    P.target_row = -1;
    P.then_a = 0;
}

/* The row of known screen S under a point, or -1 (outside every item). */
static int row_at(const Screen *S, float gx, float gy)
{
    int r;
    if (gx < (float)(S->x - STAR_LEFT) || gx >= (float)(S->x + LABEL_W))
        return -1;
    for (r = 0; r < S->nrows; r++)
        if (gy >= (float)(S->y[r] - 4) && gy < (float)(S->y[r] - 4 + S->row_h))
            return r;
    return -1;
}

/* Called from xbox_InputGetState for pad 1 on every poll. Adds the button
 * the mouse holds now; returns 1 when the mouse is in use (so pad 1 stays
 * connected with no controller and the keyboard off). */
int gb_mouse_pad(uint16_t *buttons, uint8_t *analog_a, uint8_t *analog_b)
{
    Seen seen;
    Mouse mo;
    double t;
    if (!s_on)
        return 0;
    t = now_ms();
    EnterCriticalSection(&s_lock);
    seen = s_seen;
    mo = s_mouse;
    s_mouse.clicks_l = s_mouse.clicks_r = 0;
    s_mouse.wheel = 0;
    LeaveCriticalSection(&s_lock);
    InterlockedExchange(&s_in_scene, seen.scene3d);

    /* How long the current screen has been what it is (a known screen, an
     * unknown one, a 3D scene). A click before it is steady is dropped. */
    {
        int id = seen.scene3d ? -3 : seen.screen;
        if (id != P.screen_last) {
            P.screen_last = id;
            P.screen_since = t;
        }
    }

    switch (P.phase) {
    case HOLDING:
        if (t - P.since >= HOLD_MS) {
            P.phase = GAP;
            P.since = t;
        }
        break;
    case GAP:
        if (t - P.since >= GAP_MS) {
            if ((P.button == BTN_UP || P.button == BTN_DOWN) && P.target_row >= 0) {
                P.phase = WAIT_STAR;
                P.since = t;
            } else {
                P.phase = IDLE;
            }
        }
        break;
    case WAIT_STAR:
        if (seen.screen != P.screen || seen.scene3d) {
            drop_sequence("the screen changed");
            P.phase = IDLE;
        } else if (seen.star_row >= 0 && seen.star_row != P.from_row && seen.at > P.since) {
            P.phase = IDLE;              /* the move landed: next step below */
        } else if (t - P.since > STAR_WAIT_MS) {
            drop_sequence("the highlight did not follow");
            P.phase = IDLE;
        }
        break;
    case IDLE:
        break;
    }

    {
        int steady = t - P.screen_since >= STEADY_MS && t - P.ab_at >= AFTER_AB_MS;
        int known = seen.screen >= 0 && !seen.scene3d && seen.star_row >= 0;
        const Screen *S = known ? &s_screens[seen.screen] : NULL;
        int busy = P.phase != IDLE;
        int r;

        /* A left click on a known screen while a move is under way for the
         * same screen: the move goes on to the clicked row instead. */
        if (S && mo.clicks_l > 0 && P.target_row >= 0 && P.screen == seen.screen && steady
            && mo.click_in && (r = row_at(S, mo.click_gx, mo.click_gy)) >= 0) {
            P.target_row = r;
            P.then_a = 1;
            mo.clicks_l = 0;
        }

        /* the next step of a move */
        if (!busy && P.target_row >= 0) {
            if (!S || seen.screen != P.screen) {
                drop_sequence("the screen changed");
            } else if (seen.star_row == P.target_row) {
                int a = P.then_a;
                P.target_row = -1;
                P.then_a = 0;
                if (a)
                    start_press(BTN_A, t);
            } else {
                P.from_row = seen.star_row;
                start_press(seen.star_row < P.target_row ? BTN_DOWN : BTN_UP, t);
            }
            busy = P.phase != IDLE;
        }

        /* new mouse input: taken only when nothing is under way and the
         * screen is steady; otherwise dropped (never kept for later) */
        if (busy || P.target_row >= 0 || seen.scene3d || !seen.seq) {
        } else if (!steady) {
        } else if (mo.clicks_r > 0) {
            start_press(BTN_B, t);
        } else if (S && mo.clicks_l > 0) {
            r = mo.click_in ? row_at(S, mo.click_gx, mo.click_gy) : -1;
            if (r >= 0 && seen.star_row == r) {
                start_press(BTN_A, t);
            } else if (r >= 0) {
                P.screen = seen.screen;
                P.target_row = r;
                P.then_a = 1;
                P.from_row = seen.star_row;
                start_press(seen.star_row < r ? BTN_DOWN : BTN_UP, t);
            }
            /* outside every item: nothing */
        } else if (S && mo.wheel != 0) {
            r = seen.star_row - mo.wheel;   /* wheel up (positive) moves up */
            if (r < 0) r = 0;
            if (r >= S->nrows) r = S->nrows - 1;
            if (r != seen.star_row) {
                P.screen = seen.screen;
                P.target_row = r;
                P.then_a = 0;
                P.from_row = seen.star_row;
                start_press(seen.star_row < r ? BTN_DOWN : BTN_UP, t);
            }
        } else if (S && mo.has_point && !s_pointer_hidden && mo.moved_at > P.screen_since
                   && mo.moved_at > P.hover_served && t - mo.moved_at >= 60.0) {
            /* hover: the pointer has moved on this screen and come to rest
             * over an item: the highlight goes there. Once per movement, so
             * a controller or key press afterwards is never undone. */
            P.hover_served = mo.moved_at;
            r = row_at(S, mo.gx, mo.gy);
            if (r >= 0 && r != seen.star_row) {
                P.screen = seen.screen;
                P.target_row = r;
                P.then_a = 0;
                P.from_row = seen.star_row;
                start_press(seen.star_row < r ? BTN_DOWN : BTN_UP, t);
            }
        } else if (!S && seen.screen < 0) {
            /* a screen the port does not know: the coarse mouse */
            if (mo.clicks_l > 0 && mo.click_in)
                start_press(BTN_A, t);
            else if (mo.wheel > 0)
                start_press(BTN_UP, t);
            else if (mo.wheel < 0)
                start_press(BTN_DOWN, t);
        }
    }

    if (P.phase == HOLDING) {
        if (P.button == BTN_A)
            *analog_a = 255;
        else if (P.button == BTN_B)
            *analog_b = 255;
        else
            *buttons |= (uint16_t)P.button;
        return 1;
    }
    return 1;
}

/* ── Snapshots of the game's memory (diagnostic builds) ──────────────────────── */


/* ── Start ─────────────────────────────────────────────────────────── */

void gb_mouse_init(void)
{
    const char *e = getenv("GB_MOUSE_MENUS");
    int s;
    QueryPerformanceFrequency(&s_qpf);
    InitializeCriticalSection(&s_lock);
    if (!(e && *e && *e != '0'))
        return;
    s_nlayouts = pack_parse("game_files\\fe\\xlay_fe.bin");
    if (s_nlayouts <= 0) {
        gb_log("[MOUSE] mouse in menus: the menu layouts (game_files\\fe\\xlay_fe.bin) could not be read; off\n");
        return;
    }
    screens_build();
    for (s = 0; s < NSCREENS; s++)
        if (s_screens[s].nrows > 0) {
            if (s_col_x < 0)
                s_col_x = s_screens[s].x;
            else if (s_col_x != s_screens[s].x)
                s_screens[s].nrows = 0;      /* one column x is watched */
        }
    s_on = 1;
    gb_log("[MOUSE] mouse in menus: on (GB_MOUSE_MENUS); %d layouts read, known screens:%s%s\n",
           s_nlayouts, s_screens[0].nrows ? " Main Menu" : "", s_screens[1].nrows ? " Play It" : "");
}
