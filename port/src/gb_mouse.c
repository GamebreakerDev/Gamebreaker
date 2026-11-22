/*
 * The mouse in menus: GB_MOUSE_MENUS=1 (off by default).
 *
 * The game's menus were made for a controller. With this switch the mouse
 * works in them the way a controller does, by pressing the same buttons a
 * player would; the port never writes the game's own state, so a click can
 * only lead somewhere a controller can reach.
 *
 *   Lists           any menu drawn as one column of items, one of them
 *                   highlighted (the Main Menu, Play It, My Team, Inside
 *                   Vol. 2 and its sub-menus, Game Data, the Yes/No boxes):
 *                   pointing at an item moves the highlight to it (hover),
 *                   a left click moves the highlight there and presses A,
 *                   the wheel moves it up and down. A click outside every
 *                   item does nothing.
 *   Other screens   with no 3D scene (the title, grids, option rows, card
 *                   lists): the wheel is the D-pad's up and down, a left
 *                   click is A.
 *   Everywhere      a right click is B (back), as Esc and Backspace are.
 *   In a match      (any frame with a 3D scene: the court, the pause menu
 *                   over it) the mouse does nothing and the pointer is
 *                   hidden.
 *   The pointer     shown over the game's picture while the mouse is in
 *                   use on a menu; hidden after a controller or key press
 *                   until the mouse moves again, in a window and in
 *                   fullscreen alike.
 *
 * How the port knows where things are. The game keeps no plain
 * "highlighted item" value in its memory that the port can read (memory
 * snapshots taken with the highlight on different items show none, and the
 * layouts are freed once a screen is built), so the port reads what is
 * drawn: each frame the Direct3D 11 back end hands over the small flat quads
 * of the menu (one per letter). Letters on one line, close together, make a
 * label. A list is a column of labels of one size, aligned on their left
 * edge or their middle and evenly spaced, of which exactly one is drawn in
 * the game's highlight gold and the rest in one other colour; a row with a
 * value beside it (an option row) makes the screen "not a list". The white
 * stars drawn beside the highlighted label are left out.
 *
 * A click is then a closed loop: one move, wait until the gold has moved to
 * the next row, the next move, and A only once the gold sits on the clicked
 * row. If the list stops being recognised, or the gold does not move in
 * time, the rest is dropped (never queued): a click during a transition does
 * nothing.
 *
 * The disc's layout pack (game_files\fe\xlay_fe.bin, the player's own file:
 * nothing is shipped) only gives the lists their names in the log.
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

/* ── The layouts, read from the player's own disc (names only) ────────── */

#define MAX_LAYOUTS 128
#define MAX_ITEMS   48

typedef struct {
    int n;                  /* element number (id >> 12) */
    int index;              /* tag 0x22: the item's number in its menu */
    int x, y;               /* tag 0x03: the item's point */
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
 * u16), 0x22 the item's index. Items are kept sorted top to bottom. */
static void layout_parse(const uint8_t *e, uint32_t size, Layout *L)
{
    uint32_t cnt, k, j;
    memset(L, 0, sizeof *L);
    if (size < 6)
        return;
    cnt = rd16(e + 4);
    for (k = 0; k < cnt; k++) {
        uint32_t d = 6 + 6 * (k + 1), np, off, id = 0;
        Item it;
        int s;
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
            if (t == 0x00) id = v;
            else if (t == 0x03) { it.x = (int)(v & 0xFFFF); it.y = (int)(v >> 16); }
            else if (t == 0x22) it.index = (int)v;
        }
        if ((id & 0xFF0) != 0x0A0 || it.index < 0 || L->nitems >= MAX_ITEMS)
            continue;
        it.n = (int)(id >> 12);
        for (s = L->nitems; s > 0 && L->it[s - 1].y > it.y; s--)
            L->it[s] = L->it[s - 1];
        L->it[s] = it;
        L->nitems++;
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

/* Names for the log, by the layout whose items sit where the list's rows
 * are drawn (each list draws its labels a fixed distance from its items'
 * points). */
static const struct { int layout; const char *name; int hides; } s_names[] = {
    { 0, "Main Menu", 0 }, { 2, "Play It", 0 }, { 1, "My Team", 0 },
    { 57, "Inside Vol. 2", 0 }, { 55, "Game Data", 1 },   /* hides: shows only the top of its items */
};
#define NNAMES ((int)(sizeof s_names / sizeof s_names[0]))

/* ── What the back end saw drawn ───────────────────────────────────── */

#define MAX_GLYPHS 1536
#define MAX_LABELS 160
#define MAX_ROWS   12
#define HELP_Y     405       /* the help line along the bottom of every menu */
#define TITLE_Y    90        /* the screen's title above the panel */

typedef struct { short x0, y0, x1, y1; uint32_t rgb; } Glyph;

/* Built on the render thread during a frame. */
static Glyph s_g[MAX_GLYPHS];
static int s_ng;

typedef struct {
    uint32_t seq;
    int scene3d;                         /* the frame drew 3D */
    int screen;                          /* the list's signature, or -1: no list */
    int hl;                              /* the highlighted row, or -1 */
    int nrows;
    int row_top[MAX_ROWS], row_bot[MAX_ROWS];   /* each row's hit band, 640x480 menu screen */
    int left, right;                     /* the rows' hit band across */
    int title;                           /* the title screen (Press START) */
    char name[48];
    double at;                           /* when it was published */
} Seen;

static CRITICAL_SECTION s_lock;
static Seen s_seen;                      /* the latest frame's, under s_lock */

typedef struct { float x, y, z, rhw; uint32_t diffuse; } VertexHead;   /* the start of Nv2aVertex */


void gb_mouse_note_batch(const void *vertices, uint32_t count, uint32_t stride,
                         uint32_t surf_w, uint32_t surf_h, uint32_t aa_x, uint32_t aa_y)
{
    uint32_t i, k;
    float sx, sy;
    if (!s_on || !count || !surf_w || !surf_h || !aa_x || !aa_y)
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
        uint32_t rgb = ((const VertexHead *)((const uint8_t *)vertices + (size_t)i * stride))->diffuse & 0xFFFFFFu;
        Glyph *g;
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
        if (w < 2.0f || h < 8.0f || w > 40.0f || h > 40.0f || y0 < (float)TITLE_Y || y1 > (float)HELP_Y
            || x0 < 0.0f || x1 > 640.0f)
            continue;                    /* not a letter of a list */
        if (rgb == 0xFFFFFFu || rgb == 0)
            continue;                    /* the white stars beside the highlight; drop shadows */
        if (s_ng > 0) {                  /* the quad's second triangle */
            g = &s_g[s_ng - 1];
            if (g->x0 == (short)x0 && g->y0 == (short)y0 && g->x1 == (short)x1 && g->y1 == (short)y1)
                continue;
        }
        if (s_ng >= MAX_GLYPHS)
            continue;
        g = &s_g[s_ng++];
        g->x0 = (short)x0;
        g->y0 = (short)y0;
        g->x1 = (short)x1;
        g->y1 = (short)y1;
        g->rgb = rgb;
    }
}

/* The game's highlight gold (FB AC 00; its alpha pulses). */
static int is_gold(uint32_t c)
{
    int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
    return r >= 0xE0 && g >= 0x90 && g <= 0xC8 && b <= 0x40;
}

static int same_rgb(uint32_t a, uint32_t b)
{
    int s;
    for (s = 0; s < 24; s += 8) {
        int d = (int)(a >> s & 255) - (int)(b >> s & 255);
        if (d < -16 || d > 16)
            return 0;
    }
    return 1;
}

typedef struct { int x0, y0, x1, y1, h, n; uint32_t rgb; } Label;

static int cmp_glyph_y(const void *a, const void *b)
{
    const Glyph *p = (const Glyph *)a, *q = (const Glyph *)b;
    return p->y0 != q->y0 ? p->y0 - q->y0 : p->x0 - q->x0;
}

/* Letters into labels: a line is letters whose tops are within 10 pixels of
 * the line's first (capitals and small letters of one label); along a line
 * a gap of more than 40 pixels starts another label (word gaps are under
 * 15). A label's colour is its letters' most common one. */
static int make_labels(Glyph *g, int ng, Label *lab)
{
    int nl = 0, a = 0;
    qsort(g, (size_t)ng, sizeof *g, cmp_glyph_y);
    while (a < ng) {
        int b = a + 1, i, j;
        while (b < ng && g[b].y0 <= g[a].y0 + 10)
            b++;
        for (i = a + 1; i < b; i++) {        /* the line, by x */
            Glyph t = g[i];
            for (j = i; j > a && g[j - 1].x0 > t.x0; j--)
                g[j] = g[j - 1];
            g[j] = t;
        }
        for (i = a; i < b && nl < MAX_LABELS; ) {
            Label *L = &lab[nl++];
            int k, best = 0;
            L->x0 = g[i].x0; L->y0 = g[i].y0; L->x1 = g[i].x1; L->y1 = g[i].y1;
            L->h = g[i].y1 - g[i].y0;
            L->rgb = g[i].rgb;
            for (j = i + 1; j < b && g[j].x0 <= L->x1 + 40; j++) {
                if (g[j].x1 > L->x1) L->x1 = g[j].x1;
                if (g[j].y0 < L->y0) L->y0 = g[j].y0;
                if (g[j].y1 > L->y1) L->y1 = g[j].y1;
                if (g[j].y1 - g[j].y0 > L->h) L->h = g[j].y1 - g[j].y0;
            }
            L->n = j - i;
            for (k = i; k < j; k++) {
                int c = 0, m;
                for (m = i; m < j; m++)
                    c += g[m].rgb == g[k].rgb;
                if (c > best) { best = c; L->rgb = g[k].rgb; }
            }
            i = j;
        }
        a = b;
    }
    return nl;
}

/* Finds a list among the labels; fills out's rows, returns 1 if found. */
static int find_list(const Label *lab, int nl, Seen *out)
{
    int hi = -1, i, j, mode, nm = 0, h, gap = 0, top, bot, r, mx0, mx1;
    int mem[MAX_LABELS], run[MAX_ROWS], nrun;
    uint32_t normal = 0;
    int have_normal = 0;
    for (i = 0; i < nl; i++)
        if (lab[i].n >= 2 && is_gold(lab[i].rgb)) {
            if (hi >= 0)
                return 0;                /* two gold labels: not a list (an option row and its value) */
            hi = i;
        }
    if (hi < 0)
        return 0;
    /* aligned with the gold one on the left edge, else in the middle */
    for (mode = 0; mode < 2 && nm == 0; mode++)
        for (i = 0; i < nl; i++) {
            const Label *L = &lab[i];
            int d = mode == 0 ? L->x0 - lab[hi].x0 : (L->x0 + L->x1) - (lab[hi].x0 + lab[hi].x1);
            if (i == hi || L->n < 2 || is_gold(L->rgb) || L->h - lab[hi].h > 5 || lab[hi].h - L->h > 5)
                continue;
            if (d >= -(mode == 0 ? 4 : 8) && d <= (mode == 0 ? 4 : 8))
                mem[nm++] = i;
        }
    mode--;
    if (nm == 0)
        return 0;
    mem[nm++] = hi;
    for (i = 1; i < nm; i++) {           /* by height on the screen */
        int t = mem[i];
        for (j = i; j > 0 && lab[mem[j - 1]].y0 > lab[t].y0; j--)
            mem[j] = mem[j - 1];
        mem[j] = t;
    }
    for (h = 0; mem[h] != hi; h++)
        ;
    /* the evenly spaced run of one colour around the gold label */
    top = bot = h;
    for (j = h + 1; j < nm; j++) {
        int g = lab[mem[j]].y0 - lab[mem[j - 1]].y0;
        if (g < 18 || g > 64 || (gap && (g - gap > 6 || gap - g > 6)))
            break;
        if (have_normal && !same_rgb(lab[mem[j]].rgb, normal))
            break;
        if (!gap) gap = g;
        if (!have_normal) { normal = lab[mem[j]].rgb; have_normal = 1; }
        bot = j;
    }
    for (j = h - 1; j >= 0; j--) {
        int g = lab[mem[j + 1]].y0 - lab[mem[j]].y0;
        if (g < 18 || g > 64 || (gap && (g - gap > 6 || gap - g > 6)))
            break;
        if (have_normal && !same_rgb(lab[mem[j]].rgb, normal))
            break;
        if (!gap) gap = g;
        if (!have_normal) { normal = lab[mem[j]].rgb; have_normal = 1; }
        top = j;
    }
    nrun = bot - top + 1;
    if (nrun < 2 || nrun > MAX_ROWS)
        return 0;
    for (r = 0; r < nrun; r++)
        run[r] = mem[top + r];
    /* a row with something else on its line, to its right, is an option row
     * (its value): not a plain list; a row of more than 30 letters is a
     * sentence (the title screen's copyright line under Press START), not
     * an item */
    for (r = 0; r < nrun; r++) {
        const Label *R = &lab[run[r]];
        if (R->n > 30) {
            /* gold "Press START" over the copyright line: the title screen */
            out->title = run[r] != hi;
            return 0;
        }
        for (i = 0; i < nl; i++)
            if (lab[i].x0 > R->x1 + 30 && lab[i].y0 - R->y0 <= 10 && R->y0 - lab[i].y0 <= 10)
                return 0;
    }
    mx0 = 640; mx1 = 0;
    for (r = 0; r < nrun; r++) {
        const Label *R = &lab[run[r]];
        int c = (R->y0 + R->y1) / 2;
        out->row_top[r] = c - gap / 2;
        out->row_bot[r] = c - gap / 2 + gap;
        if (R->x0 < mx0) mx0 = R->x0;
        if (R->x1 > mx1) mx1 = R->x1;
        if (run[r] == hi)
            out->hl = r;
    }
    out->nrows = nrun;
    out->left = mx0 - 30;                /* the stars sit up to 24 pixels out */
    out->right = mx1 + 30;
    /* the same list whatever is highlighted: rows, first row, alignment */
    out->screen = ((nrun * 631 + lab[run[0]].y0) * 1031
                   + (mode == 0 ? mx0 : (lab[run[0]].x0 + lab[run[0]].x1) / 2)) * 2 + mode;
    if (out->screen < 0)
        out->screen = -out->screen;
    /* its name, from the layout whose items sit at the rows */
    snprintf(out->name, sizeof out->name, mode == 0 ? "a list of %d rows" : "a box of %d rows", nrun);
    /* all of a layout's items first, then the top of one (screens that hide
     * their last items) */
    for (i = 0; mode == 0 && i < 2 * NNAMES; i++) {
        int pass = i / NNAMES, k = i % NNAMES;
        const Layout *L = &s_layouts[s_names[k].layout];
        int off, ok;
        if (pass == 0 ? L->nitems != nrun : L->nitems <= nrun || !s_names[k].hides)
            continue;
        if (mx0 - L->it[0].x > 6 || L->it[0].x - mx0 > 6)
            continue;
        off = lab[run[0]].y0 - L->it[0].y;
        ok = off >= -24 && off <= 16;
        for (r = 1; ok && r < nrun; r++) {
            int d = lab[run[r]].y0 - L->it[r].y - off;
            ok = d >= -3 && d <= 3;
        }
        if (ok) {
            snprintf(out->name, sizeof out->name, "%s", s_names[k].name);
            break;
        }
    }
    return 1;
}

void gb_mouse_frame_end(int scene3d)
{
    static Label lab[MAX_LABELS];
    Seen now;
    int nl;
    if (!s_on)
        return;
    memset(&now, 0, sizeof now);
    now.scene3d = scene3d;
    now.screen = -1;
    now.hl = -1;
    if (!scene3d) {
        nl = make_labels(s_g, s_ng, lab);
        if (!find_list(lab, nl, &now)) {
            now.screen = -1;
            now.hl = -1;
            now.nrows = 0;
        }
    }
    now.at = now_ms();
    s_ng = 0;
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
#define BTN_START 0x0010                 /* XBOX_GAMEPAD_START */
#define HOLD_MS   50.0
#define GAP_MS    50.0
#define HL_WAIT_MS 500.0                 /* a move the highlight has not followed by then: drop the rest */
#define STEADY_MS 400.0                  /* a screen must be what it is this long to take a click */
#define AFTER_AB_MS 600.0                /* and this long after the mouse's own A or B */

typedef enum { IDLE, HOLDING, GAP, WAIT_HL } Phase;

static struct {
    Phase phase;
    int button;                          /* held or last sent */
    double since;
    int target_row;                      /* the row a move sequence goes to, or -1 */
    int then_a;                          /* press A once there */
    int screen;                          /* the list the sequence was made for */
    int from_row;                        /* the highlight's row when the move was sent */
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

/* The row of the list under a point, or -1 (outside every item). */
static int row_at(const Seen *S, float gx, float gy)
{
    int r;
    if (gx < (float)S->left || gx >= (float)S->right)
        return -1;
    for (r = 0; r < S->nrows; r++)
        if (gy >= (float)S->row_top[r] && gy < (float)S->row_bot[r])
            return r;
    return -1;
}

static void start_move(const Seen *S, int r, int then_a, double t)
{
    P.screen = S->screen;
    P.target_row = r;
    P.then_a = then_a;
    P.from_row = S->hl;
    start_press(S->hl < r ? BTN_DOWN : BTN_UP, t);
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

    /* How long the current screen has been what it is (a list, a screen
     * with none, a 3D scene). A click before it is steady is dropped. */
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
                P.phase = WAIT_HL;
                P.since = t;
            } else {
                P.phase = IDLE;
            }
        }
        break;
    case WAIT_HL:
        if (seen.screen != P.screen || seen.scene3d) {
            drop_sequence("the screen changed");
            P.phase = IDLE;
        } else if (seen.hl >= 0 && seen.hl != P.from_row && seen.at > P.since) {
            P.phase = IDLE;              /* the move landed: next step below */
        } else if (t - P.since > HL_WAIT_MS) {
            drop_sequence("the highlight did not follow");
            P.phase = IDLE;
        }
        break;
    case IDLE:
        break;
    }

    {
        int steady = t - P.screen_since >= STEADY_MS && t - P.ab_at >= AFTER_AB_MS;
        const Seen *S = seen.screen >= 0 && !seen.scene3d && seen.hl >= 0 ? &seen : NULL;
        int busy = P.phase != IDLE;
        int r;

        /* A left click on the list while a move is under way on it: the
         * move goes on to the clicked row instead. */
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
            } else if (seen.hl == P.target_row) {
                int a = P.then_a;
                P.target_row = -1;
                P.then_a = 0;
                if (a)
                    start_press(BTN_A, t);
            } else {
                P.from_row = seen.hl;
                start_press(seen.hl < P.target_row ? BTN_DOWN : BTN_UP, t);
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
            if (r >= 0 && seen.hl == r)
                start_press(BTN_A, t);
            else if (r >= 0)
                start_move(S, r, 1, t);
            /* outside every item: nothing */
        } else if (S && mo.wheel != 0) {
            r = seen.hl - mo.wheel;      /* wheel up (positive) moves up */
            if (r < 0) r = 0;
            if (r >= S->nrows) r = S->nrows - 1;
            if (r != seen.hl)
                start_move(S, r, 0, t);
        } else if (S && mo.has_point && !s_pointer_hidden && mo.moved_at > P.screen_since
                   && mo.moved_at > P.hover_served && t - mo.moved_at >= 60.0) {
            /* hover: the pointer has moved on this list and come to rest
             * over an item: the highlight goes there. Once per movement, so
             * a controller or key press afterwards is never undone. */
            P.hover_served = mo.moved_at;
            r = row_at(S, mo.gx, mo.gy);
            if (r >= 0 && r != seen.hl)
                start_move(S, r, 0, t);
        } else if (!S && seen.screen < 0) {
            /* a screen with no list: the coarse mouse */
            if (mo.clicks_l > 0 && mo.click_in)
                start_press(seen.title ? BTN_START : BTN_A, t);   /* A does not leave the title */
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
    }
    return 1;
}

/* ── Snapshots of the game's memory (diagnostic builds) ──────────────────────── */



/* ── Start ─────────────────────────────────────────────────────────── */

void gb_mouse_init(void)
{
    const char *e = getenv("GB_MOUSE_MENUS");
    QueryPerformanceFrequency(&s_qpf);
    InitializeCriticalSection(&s_lock);
    if (!(e && *e && *e != '0'))
        return;
    s_nlayouts = pack_parse("game_files\\fe\\xlay_fe.bin");
    s_on = 1;
    gb_log("[MOUSE] mouse in menus: on (GB_MOUSE_MENUS); %d menu layouts read for the lists' names\n",
           s_nlayouts);
}
