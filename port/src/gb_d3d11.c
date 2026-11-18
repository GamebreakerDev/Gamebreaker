/*
 * Direct3D 11 render back end for the toolkit's pushbuffer executor.
 *
 * The game links the XDK's Direct3D 8 statically, so what it produces is a
 * stream of NV2A commands. The toolkit's executor (xboxrecomp, nv2a_pb_exec.c)
 * decodes that stream. By default it also draws it, on the CPU, which is what
 * limits a match to under 10 frames a second; the game's own code needs
 * about 1 ms a frame. This file registers a back end through the toolkit's
 * nv2a_backend.h, so the executor hands over clears, triangles and flips and
 * the host GPU draws them.
 *
 * What arrives: triangle lists already transformed to surface pixels and
 * clipped at the near plane, with 1/w, one colour and one texture coordinate
 * per vertex; the texture of stage 0; blend, alpha-test, depth and stencil
 * state in the NV2A's own values. With the toolkit patch in
 * port/patches/ (Nv2aBatch.ext) also: all four texture stages, the register
 * combiners, and each vertex's stage coordinates, specular colour and fog.
 * The pixel shader below evaluates the combiners as the title programmed
 * them, so two-texture materials come out as on the console.
 *
 * Everything here runs on the executor's thread except the window, which has
 * its own thread so that it stays responsive while the game loads.
 *
 * Switches (environment), set by the launcher:
 *
 * Window and display (see "window" and "presenting" below):
 *   GB_D3D11_FULLSCREEN=1  start in borderless fullscreen. Alt+Enter or F11
 *                          switches between window and fullscreen at any time
 *   GB_D3D11_MONITOR=<n>   start on monitor n (1, 2, ... in Windows' enumeration
 *                          order, not the numbers Windows Settings shows), or
 *     =<name>              on the display of that name, as Windows Settings
 *                          shows it (the monitor's own name, not case
 *                          sensitive). Default the primary one
 *   GB_D3D11_RES=<s>       internal resolution: every render target, depth
 *                          buffer and viewport s times the game's size (1, 2,
 *                          3, 4, or a fraction such as 1.5); "window" follows
 *                          the window, so the picture is drawn at the size it
 *                          is shown. Default 1, the console's resolution
 *   GB_D3D11_VSYNC=<n>     0: present at once, the frame limit alone sets the
 *                          pace (default). n: wait for n display refreshes
 *                          a frame. auto: n = refresh rate / 60 when the rate
 *                          is a whole multiple of 60, else 0. Game speed comes
 *                          from the frame limit in every case
 *
 * Picture quality, off by default (see "texture quality" and "multisampling"
 * below):
 *   GB_D3D11_ANISO=<n>     0 or unset: every texture sampled at its top level
 *                          only, as before. 1: the mip levels the game supplies,
 *                          with the filter the game asks for (trilinear on
 *                          nearly everything): distant textures stop shimmering.
 *                          2, 4, 8, 16: the same plus anisotropic filtering of
 *                          that degree, so surfaces seen at a low angle (the
 *                          court) stay sharp. 3D only: menus, HUD, text and
 *                          movies are sampled exactly as before
 *   GB_D3D11_MSAA=<n>      2, 4 or 8: n samples a pixel on every render target
 *                          (on top of GB_D3D11_RES), stepped down to what the
 *                          GPU supports. 0 or unset: off
 *   GB_D3D11_ADAPTER=<a>   which graphics chip: unset or "default" Windows'
 *                          choice (on a laptop with two chips the program's
 *                          exported hints make that the dedicated one, see
 *                          "graphics chip"), "high" the one DXGI ranks for high
 *                          performance, or a number (0 is the first adapter).
 *                          The log's "[D3D11] adapter:" line names the one used
 *
 */
/*
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "nv2a_backend.h"   /* toolkit, src/kernel */
#include "gb_d3d11.h"
#include "gb_save.h"        /* the save-write guard */
#include "gb_log.h"         /* the log for bug reports */
#include "gb_overlay.h"     /* start screen, corner mark, frame counter, screenshots */

int gb_game_hz(void);        /* recomp_manual.c: the game's step rate, GB_GAME_HZ (60 unless set) */

/* Toolkit functions used here (declared in its src/input/keyboard.h,
 * src/platform/recomp_exit_hook.h and the kernel library). */
void xbox_InputKeySet(int vk, int down);
void xbox_InputKeysClear(void);
int  xbox_InputWin32SideVk(int vk, uint32_t lparam);
void recomp_exit_hook_run(const char *why);
ptrdiff_t xbox_GetMemoryOffset(void);
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);

/* gb_pad.c: the controller layer (GB_PADS) is told about the window, device
 * arrivals and the exit, and hands back a notice for the title bar. */
void gb_pad_window(HWND hwnd);
void gb_bgsound_window(HWND hwnd);
void gb_bgsound_shutdown(void);
void gb_pad_devices_changed(void);
void gb_pad_shutdown(void);
unsigned gb_pad_notice(char *buf, int size);
#define GB_PAD_TIMER 0x6B50
#define GB_WINDOW_TITLE "Gamebreaker (Direct3D 11)"

#define GB_LOGICAL_W 640
#define GB_LOGICAL_H 480
#define GB_MAX_TARGETS 16
#define GB_MAX_DEPTHS 8
#define GB_MAX_TEXTURES 2048
#define GB_TEX_BUCKETS 4096
#define GB_VB_BYTES (32u * 1024u * 1024u)
#define GB_MAX_STATES 64
#define GB_MAX_RES 8.0                  /* 1280 x 8 = 10240, inside Direct3D 11's 16384 */

/* 104 bytes: clip-space position, diffuse, specular, fog factor, and the
 * texture coordinates of the four stages (s, t, r, q). */
typedef struct {
    float x, y, z, w;
    uint32_t color;
    float spec[4];
    float fog;
    float tex[4][4];
} GbVertex;

/* w, h: the size the game gave the surface, used for every calculation in the
 * game's pixel units and to match surfaces. rw, rh: the size of the Direct3D
 * texture behind it, w and h times the internal resolution (GB_D3D11_RES). */
typedef struct {
    uint32_t va, w, h, rw, rh;
    ID3D11Texture2D *tex;               /* single-sample: what is sampled, shown and dumped */
    ID3D11RenderTargetView *rtv;        /* on ms when multisampling, else on tex */
    ID3D11ShaderResourceView *srv;      /* on tex */
    ID3D11Texture2D *ms;                /* GB_D3D11_MSAA: the multisampled target, else NULL */
    int dirty;                          /* ms drawn into since it was last resolved into tex */
    uint32_t last_frame;
} GbTarget;

typedef struct {
    uint32_t va, w, h;                  /* real (scaled) size */
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint32_t last_frame;
} GbDepth;

typedef struct {
    uint32_t offset, w, h, pitch, color, palette;
    uint32_t levels;                            /* mip levels uploaded (1 unless GB_D3D11_ANISO) */
    uint32_t want;                              /* levels offered by tex_levels (the lookup key) */
    uint64_t hash;
    uint64_t hash0;                             /* level 0 only, no palette: names the picture whatever its colours (anchored_2d) */
    uint32_t checked_frame, used_frame;
    int next;                                   /* bucket chain, -1 ends */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GbTexture;

typedef struct { uint32_t key[6]; ID3D11BlendState *obj; } GbBlend;
typedef struct { uint32_t key[10]; ID3D11DepthStencilState *obj; } GbDepthState;
typedef struct { uint32_t key[5]; ID3D11SamplerState *obj; } GbSampler;

static struct {
    int tried, ok;
    HWND hwnd;
    HANDLE window_ready;
    int win_w, win_h;                    /* starting client size */
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *back_rtv;
    ID3D11VertexShader *vs, *vs_present;
    ID3D11PixelShader *ps, *ps_present;
    ID3D11PixelShader *ps_a2c;           /* ps with alpha-to-coverage (GB_D3D11_MSAA) */

    /* picture quality (GB_D3D11_ANISO, GB_D3D11_MSAA) */
    int tex_q;                           /* 0 top level only, 1 mip levels, 2..16 anisotropic */
    UINT msaa;                           /* samples a pixel, 1 off */
    int a2c;                             /* alpha-to-coverage with msaa */
    int bias_told;
    uint32_t st_tex_mip, st_tex_one3d;   /* 3D texture uses with levels, with one level */
    uint32_t st_a2c, st_resolves;
    uint32_t ab_frames, ab_off;          /* comparison of the quality settings; 0 in a public build */
    ID3D11InputLayout *layout;
    ID3D11Buffer *vb, *cb, *cb_present;

    /* Window. The window thread writes cl_w, cl_h and minimized, then bumps
     * size_gen; it bumps mon_gen when the window may have changed monitor.
     * The executor thread reads them at the top of each flip, so all Direct3D
     * work stays on one thread. */
    int fixed_window;                    /* the fixed-size window; 0 in a public build */
    int start_fullscreen, monitor_pick;
    char monitor_name[128];       /* GB_D3D11_MONITOR given as a name */
    int background, start_minimised;   /* scripted test runs; 0 in a public build */
    int fullscreen;                      /* window thread */
    WINDOWPLACEMENT restore;             /* window thread: where to go back to */
    volatile LONG cl_w, cl_h, minimized, size_gen, mon_gen;
    LONG seen_size_gen, seen_mon_gen;
    int flip_model;                      /* the swap chain is DXGI's flip model */
    UINT back_w, back_h;                 /* swap-chain buffer size */
    float view_x, view_y, view_w, view_h;  /* the picture's rectangle in it */

    /* internal resolution */
    int res_window;                      /* GB_D3D11_RES=window */
    double res;                          /* scale in use */
    double res_want, res_want_ms;        /* window mode: wanted scale, since when */

    /* widescreen (GB_ASPECT, see "aspect" below) */
    double wide;                         /* picture aspect / (4/3); 1 = off */
    double wide_want, wide_want_ms;      /* GB_ASPECT=window: wanted, since when */
    int has3d_cur, has3d_prev;           /* a 3D batch this frame, last frame */
    uint32_t fr_verts3d;                 /* vertices of this frame's 3D batches (the overlay: a backdrop or a court) */
    uint32_t as_sq, as_full, as_3d, as_frames;  /* 2D batches squeezed, left wide; 3D frames */
    uint32_t hist3d;                     /* bit i: a 3D batch i + 1 frames ago */
    uint32_t as_st, as_flips;            /* 2D batches stretched by GB_ASPECT_2D=auto; state changes */
    long views_scene, views_panel;       /* the game's view counters at the last frame (gb_d3d11_view_set) */
    uint32_t scene_frame, panel_frame;   /* the frame each last moved */
    int scene_seen, panel_seen;          /* each has moved at least once */
    int panel_only;                      /* a menu panel's 3D view is the only one in use: its 3D is no 3D scene */
    uint64_t view_hash0;                 /* texture_view: the level-0 hash of the texture it returned, 0 none */
    uint32_t as_anchor;                  /* 2D batches placed by the game on the wide view: left wide */

    /* pacing and display */
    int vsync_req;                       /* -1 auto, 0 off, n refreshes */
    UINT sync;                           /* sync interval in use */
    HMONITOR mon;
    int mon_hz;                          /* the window's monitor, as Windows reports it */

    /* statistics of the display and the GPU */
    UINT fs_count, fs_refresh, fs_sync_refresh;
    LONGLONG fs_sync_qpc;
    int fs_ok, fs_told;
    uint32_t st_cad[5], st_cad_multi;    /* refreshes each displayed frame stayed: 1, 2, 3, 4+ */
    uint32_t st_dp, st_dr;               /* frames displayed, refreshes they took */
    UINT st_hz_refresh0;
    LONGLONG st_hz_qpc0;
    ID3D11Query *q_dis[4], *q_t0[4], *q_t1[4];
    int q_live[4];
    double st_gpu_ms;
    uint32_t st_gpu_n;
    int dump_window;
    uint32_t vb_used;
    ID3D11RasterizerState *raster;
    ID3D11RasterizerState *raster_clip;  /* the same with the scissor on: batches under a window clip */
    ID3D11SamplerState *present_sampler;

    GbTarget targets[GB_MAX_TARGETS];
    GbDepth depths[GB_MAX_DEPTHS];
    GbTexture *textures;
    int ntextures;
    int buckets[GB_TEX_BUCKETS];
    GbBlend blends[GB_MAX_STATES];       int nblends;
    GbDepthState dstates[GB_MAX_STATES]; int ndstates;
    GbSampler samplers[GB_MAX_STATES];   int nsamplers;

    GbTarget *bound_target;
    GbDepth *bound_depth;
    GbTarget *present;                   /* what the next flip shows */
    uint32_t frame;
    uint32_t *decode;                    /* scratch for decoded textures */
    size_t decode_cap;

    /* frame limiter */
    double period_ns;
    LARGE_INTEGER qpf, next;

    /* diagnostics */
    int stats;
    uint64_t st_draws, st_tris, st_tex_new, st_tex_upd, st_hash_bytes, st_dropped;
    double st_ms_draw, st_ms_flip, st_over_ms, st_over_max;
    uint32_t st_late, fr_draws;
    uint32_t st_frames_dropped;     /* frames not presented because the game's thread dropped them */
    uint32_t fr_tris;               /* triangles handed over in the frame being built */
    double fr_ms_draw;
    HANDLE timer;                        /* high-resolution waitable timer for the frame limit */
    FILE *log;                           /* statistics */
    FILE *framelog;                      /* per-frame log; unused in a public build */
    uint32_t fl_calls, fl_draws, fl_tris, fl_clears, fl_put;
    uint64_t fl_sig;
    uint32_t fence_probe_va;
    const char *dump_prefix, *dump_flag;
    uint32_t dump_n;
    DWORD dump_last_ms, dump_ms;
} G;

static void fail(const char *what, HRESULT hr)
{
    gb_log("[D3D11] %s failed (0x%08lX)\n", what, (unsigned long)hr);
    fflush(stderr);
}

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)G.qpf.QuadPart;
}

/* ---- aspect (GB_ASPECT) ------------------------------------------------ */

/* Widescreen and ultrawide, off by default: more of the court at the sides
 * with the 4:3 picture's height kept, not a stretch.
 *
 *   GB_ASPECT=4:3          the console's picture (default; everything below off)
 *   GB_ASPECT=16:9|16:10|21:9|32:9|<w>:<h>|<number>
 *                          a wider picture. Every render target is drawn wider by
 *                          aspect / (4/3) (on top of GB_D3D11_RES), the window
 *                          keeps that shape with black bars, and the game's 3D
 *                          view is widened to match (recomp_manual.c, the
 *                          projection wrapper): the 4:3 picture's height is
 *                          kept and the sides show more of the court
 *   GB_ASPECT=window       the window's (or the monitor's, in fullscreen) own
 *                          shape, between 4:3 and 32:9. A change of shape takes
 *                          effect in the game's 3D view the next time the game
 *                          sets its projection
 *   GB_ASPECT_2D=stretch   menus, HUD and text stretched sideways as on a
 *                          widescreen TV. Default with an aspect: kept at their
 *                          4:3 shape, centred (flat batches, see gb_draw)
 *   GB_ASPECT_2D=auto      screens without a 3D scene (films, title, menus)
 *                          stretched to fill; over a 3D scene (the match, the
 *                          loading court) the 2D kept at 4:3 as by default.
 *                          Decided per frame (see gb_draw)
 *
 * Off (4:3) with the fixed window and on the CPU rasteriser. */
static struct {
    int parsed;
    double aspect;                       /* wanted picture aspect, 4/3 when off */
    int window;                          /* GB_ASPECT=window */
    int game_mode;                       /* the game's own 16:9 mode; 0 in a public build */
    int keep2d;                          /* 2D kept at 4:3 */
    int auto2d;                          /* GB_ASPECT_2D=auto: only over a 3D scene */
} AS;

static void aspect_parse(void)
{
    const char *e;
    double a = 4.0 / 3.0;

    if (AS.parsed)
        return;
    e = getenv("GB_ASPECT");
    if (e && !strcmp(e, "window")) {
        AS.window = 1;
    } else if (e && *e) {
        double n = atof(e), d = 1.0;
        const char *c = strchr(e, ':');
        if (c) d = atof(c + 1);
        if (n > 0.0 && d > 0.0)
            a = n / d;
    }
    if (a < 4.0 / 3.0) a = 4.0 / 3.0;
    if (a > 32.0 / 9.0) a = 32.0 / 9.0;
    e = getenv("GB_ASPECT_2D");
    AS.keep2d = !(e && !strcmp(e, "stretch"));
    AS.auto2d = e && !strcmp(e, "auto");
    AS.aspect = a;
    AS.parsed = 1;
}

/* For the game side (recomp_manual.c): the aspect the 3D view should have,
 * 4/3 when widescreen is off; and 0 off, 1 wider view, 2 the game's own mode. */
double gb_d3d11_aspect(void)
{
    aspect_parse();
    if (AS.window)
        return G.wide > 0.0 ? G.wide * 4.0 / 3.0 : 4.0 / 3.0;
    return AS.aspect;
}

/* For the game side: how 2D is drawn with a wide aspect. 0 kept at 4:3
 * (default), 1 GB_ASPECT_2D=auto, 2 stretched. */
int gb_d3d11_aspect_2d(void)
{
    aspect_parse();
    return AS.auto2d ? 1 : AS.keep2d ? 0 : 2;
}

/* The game side counts the 3D views it sets up (recomp_manual.c, the
 * projection wrapper): a menu panel (Create A Baller's player) or any other
 * (the court). At each frame the back end notes which moved recently; while
 * only a panel's did, that panel's 3D is not a "3D scene": the menu around it
 * is drawn as any other menu (kept at 4:3 with black sides, or stretched by
 * GB_ASPECT_2D=auto), and the game side fits the panel's view to that. */
static volatile LONG views_scene, views_panel;

void gb_d3d11_view_set(int panel)
{
    InterlockedIncrement(panel ? &views_panel : &views_scene);
}

/* 2D the game places on the 3D view, by the picture it draws with (its
 * texture's level 0, hashed when it is uploaded; the palette is left out, so a
 * recoloured copy matches, and so are the other levels, so the texture
 * filtering setting does not matter). The game works out where such a marker goes from the
 * camera, which is the wider one, so its position is already right for the
 * wide picture: squeezed like the rest of the HUD it would sit inward of the
 * player it belongs to. Over a 3D scene these are left as they are.
 *   - the player marker: the player's number in a box, with an arrow toward a
 *     player off the screen, at the edge (256x128) */
static int anchored_2d(uint64_t h0)
{
    static const uint64_t known[] = { 0x9D1A8106E6ACD23Dull };
    size_t i;
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (known[i] && known[i] == h0)
            return 1;
    return 0;
}

int gb_d3d11_aspect_mode(void)
{
    aspect_parse();
    if (AS.game_mode)
        return 2;
    return (AS.window || AS.aspect > 4.0 / 3.0 + 1e-6) ? 1 : 0;
}

/* GB_ASPECT=window: the window's shape, within 4:3 to 32:9. */
static double window_aspect(void)
{
    double a;
    if (!G.back_w || !G.back_h)
        return 4.0 / 3.0;
    a = (double)G.back_w / (double)G.back_h;
    return a < 4.0 / 3.0 ? 4.0 / 3.0 : a > 32.0 / 9.0 ? 32.0 / 9.0 : a;
}

/* The picture's rectangle in the swap-chain buffer: the largest centred
 * rectangle of the picture's shape (4:3, or GB_ASPECT), black bars around it.
 * The fixed window fills its buffer, as it always did. In whole pixels,
 * rounded, so a 1280x960 window is all picture. */
static void compute_view(void)
{
    UINT bw = G.back_w, bh = G.back_h, w = bw, h = bh;
    double pa = (G.wide > 0.0 ? G.wide : 1.0) * 4.0 / 3.0;
    if (!G.fixed_window && bw && bh) {
        if (G.wide <= 1.0 + 1e-9) {      /* exactly as before */
            if (bw * 3 > bh * 4)
                w = (bh * 4 + 1) / 3;
            else
                h = (bw * 3 + 2) / 4;
        } else if ((double)bw > (double)bh * pa) {
            w = (UINT)((double)bh * pa + 0.5);
        } else {
            h = (UINT)((double)bw / pa + 0.5);
        }
    }
    G.view_w = (float)w;
    G.view_h = (float)h;
    G.view_x = (float)((bw - w) / 2);
    G.view_y = (float)((bh - h) / 2);
}

/* GB_D3D11_RES=window: the scale at which the game's 480 lines fill the
 * picture's height on screen, at least 1. */
static double res_for_window(void)
{
    double r;
    compute_view();
    r = G.view_h / (double)GB_LOGICAL_H;
    return r < 1.0 ? 1.0 : r > GB_MAX_RES ? GB_MAX_RES : r;
}

/* ---- window ------------------------------------------------------------ */

/* Everything in this section runs on the window thread.
 *
 * Fullscreen is a borderless window that covers the monitor, not exclusive
 * fullscreen: no display mode change, Alt+Tab and other monitors keep working,
 * and the picture still comes through the same swap chain. DXGI is told to
 * leave Alt+Enter alone (MakeWindowAssociation in gb_init_once). */
#define GB_WINDOWED_STYLE (WS_OVERLAPPEDWINDOW)
#define GB_FIXED_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

static void set_fullscreen(HWND h, int on)
{
    MONITORINFO mi;
    LONG style = GetWindowLongA(h, GWL_STYLE);

    if (G.fixed_window || on == G.fullscreen)
        return;
    if (on) {
        mi.cbSize = sizeof mi;
        G.restore.length = sizeof G.restore;
        if (!GetWindowPlacement(h, &G.restore)
            || !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        G.fullscreen = 1;
        SetWindowLongA(h, GWL_STYLE, (style & ~GB_WINDOWED_STYLE) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW
                     | (G.background ? SWP_NOACTIVATE : 0));
    } else {
        G.fullscreen = 0;
        SetWindowLongA(h, GWL_STYLE, (style & ~WS_POPUP) | GB_WINDOWED_STYLE);
        if (G.background && G.restore.showCmd == SW_SHOWNORMAL)
            G.restore.showCmd = SW_SHOWNOACTIVATE;   /* SW_SHOWNORMAL would take focus */
        SetWindowPlacement(h, &G.restore);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (G.background ? SWP_NOACTIVATE : 0));
    }
}

/* Keep a fullscreen window covering its monitor when the monitor's mode or
 * the desktop layout changes. */
static void refit_fullscreen(HWND h)
{
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (G.fullscreen && GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

/* Keyboard focus, for pausing the game when its window goes to the
 * background (recomp_manual.c, read on the game's thread). "Lost" needs the
 * window to have had focus once: a window that never had it (scripted
 * background runs) is never treated as having lost it. */
static volatile LONG s_focus_has, s_focus_ever;

int gb_window_focus_lost(void)
{
    return s_focus_ever && !s_focus_has;
}

static LRESULT CALLBACK gb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) {
            G.minimized = 1;
        } else {
            G.minimized = 0;
            G.cl_w = LOWORD(l);
            G.cl_h = HIWORD(l);
        }
        InterlockedIncrement(&G.size_gen);
        return 0;
    case WM_MOVE:
        InterlockedIncrement(&G.mon_gen);
        return 0;
    case WM_DISPLAYCHANGE:
        refit_fullscreen(h);
        InterlockedIncrement(&G.mon_gen);
        return 0;
    case 0x02E0: /* WM_DPICHANGED: moved to a monitor with another scale. Take
                  * the size Windows suggests, so the window keeps its size on
                  * screen (the picture is redrawn for it, never bitmap-scaled). */
        if (!G.fullscreen) {
            const RECT *r = (const RECT *)l;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InterlockedIncrement(&G.mon_gen);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)l)->ptMinTrackSize.x = 240;
        ((MINMAXINFO *)l)->ptMinTrackSize.y = 200;
        return 0;
    case WM_ERASEBKGND:
        return 1;                       /* the swap chain paints every pixel */
    case WM_SETCURSOR:
        if (G.fullscreen && LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);            /* no pointer over a fullscreen picture */
            return TRUE;
        }
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN)
            return 0;                   /* Alt+Enter: no error beep */
        break;
    case WM_TIMER:
        if (w == GB_PAD_TIMER) {
            /* "Controller N disconnected" in the title bar, from gb_pad.c. */
            static unsigned shown;
            char notice[160], title[224];
            unsigned gen = gb_pad_notice(notice, sizeof notice);
            if (gen != shown) {
                shown = gen;
                if (notice[0])
                    snprintf(title, sizeof title, GB_WINDOW_TITLE ": %s", notice);
                else
                    snprintf(title, sizeof title, GB_WINDOW_TITLE);
                SetWindowTextA(h, title);
            }
            return 0;
        }
        break;
    case WM_DEVICECHANGE:
        gb_pad_devices_changed();       /* a controller may have arrived: look at once */
        break;
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window ends the run, as the toolkit's own window does:
         * the guest threads have no exit path of their own. */
        gb_log("[D3D11] window closed: exiting\n");
        gb_pad_shutdown();              /* motors off: TerminateProcess skips atexit */
        gb_bgsound_shutdown();          /* the mixer's level back, for the same reason */
        gb_save_before_exit("window");  /* a save being written finishes first (gb_save.c) */
        recomp_exit_hook_run("window");
        fflush(stdout);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int side;
        /* Alt+Enter and F11 switch fullscreen and do not reach the pad (Enter
         * alone is START). Held keys repeat: act on the first press only. */
        /* F12 saves a screenshot (gb_overlay.c); it does not reach the pad. */
        if (w == VK_F12 && m == WM_KEYDOWN) {
            if (!(l & (1 << 30)))
                gb_overlay_request_shot();
            return 0;
        }
        if (!G.fixed_window && ((w == VK_RETURN && m == WM_SYSKEYDOWN && (l & (1 << 29)))
                                || (w == VK_F11 && m == WM_KEYDOWN))) {
            if (!(l & (1 << 30)))
                set_fullscreen(h, !G.fullscreen);
            return 0;
        }
        side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
        xbox_InputKeySet((int)w, 1);
        if (side)
            xbox_InputKeySet(side, 1);
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
        xbox_InputKeySet((int)w, 0);
        if (side)
            xbox_InputKeySet(side, 0);
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    }
    case WM_SETFOCUS:
        InterlockedExchange(&s_focus_has, 1);
        InterlockedExchange(&s_focus_ever, 1);
        return 0;
    case WM_KILLFOCUS:
        InterlockedExchange(&s_focus_has, 0);
        xbox_InputKeysClear();          /* or a key held across Alt-Tab stays held */
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

typedef struct { HMONITOR list[16]; int n; } GbMonitors;

static BOOL CALLBACK collect_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM p)
{
    GbMonitors *ms = (GbMonitors *)p;
    (void)dc; (void)r;
    if (ms->n < 16)
        ms->list[ms->n++] = m;
    return TRUE;
}

/* The display whose name (as Windows Settings shows it, from the monitor's
 * EDID) is `name`, as the GDI device name ("\\.\DISPLAYn") that
 * MONITORINFOEX.szDevice carries. Asks Windows' display configuration: each
 * active path joins a target (the monitor, with its friendly name) to a
 * source (the desktop surface, with its GDI name). Logs every name it saw
 * when there is no match. Returns 1 if found. */
static int display_by_name(const char *name, char *gdi, size_t gdi_size)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    int found = 0;
    char seen[512] = "";

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
        return 0;
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np ? np : 1, sizeof *paths);
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof *modes);
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (i = 0; i < np && !found; i++) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            char friendly[128];

            memset(&tn, 0, sizeof tn);
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tn.header.size = sizeof tn;
            tn.header.adapterId = paths[i].targetInfo.adapterId;
            tn.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS)
                continue;
            WideCharToMultiByte(CP_UTF8, 0, tn.monitorFriendlyDeviceName, -1, friendly, sizeof friendly, NULL, NULL);
            if (strlen(seen) + strlen(friendly) + 4 < sizeof seen)
                strcat(strcat(strcat(seen, seen[0] ? ", \"" : "\""), friendly), "\"");
            if (_stricmp(friendly, name))
                continue;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS) {
                WideCharToMultiByte(CP_UTF8, 0, sn.viewGdiDeviceName, -1, gdi, (int)gdi_size, NULL, NULL);
                found = 1;
            }
        }
    }
    if (!found)
        gb_log("[D3D11] GB_D3D11_MONITOR=\"%s\": no active display of that name (seen: %s)\n",
                name, seen[0] ? seen : "none");
    free(paths);
    free(modes);
    return found;
}

/* The monitor to start on: GB_D3D11_MONITOR=n (1-based, Windows' order) or a
 * display name, else the primary one. */
static HMONITOR start_monitor(void)
{
    GbMonitors ms;
    POINT origin = { 0, 0 };
    int k;
    ms.n = 0;
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)&ms);
    if (G.monitor_pick > 0) {
        if (G.monitor_pick <= ms.n)
            return ms.list[G.monitor_pick - 1];
        gb_log("[D3D11] GB_D3D11_MONITOR=%d: there are %d monitors\n", G.monitor_pick, ms.n);
    } else if (G.monitor_name[0]) {
        char gdi[64];
        if (display_by_name(G.monitor_name, gdi, sizeof gdi)) {
            for (k = 0; k < ms.n; k++) {
                MONITORINFOEXA mi;
                memset(&mi, 0, sizeof mi);
                mi.cbSize = sizeof mi;
                if (GetMonitorInfoA(ms.list[k], (MONITORINFO *)&mi) && !_stricmp(mi.szDevice, gdi)) {
                    gb_log("[D3D11] GB_D3D11_MONITOR=\"%s\": %s, monitor %d in Windows' order\n",
                            G.monitor_name, gdi, k + 1);
                    return ms.list[k];
                }
            }
            gb_log("[D3D11] GB_D3D11_MONITOR=\"%s\": %s has no desktop\n", G.monitor_name, gdi);
        }
    }
    if (G.monitor_pick > 0 || G.monitor_name[0]) {
        gb_log("[D3D11] using the primary monitor\n");
    }
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

static DWORD WINAPI gb_window_thread(LPVOID unused)
{
    WNDCLASSA wc;
    RECT r, wr, cr;
    MSG msg;
    MONITORINFO mi;
    DWORD style = G.fixed_window ? GB_FIXED_STYLE : GB_WINDOWED_STYLE;
    int cw = G.win_w, ch = G.win_h, x = 60, y = 40;

    (void)unused;
    if (!G.fixed_window) {
        /* Per-monitor DPI aware (this thread's windows only), so that on a
         * monitor scaled to 150% Windows does not stretch the picture a second
         * time; WM_DPICHANGED keeps the window's size on screen instead.
         * Looked up at run time: Windows 10 1703 and later. */
        typedef void *(WINAPI *SetCtxFn)(void *);
        SetCtxFn set_ctx = (SetCtxFn)(void *)GetProcAddress(GetModuleHandleA("user32.dll"),
                                                             "SetThreadDpiAwarenessContext");
        if (set_ctx)
            set_ctx((void *)(intptr_t)-4);  /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    }
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = gb_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    /* The program's icon (port\res\gamebreaker_game.rc, resource 1) for the
     * taskbar and Alt+Tab; NULL, Windows' plain one, if it is not built in. */
    wc.hIcon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.lpszClassName = "GamebreakerD3D11";
    RegisterClassA(&wc);
    mi.cbSize = sizeof mi;
    if (!G.fixed_window && GetMonitorInfoA(start_monitor(), &mi)) {
        /* Near the top-left of the monitor's work area. A window larger than
         * nine tenths of it is made smaller, keeping 4:3. */
        int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
        if (cw > aw * 9 / 10 || ch > ah * 9 / 10) {
            double s = (double)aw * 0.9 / cw;
            if ((double)ah * 0.9 / ch < s) s = (double)ah * 0.9 / ch;
            cw = (int)(cw * s);
            ch = (int)(ch * s);
        }
        x = mi.rcWork.left + 60;
        y = mi.rcWork.top + 40;
    }
    r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    AdjustWindowRect(&r, style, FALSE);
    /* Near the top-left of the monitor, so that the whole window is on screen
     * (the default position can hang it over the bottom edge). */
    /* Background: WS_EX_NOACTIVATE keeps the window from ever becoming the
     * foreground window (even through the first ShowWindow of a process,
     * which Windows may run with the launcher's show command instead of
     * ours); WS_EX_APPWINDOW keeps its taskbar button, so it can be found and
     * closed. */
    G.hwnd = CreateWindowExA(G.background ? (WS_EX_NOACTIVATE | WS_EX_APPWINDOW) : 0,
                             wc.lpszClassName, GB_WINDOW_TITLE, style,
                             x, y, r.right - r.left,
                             r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (G.hwnd) {
        /* AdjustWindowRect knows only the system's scale; correct the frame
         * for the monitor's own, so the client area is exactly cw x ch. */
        GetWindowRect(G.hwnd, &wr);
        GetClientRect(G.hwnd, &cr);
        if (cr.right != cw || cr.bottom != ch)
            SetWindowPos(G.hwnd, NULL, 0, 0, (wr.right - wr.left) + cw - cr.right,
                         (wr.bottom - wr.top) + ch - cr.bottom,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(G.hwnd, !G.background ? SW_SHOWNORMAL
                           : G.start_minimised ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
        GetClientRect(G.hwnd, &cr);
        G.cl_w = cr.right;
        G.cl_h = cr.bottom;
        if (G.start_minimised) {
            G.cl_w = cw;            /* the size it would have had: the back end */
            G.cl_h = ch;            /* sets up its swap chain at that size */
            G.minimized = 1;
        }
        if (G.start_fullscreen && !G.start_minimised)
            set_fullscreen(G.hwnd, 1);
        gb_pad_window(G.hwnd);
        gb_bgsound_window(G.hwnd);
        SetTimer(G.hwnd, GB_PAD_TIMER, 250, NULL);
        {
            /* The title bar's small icon, drawn from the icon's own 16-pixel
             * image rather than shrunk from the large one. */
            /* (Not named "small": rpcndr.h defines that as char under MSVC.) */
            HICON icon_sm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), 0);
            if (icon_sm)
                SendMessageA(G.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_sm);
        }
    }
    SetEvent(G.window_ready);
    while (G.hwnd && GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* ---- shaders ----------------------------------------------------------- */

/* The pixel shader evaluates the NV2A's register combiners from the words the
 * title programmed, handed over in a constant buffer (GbConstants below). It
 * is a transcription of the toolkit's reference evaluator, nv2a_combiner.c
 * (nv2a_rc_eval), and of the texture-stage modes in nv2a_pb_exec.c
 * (rc_stage_fetch): same registers, same input and output mappings, same
 * order of reads and writes. Register numbers: 0 zero, 1 c0, 2 c1, 3 fog,
 * 4 diffuse, 5 specular, 8-11 texture stages, 12 r0, 13 r1, 14 the final
 * combiner's sum, 15 its product. */
static const char GB_HLSL[] =
"cbuffer C : register(b0) {\n"
"    uint4  cw[8];\n"          /* per stage: colour ICW, alpha ICW, colour OCW, alpha OCW */
"    float4 f0[8];\n"          /* SET_COMBINER_FACTOR0 */
"    float4 f1[8];\n"          /* SET_COMBINER_FACTOR1 */
"    uint4  fin;\n"            /* final CW0, final CW1, COMBINER_CONTROL, SHADER_STAGE_PROGRAM */
"    float4 fc0;\n"            /* SET_SPECULAR_FOG_FACTOR 0 and 1 */
"    float4 fc1;\n"
"    float4 fogc;\n"           /* fog colour */
"    uint4  misc;\n"           /* alpha func (0 off, 1..8), alpha ref, flags, clip-plane modes */
"};\n"                         /* flags: 1 combiners, 2 stage 0 texture (no combiners), 16<<i stage i bound */
"Texture2D t0 : register(t0); Texture2D t1 : register(t1);\n"
"Texture2D t2 : register(t2); Texture2D t3 : register(t3);\n"
"SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
"SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n"
"struct VI { float4 pos : POSITION; float4 col : COLOR0; float4 spec : COLOR1; float fog : FOG;\n"
"            float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n"
"struct VO { float4 pos : SV_Position; float4 col : COLOR0; float4 spec : COLOR1; float fog : FOG;\n"
"            float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n"
"VO vs(VI i) { VO o; o.pos = i.pos; o.col = i.col; o.spec = i.spec; o.fog = i.fog;\n"
"    o.tc0 = i.tc0; o.tc1 = i.tc1; o.tc2 = i.tc2; o.tc3 = i.tc3; return o; }\n"
"float mapin(float x, uint b) {\n"                 /* PS_INPUTMAPPING */
"    uint m = (b >> 5) & 7u;\n"
"    float p = max(x, 0.0);\n"
"    if (m == 0u) return p;\n"
"    if (m == 1u) return 1.0 - saturate(x);\n"
"    if (m == 2u) return 2.0 * p - 1.0;\n"
"    if (m == 3u) return -2.0 * p + 1.0;\n"
"    if (m == 4u) return p - 0.5;\n"
"    if (m == 5u) return -p + 0.5;\n"
"    if (m == 6u) return x;\n"
"    return -x;\n"
"}\n"
"float3 rgbin(float4 r, uint b) {\n"
"    float3 v = (b & 16u) != 0u ? r.aaa : r.rgb;\n"
"    return float3(mapin(v.x, b), mapin(v.y, b), mapin(v.z, b));\n"
"}\n"
"float ain(float4 r, uint b) { return mapin((b & 16u) != 0u ? r.a : r.b, b); }\n"
"float3 mapout(float3 x, uint m) {\n"              /* PS_COMBINEROUTPUT scale and bias */
"    if (m == 0x08u) return x - 0.5;\n"
"    if (m == 0x10u) return x * 2.0;\n"
"    if (m == 0x18u) return (x - 0.5) * 2.0;\n"
"    if (m == 0x20u) return x * 4.0;\n"
"    if (m == 0x30u) return x * 0.5;\n"
"    return x;\n"
"}\n"
"float4 stage(uint st, float4 tc, float4 smp) {\n" /* what texture stage st contributes */
"    uint mode = (fin.w >> (st * 5u)) & 31u;\n"
"    if (mode == 0u) return float4(0, 0, 0, 1);\n"
"    if (mode == 4u) return saturate(tc);\n"
"    if (mode >= 5u) return float4(0, 0, 0, 0);\n"
"    return ((misc.z >> (4u + st)) & 1u) != 0u ? smp : float4(1, 1, 1, 1);\n"
"}\n"
"bool clipped(uint st, float4 tc) {\n"             /* stage mode 5: clip planes */
"    if (((fin.w >> (st * 5u)) & 31u) != 5u) return false;\n"
"    bool kill = false;\n"
"    for (uint j = 0u; j < 4u; j++) {\n"
"        bool ge = ((misc.w >> (st * 4u + j)) & 1u) != 0u;\n"
"        if (ge ? tc[j] >= 0.0 : tc[j] < 0.0) kill = true;\n"
"    }\n"
"    return kill;\n"
"}\n"
"#ifdef GB_A2C\n"
"float4 ps(VO i, out uint cov : SV_Coverage) : SV_Target {\n"
"    cov = 0xFFFFFFFFu;\n"
"#else\n"
"float4 ps(VO i) : SV_Target {\n"
"#endif\n"
"    uint flags = misc.z;\n"
"    float4 x0 = t0.Sample(s0, i.tc0.xy / (i.tc0.w != 0.0 ? i.tc0.w : 1.0));\n"
"    float4 x1 = t1.Sample(s1, i.tc1.xy / (i.tc1.w != 0.0 ? i.tc1.w : 1.0));\n"
"    float4 x2 = t2.Sample(s2, i.tc2.xy / (i.tc2.w != 0.0 ? i.tc2.w : 1.0));\n"
"    float4 x3 = t3.Sample(s3, i.tc3.xy / (i.tc3.w != 0.0 ? i.tc3.w : 1.0));\n"
"    float4 c;\n"
"    if ((flags & 1u) != 0u) {\n"
"        if (clipped(0u, i.tc0) || clipped(1u, i.tc1) || clipped(2u, i.tc2) || clipped(3u, i.tc3)) discard;\n"
"        float4 R[16];\n"
"        for (int k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"        float4 T0 = stage(0u, i.tc0, x0);\n"
"        R[3] = float4(fogc.rgb, i.fog); R[4] = i.col; R[5] = i.spec;\n"
"        R[8] = T0; R[9] = stage(1u, i.tc1, x1); R[10] = stage(2u, i.tc2, x2); R[11] = stage(3u, i.tc3, x3);\n"
"        R[12] = float4(0, 0, 0, (fin.w & 31u) != 0u ? T0.a : 1.0);\n"
"        uint n = min(fin.z & 255u, 8u), cf = fin.z >> 8;\n"
"        float4 v;\n"
"        for (uint s = 0u; s < n; s++) {\n"
"            uint icw = cw[s].x, aicw = cw[s].y, ocw = cw[s].z, aocw = cw[s].w;\n"
"            uint fl = ocw >> 12, afl = aocw >> 12, d;\n"
"            R[1] = f0[(cf & 0x10u) != 0u ? s : 0u];\n"
"            R[2] = f1[(cf & 0x100u) != 0u ? s : 0u];\n"
"            bool muxcd = (cf & 1u) != 0u ? R[12].a >= 0.5 : ((uint)(R[12].a * 255.0) & 1u) != 0u;\n"
"            float3 A = rgbin(R[(icw >> 24) & 15u], icw >> 24), B = rgbin(R[(icw >> 16) & 15u], icw >> 16);\n"
"            float3 C = rgbin(R[(icw >> 8) & 15u], icw >> 8), D = rgbin(R[icw & 15u], icw);\n"
"            float aA = ain(R[(aicw >> 24) & 15u], aicw >> 24), aB = ain(R[(aicw >> 16) & 15u], aicw >> 16);\n"
"            float aC = ain(R[(aicw >> 8) & 15u], aicw >> 8), aD = ain(R[aicw & 15u], aicw);\n"
"            float3 pab = (fl & 2u) != 0u ? dot(A, B).xxx : A * B;\n"
"            float3 pcd = (fl & 1u) != 0u ? dot(C, D).xxx : C * D;\n"
"            float3 sum = (fl & 4u) != 0u ? (muxcd ? pcd : pab) : pab + pcd;\n"
"            float3 ab = clamp(mapout(pab, fl & 0x38u), -1.0, 1.0);\n"
"            float3 cd = clamp(mapout(pcd, fl & 0x38u), -1.0, 1.0);\n"
"            float3 ms = clamp(mapout(sum, fl & 0x38u), -1.0, 1.0);\n"
"            float3 ap = float3(aA * aB, aC * aD, 0.0);\n"
"            ap.z = (afl & 4u) != 0u ? (muxcd ? ap.y : ap.x) : ap.x + ap.y;\n"
"            ap = clamp(mapout(ap, afl & 0x38u), -1.0, 1.0);\n"
"            d = (ocw >> 4) & 15u;  if (d != 0u) { v = R[d]; v.rgb = ab; if ((fl & 0x80u) != 0u) v.a = ab.b; R[d] = v; }\n"
"            d = ocw & 15u;         if (d != 0u) { v = R[d]; v.rgb = cd; if ((fl & 0x40u) != 0u) v.a = cd.b; R[d] = v; }\n"
"            d = (ocw >> 8) & 15u;  if (d != 0u) { v = R[d]; v.rgb = ms; R[d] = v; }\n"
"            d = (aocw >> 4) & 15u; if (d != 0u) { v = R[d]; v.a = ap.x; R[d] = v; }\n"
"            d = aocw & 15u;        if (d != 0u) { v = R[d]; v.a = ap.y; R[d] = v; }\n"
"            d = (aocw >> 8) & 15u; if (d != 0u) { v = R[d]; v.a = ap.z; R[d] = v; }\n"
"            R[0] = float4(0, 0, 0, 0);\n"
"        }\n"
"        if (fin.x != 0u || fin.y != 0u) {\n"      /* final combiner: D + mix(C, B, A), alpha from G */
"            uint fa = fin.x, fb = fin.y, ff = fb & 255u;\n"
"            R[1] = fc0; R[2] = fc1;\n"
"            float3 sa = (ff & 0x40u) != 0u ? 1.0 - R[5].rgb : R[5].rgb;\n"
"            float3 sb = (ff & 0x20u) != 0u ? 1.0 - R[12].rgb : R[12].rgb;\n"
"            R[14] = float4((ff & 0x80u) != 0u ? saturate(sa + sb) : sa + sb, 0.0);\n"
"            float3 E = rgbin(R[(fb >> 24) & 15u], fb >> 24), F = rgbin(R[(fb >> 16) & 15u], fb >> 16);\n"
"            R[15] = float4(E * F, 0.0);\n"
"            float3 fA = rgbin(R[(fa >> 24) & 15u], fa >> 24), fB = rgbin(R[(fa >> 16) & 15u], fa >> 16);\n"
"            float3 fC = rgbin(R[(fa >> 8) & 15u], fa >> 8), fD = rgbin(R[fa & 15u], fa);\n"
"            c = float4(fD + fC * (1.0 - fA) + fB * fA, ain(R[(fb >> 8) & 15u], fb >> 8));\n"
"        } else {\n"
"            c = R[12];\n"
"        }\n"
"        c = saturate(c);\n"
"    } else {\n"                                   /* no combiners: stage 0 times diffuse */
"        c = i.col;\n"
"        if ((flags & 2u) != 0u) c *= x0;\n"
"    }\n"
"    uint f = misc.x;\n"
/* Alpha-to-coverage (GB_D3D11_MSAA): instead of keeping or dropping the whole
 * pixel, a cut-out edge covers as many of the pixel's samples as the alpha's
 * distance past the reference, measured in its own screen gradient. Only for
 * GREATER and GEQUAL (all this title uses). The colour, alpha included, is
 * written as the game computed it; only the coverage changes. The reference
 * is taken half a step up, so that the 0.5 coverage line sits on the same
 * edge the discard below would cut, and an alpha of 0 against a reference of
 * 0 covers nothing. */
"#ifdef GB_A2C\n"
"    if (f == 5u || f == 7u) {\n"
"        float a = saturate(c.a), r = ((float)misc.y + (f == 5u ? 0.5 : -0.5)) / 255.0;\n"
"        float s = saturate((a - r) / max(fwidth(a), 1e-4) + 0.5);\n"
"        uint k = (uint)(s * (float)GetRenderTargetSampleCount() + 0.5);\n"
"        if (k == 0u) discard;\n"
"        cov = k >= 32u ? 0xFFFFFFFFu : (1u << k) - 1u;\n"
"        f = 0u;\n"
"    }\n"
"#endif\n"
"    if (f != 0u) {\n"                             /* GL_NEVER..GL_ALWAYS as 1..8, on 8-bit alpha */
"        float a = floor(saturate(c.a) * 255.0 + 0.5), r = (float)misc.y;\n"
"        bool keep = true;\n"
"        if (f == 1u) keep = false;\n"
"        else if (f == 2u) keep = a < r;\n"
"        else if (f == 3u) keep = a == r;\n"
"        else if (f == 4u) keep = a <= r;\n"
"        else if (f == 5u) keep = a > r;\n"
"        else if (f == 6u) keep = a != r;\n"
"        else if (f == 7u) keep = a >= r;\n"
"        if (!keep) discard;\n"
"    }\n"
"    return c;\n"
"}\n"
"struct PO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"PO vs_present(uint id : SV_VertexID) {\n"         /* one triangle that covers the screen */
"    PO o; o.uv = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
/* Showing the picture: one bilinear sample when it is enlarged or shown at its
 * size (as before), and when it is reduced (an internal resolution larger than
 * the window) the mean of an n x m grid of samples over the window pixel's
 * footprint, so the reduction averages instead of skipping pixels. */
"cbuffer P : register(b1) { float4 pp; };\n"      /* footprint of a window pixel in uv, taps across, taps down */
"float4 ps_present(PO i) : SV_Target {\n"
"    uint nx = (uint)pp.z, ny = (uint)pp.w;\n"
"    if (nx <= 1u && ny <= 1u) return float4(t0.Sample(s0, i.uv).rgb, 1);\n"
"    float3 acc = float3(0, 0, 0);\n"
"    for (uint y = 0u; y < ny; y++)\n"
"        for (uint x = 0u; x < nx; x++)\n"
"            acc += t0.Sample(s0, i.uv + ((float2(x, y) + 0.5) / float2(nx, ny) - 0.5) * pp.xy).rgb;\n"
"    return float4(acc / (float)(nx * ny), 1);\n"
"}\n";

/* The constant buffer above, as C sees it. */
typedef struct {
    uint32_t cw[8][4];
    float    f0[8][4], f1[8][4];
    uint32_t fin[4];
    float    fc0[4], fc1[4], fogc[4];
    uint32_t misc[4];
} GbConstants;

static ID3DBlob *compile_with(const char *entry, const char *target, const D3D_SHADER_MACRO *defs)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(GB_HLSL, sizeof GB_HLSL - 1, "gb_d3d11", defs, NULL,
                            entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        gb_log("[D3D11] shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    return compile_with(entry, target, NULL);
}

/* ---- device ------------------------------------------------------------ */

static int gb_init_once(void)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
    ID3D11Texture2D *back = NULL;
    ID3DBlob *b;
    HRESULT hr;
    const char *e;
    int scale = 2;

    if (G.tried)
        return G.ok;
    G.tried = 1;
    QueryPerformanceFrequency(&G.qpf);

    G.win_w = GB_LOGICAL_W * scale;
    G.win_h = GB_LOGICAL_H * scale;
    aspect_parse();
    G.wide = AS.aspect * 3.0 / 4.0;
    if (G.wide > 1.0 + 1e-9)             /* a window of the picture's shape */
        G.win_w = (int)(GB_LOGICAL_W * scale * G.wide + 0.5);
    if (AS.window || G.wide > 1.0 + 1e-9)
        gb_log("[GB] aspect: %s%.4f (%s), 2D %s\n",
                AS.window ? "window, starting at " : "", AS.aspect,
                AS.game_mode ? "the game's own 16:9 mode" : "wider 3D view",
                AS.auto2d ? "stretched without a 3D scene, kept at 4:3 over one"
                : AS.keep2d ? "kept at 4:3" : "stretched");
    e = getenv("GB_D3D11_FULLSCREEN");
    G.start_fullscreen = e && atoi(e) > 0 && !G.fixed_window;
    e = getenv("GB_D3D11_MONITOR");
    G.monitor_pick = 0;
    G.monitor_name[0] = 0;
    if (e && *e && strspn(e, "0123456789") == strlen(e))
        G.monitor_pick = atoi(e);
    else if (e && *e)
        snprintf(G.monitor_name, sizeof G.monitor_name, "%s", e);
    G.res = 1.0;
    e = getenv("GB_D3D11_RES");
    if (e && !strcmp(e, "window"))
        G.res_window = 1;
    else if (e && atof(e) >= 1.0)
        G.res = atof(e) > GB_MAX_RES ? GB_MAX_RES : atof(e);
    e = getenv("GB_D3D11_VSYNC");
    G.vsync_req = !e ? 0 : !strcmp(e, "auto") ? -1 : atoi(e) < 0 ? 0 : atoi(e) > 4 ? 4 : atoi(e);
    e = getenv("GB_D3D11_ANISO");
    G.tex_q = !e ? 0 : atoi(e) <= 0 ? 0 : atoi(e) > 16 ? 16 : atoi(e);
    e = getenv("GB_D3D11_MSAA");
    G.msaa = (e && atoi(e) >= 2) ? (UINT)(atoi(e) >= 8 ? 8 : atoi(e) >= 4 ? 4 : 2) : 1;
    /* The frame limit is the game's step rate (GB_GAME_HZ, recomp_manual.c;
     * 60 unless set): one step per frame, so a limit that differs from the
     * step rate is a change of game speed. */
    G.period_ns = 1e9 / (double)gb_game_hz();
    G.log = stderr;
    if (gb_game_hz() != 60 || G.period_ns != 1e9 / 60.0)
        gb_log("[GB] d3d11: frame limit %.2f a second (game rate %d)\n",
               G.period_ns > 0.0 ? 1e9 / G.period_ns : 0.0, gb_game_hz());
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    G.timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);
    if (!G.timer)                        /* before Windows 10 1803: an ordinary timer */
        G.timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);

    G.window_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, gb_window_thread, NULL, 0, NULL));
    WaitForSingleObject(G.window_ready, 5000);
    if (!G.hwnd) {
        gb_log("[D3D11] could not create the window\n");
        return 0;
    }

    {
        /* Which graphics chip (see "graphics chip" at the end of this file):
         * GB_D3D11_ADAPTER unset or "default": Windows' choice, which on a
         * laptop with two chips the exported hints turn into the dedicated
         * one. "high": the one DXGI ranks for high performance. A number:
         * that adapter in DXGI's order (0 is the first). */
        IDXGIAdapter1 *pick = NULL;
        const char *a = getenv("GB_D3D11_ADAPTER");
        if (a && *a && strcmp(a, "default")) {
            IDXGIFactory1 *f1 = NULL;
            if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1))) {
                if (!strcmp(a, "high")) {
                    IDXGIFactory6 *f6 = NULL;
                    if (SUCCEEDED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory6, (void **)&f6))) {
                        IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                 &IID_IDXGIAdapter1, (void **)&pick);
                        IDXGIFactory6_Release(f6);
                    }
                } else if (strspn(a, "0123456789") == strlen(a)) {
                    if (FAILED(IDXGIFactory1_EnumAdapters1(f1, (UINT)atoi(a), &pick)))
                        pick = NULL;
                }
                IDXGIFactory1_Release(f1);
            }
            if (!pick)
                gb_log("[D3D11] GB_D3D11_ADAPTER=%s: no such adapter, Windows' choice used\n", a);
        }
        hr = D3D11CreateDevice((IDXGIAdapter *)pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                               NULL, 0, &want, 1, D3D11_SDK_VERSION, &G.dev, &got, &G.ctx);
        if (FAILED(hr) && pick) {
            fail("D3D11CreateDevice on the adapter asked for (trying Windows' choice)", hr);
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1,
                                   D3D11_SDK_VERSION, &G.dev, &got, &G.ctx);
        }
        if (pick)
            IDXGIAdapter1_Release(pick);
    }
    if (FAILED(hr)) { fail("D3D11CreateDevice", hr); return 0; }
    if (G.msaa > 1) {
        /* The largest count up to the one asked for that both the colour and
         * the depth format support. */
        UINT want_n = G.msaa, n, qc, qd;
        for (n = want_n; n >= 2; n /= 2) {
            qc = qd = 0;
            ID3D11Device_CheckMultisampleQualityLevels(G.dev, DXGI_FORMAT_B8G8R8A8_UNORM, n, &qc);
            ID3D11Device_CheckMultisampleQualityLevels(G.dev, DXGI_FORMAT_D24_UNORM_S8_UINT, n, &qd);
            if (qc && qd)
                break;
        }
        G.msaa = n >= 2 ? n : 1;
        if (G.msaa != want_n)
            gb_log("[D3D11] GB_D3D11_MSAA=%u: the GPU supports %u\n", want_n, G.msaa);
    }
    {
        IDXGIDevice *dxd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory *fac = NULL;
        DXGI_ADAPTER_DESC adesc;
        RECT cr;

        hr = ID3D11Device_QueryInterface(G.dev, &IID_IDXGIDevice, (void **)&dxd);
        if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxd, &ad);
        if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fac);
        if (FAILED(hr)) { fail("DXGI factory", hr); return 0; }
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &adesc)))
            gb_log("[D3D11] adapter: %ls (vendor 0x%04X, %u MB of its own memory, %u MB shared)\n",
                    adesc.Description, adesc.VendorId, (unsigned)(adesc.DedicatedVideoMemory >> 20),
                    (unsigned)(adesc.SharedSystemMemory >> 20));

        GetClientRect(G.hwnd, &cr);
        memset(&sd, 0, sizeof sd);
        sd.BufferDesc.Width = G.fixed_window ? (UINT)G.win_w : (UINT)(cr.right > 0 ? cr.right : G.win_w);
        sd.BufferDesc.Height = G.fixed_window ? (UINT)G.win_h : (UINT)(cr.bottom > 0 ? cr.bottom : G.win_h);
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = G.hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        hr = E_FAIL;
        if (!G.fixed_window) {
            /* The flip model: the compositor shows our buffer instead of
             * copying it, and in fullscreen the GPU can scan it out directly.
             * Two buffers; DISCARD where available (Windows 10), else
             * SEQUENTIAL (8.1). */
            static const DXGI_SWAP_EFFECT effects[2] = { DXGI_SWAP_EFFECT_FLIP_DISCARD,
                                                         DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL };
            int k;
            sd.BufferCount = 2;
            sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            for (k = 0; k < 2 && FAILED(hr); k++) {
                sd.SwapEffect = effects[k];
                hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)G.dev, &sd, &G.swap);
            }
            G.flip_model = SUCCEEDED(hr);
            if (FAILED(hr))
                fail("flip-model swap chain (using the old one)", hr);
        }
        if (FAILED(hr)) {
            /* The old swap chain: one buffer, copied by the compositor. */
            sd.BufferCount = 1;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)G.dev, &sd, &G.swap);
        }
        if (FAILED(hr)) { fail("CreateSwapChain", hr); return 0; }
        G.back_w = sd.BufferDesc.Width;
        G.back_h = sd.BufferDesc.Height;
        /* Alt+Enter is ours (borderless fullscreen); DXGI's own would switch
         * the display mode (renderer review B7). */
        IDXGIFactory_MakeWindowAssociation(fac, G.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        IDXGIFactory_Release(fac);
        IDXGIAdapter_Release(ad);
        IDXGIDevice_Release(dxd);
    }
    hr = IDXGISwapChain_GetBuffer(G.swap, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (FAILED(hr)) { fail("GetBuffer", hr); return 0; }
    hr = ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)back, NULL, &G.back_rtv);
    ID3D11Texture2D_Release(back);
    if (FAILED(hr)) { fail("back buffer view", hr); return 0; }

    if (!(b = compile("vs", "vs_4_0"))) return 0;
    {
        static const D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "FOG",      0, DXGI_FORMAT_R32_FLOAT,          0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 72, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 88, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                        ID3D10Blob_GetBufferSize(b), NULL, &G.vs);
        hr = ID3D11Device_CreateInputLayout(G.dev, il, 8, ID3D10Blob_GetBufferPointer(b),
                                            ID3D10Blob_GetBufferSize(b), &G.layout);
        ID3D10Blob_Release(b);
        if (FAILED(hr)) { fail("input layout", hr); return 0; }
    }
    if (!(b = compile("ps", "ps_5_0"))) return 0;
    ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &G.ps);
    ID3D10Blob_Release(b);
    if (!(b = compile("vs_present", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &G.vs_present);
    ID3D10Blob_Release(b);
    if (!(b = compile("ps_present", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &G.ps_present);
    ID3D10Blob_Release(b);
    if (!G.vs || !G.ps || !G.vs_present || !G.ps_present) {
        gb_log("[D3D11] could not create the shaders\n");
        return 0;
    }
    if (G.tex_q || G.msaa > 1)
        gb_log("[GB] picture: textures %s, anisotropic %dx; multisampling %ux%s\n",
                G.tex_q ? "with mip levels" : "top level only", G.tex_q > 1 ? G.tex_q : 1,
                G.msaa, G.msaa > 1 ? (G.a2c ? ", alpha-to-coverage on" : ", alpha-to-coverage off") : " (off)");

    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = GB_VB_BYTES;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.vb);
        if (FAILED(hr)) { fail("vertex buffer", hr); return 0; }
        bd.ByteWidth = sizeof(GbConstants);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.cb);
        if (FAILED(hr)) { fail("constant buffer", hr); return 0; }
        bd.ByteWidth = 16;
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.cb_present);
        if (FAILED(hr)) { fail("constant buffer", hr); return 0; }
    }
    {
        /* No culling: the executor hands over both faces and so far neither it
         * nor its own rasteriser culls. No depth clip: the title's z is used
         * as it is and clamped to the buffer's range. */
        D3D11_RASTERIZER_DESC rd;
        D3D11_SAMPLER_DESC sm;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        ID3D11Device_CreateRasterizerState(G.dev, &rd, &G.raster);
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(G.dev, &rd, &G.raster_clip);
        memset(&sm, 0, sizeof sm);
        sm.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(G.dev, &sm, &G.present_sampler);
    }
    G.textures = (GbTexture *)calloc(GB_MAX_TEXTURES, sizeof *G.textures);
    if (!G.textures)
        return 0;
    memset(G.buckets, 0xFF, sizeof G.buckets);
    QueryPerformanceCounter(&G.next);
    compute_view();
    if (G.res_window)
        G.res = res_for_window();
    G.ok = 1;
    gb_log("[D3D11] back end up: %s window %ux%u, %s swap chain, internal resolution %s%.2fx,"
                    " frame limit %s, vsync %s\n",
            G.fixed_window ? "fixed" : "resizable", G.back_w, G.back_h,
            G.flip_model ? "flip-model" : "blit-model", G.res_window ? "follows the window, now " : "",
            G.res, G.period_ns > 0 ? "on" : "off",
            G.vsync_req < 0 ? "auto" : G.vsync_req ? "on" : "off");
    fflush(stderr);
    return 1;
}

/* A start-up that fails hands drawing back to the toolkit's CPU rasteriser,
 * so the game is never left running with nothing on screen. */
static int gb_init(void)
{
    if (G.tried)
        return G.ok;
    if (!gb_init_once()) {
        G.ok = 0;
        nv2a_backend_register(NULL);
        gb_log("[D3D11] start-up failed: back to the CPU rasteriser\n");
        fflush(stderr);
    }
    return G.ok;
}

/* ---- render targets ---------------------------------------------------- */

/* Guest addresses of one piece of memory differ in their top bits (physical,
 * or through the 0x80000000 window). Compare the physical part. */
static uint32_t phys(uint32_t va) { return va & 0x0FFFFFFFu; }

/* A size in the game's pixels to the size drawn at (GB_D3D11_RES). */
static uint32_t real_size(uint32_t n)
{
    double r = (double)n * G.res + 0.5;
    return r < 1.0 ? 1u : r > 16384.0 ? 16384u : (uint32_t)r;
}

/* Widths also widen by the aspect (GB_ASPECT): the game still draws 640
 * wide, and its 3D view is widened to fill the wider picture. */
static uint32_t real_width(uint32_t n)
{
    double r = (double)n * G.res * (G.wide > 0.0 ? G.wide : 1.0) + 0.5;
    return r < 1.0 ? 1u : r > 16384.0 ? 16384u : (uint32_t)r;
}

static void target_release(GbTarget *t)
{
    if (G.bound_target == t) G.bound_target = NULL;
    if (G.present == t) G.present = NULL;
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    if (t->ms) ID3D11Texture2D_Release(t->ms);
    memset(t, 0, sizeof *t);
}

/* ---- multisampling (GB_D3D11_MSAA) ------------------------------------- *
 *
 * Every colour target and depth buffer gets G.msaa samples a pixel. The game
 * samples its scene surface as a texture (Direct3D's own resolve pass, and
 * other uses) and the window shows it, and a multisampled texture can do
 * neither, so each target keeps a single-sample companion (GbTarget.tex and
 * .srv) and is drawn into through GbTarget.ms. The companion is brought up to
 * date only when something reads it: as a texture (texture_view), when shown
 * (gb_flip) and when dumped. A target that is never read (the back buffer the
 * game's resolve pass draws into) is never resolved. */
static void target_resolve(GbTarget *t)
{
    if (!t || !t->ms || !t->dirty)
        return;
    ID3D11DeviceContext_ResolveSubresource(G.ctx, (ID3D11Resource *)t->tex, 0,
                                           (ID3D11Resource *)t->ms, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    t->dirty = 0;
    G.st_resolves++;
}

static GbTarget *target_for(const Nv2aSurface *s)
{
    uint32_t w = s->bytes_per_pixel ? s->pitch / s->bytes_per_pixel : s->width;
    uint32_t h = s->height, va = phys(s->color_va);
    GbTarget *t, *slot = NULL, *oldest = NULL;
    D3D11_TEXTURE2D_DESC td;
    int i;

    if (w < s->width) w = s->width;
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    for (i = 0; i < GB_MAX_TARGETS; i++) {
        t = &G.targets[i];
        if (t->tex && t->va == va && t->w == w && t->h >= h) {
            t->last_frame = G.frame;
            return t;
        }
        if (!t->tex) { if (!slot) slot = t; }
        else if (!oldest || t->last_frame < oldest->last_frame) oldest = t;
    }
    /* The same memory with another size: the old target is stale. */
    for (i = 0; i < GB_MAX_TARGETS; i++)
        if (G.targets[i].tex && G.targets[i].va == va) {
            ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
            target_release(&G.targets[i]);
            slot = &G.targets[i];
            break;
        }
    if (!slot) {
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
        target_release(oldest);
        slot = oldest;
    }
    memset(&td, 0, sizeof td);
    td.Width = real_width(w); td.Height = real_size(h); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &slot->tex)))
        return NULL;
    slot->rw = td.Width;
    slot->rh = td.Height;
    if (G.msaa > 1) {
        td.SampleDesc.Count = G.msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &slot->ms))) {
            target_release(slot);
            return NULL;
        }
    }
    ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)(slot->ms ? slot->ms : slot->tex),
                                        NULL, &slot->rtv);
    ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)slot->tex, NULL, &slot->srv);
    if (!slot->rtv || !slot->srv) { target_release(slot); return NULL; }
    {
        static const float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(G.ctx, slot->rtv, black);
        if (slot->ms) {
            ID3D11RenderTargetView *rv = NULL;      /* the companion starts black too */
            ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)slot->tex, NULL, &rv);
            if (rv) {
                ID3D11DeviceContext_ClearRenderTargetView(G.ctx, rv, black);
                ID3D11RenderTargetView_Release(rv);
            }
        }
    }
    slot->va = va; slot->w = w; slot->h = h; slot->last_frame = G.frame;
    return slot;
}

/* Release every render target and depth buffer, so that the next frame
 * creates them at a new internal resolution. Only at the end of a flip: the
 * game draws each frame from scratch, so nothing visible is lost. */
static void release_targets(void)
{
    int i;
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    for (i = 0; i < GB_MAX_TARGETS; i++)
        if (G.targets[i].tex)
            target_release(&G.targets[i]);
    for (i = 0; i < GB_MAX_DEPTHS; i++)
        if (G.depths[i].tex) {
            ID3D11DepthStencilView_Release(G.depths[i].dsv);
            ID3D11Texture2D_Release(G.depths[i].tex);
            memset(&G.depths[i], 0, sizeof G.depths[i]);
        }
    G.bound_target = NULL;
    G.bound_depth = NULL;
    G.present = NULL;
}

static GbDepth *depth_for(uint32_t zeta_va, uint32_t w, uint32_t h)
{
    GbDepth *d, *slot = NULL, *oldest = NULL;
    D3D11_TEXTURE2D_DESC td;
    uint32_t va = phys(zeta_va);
    int i;

    for (i = 0; i < GB_MAX_DEPTHS; i++) {
        d = &G.depths[i];
        if (d->tex && d->va == va && d->w == w && d->h == h) {
            d->last_frame = G.frame;
            return d;
        }
        if (!d->tex) { if (!slot) slot = d; }
        else if (!oldest || d->last_frame < oldest->last_frame) oldest = d;
    }
    if (!slot) {
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
        G.bound_target = NULL; G.bound_depth = NULL;
        ID3D11DepthStencilView_Release(oldest->dsv);
        ID3D11Texture2D_Release(oldest->tex);
        memset(oldest, 0, sizeof *oldest);
        slot = oldest;
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = G.msaa;           /* as the colour targets: 1 unless GB_D3D11_MSAA */
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &slot->tex)))
        return NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilView(G.dev, (ID3D11Resource *)slot->tex, NULL, &slot->dsv))) {
        ID3D11Texture2D_Release(slot->tex);
        memset(slot, 0, sizeof *slot);
        return NULL;
    }
    ID3D11DeviceContext_ClearDepthStencilView(G.ctx, slot->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    slot->va = va; slot->w = w; slot->h = h; slot->last_frame = G.frame;
    return slot;
}

static void bind_target(GbTarget *t, GbDepth *d)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };

    if (G.bound_target == t && G.bound_depth == d)
        return;
    /* The target may be bound as a texture from an earlier batch. */
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, none);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &t->rtv, d ? d->dsv : NULL);
    /* The whole texture: positions arrive as fractions of the game's size
     * (gb_draw), so a larger texture draws the same picture with more pixels. */
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)t->rw; vp.Height = (float)t->rh;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &vp);
    G.bound_target = t;
    G.bound_depth = d;
}

/* ---- textures ---------------------------------------------------------- */

static const uint8_t *guest_ptr(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    if (end <= 0x04000000u || (va >= 0x80000000u && end <= 0x84000000u))
        return (const uint8_t *)(xbox_GetMemoryOffset() + (ptrdiff_t)va);
    return NULL;
}

/* Bytes of the texture's top level in guest memory, for change detection.
 * Format codes are the NV097 colour formats. */
static uint32_t raw_bytes(const Nv2aTexture *t)
{
    uint32_t bw = (t->width + 3) / 4, bh = (t->height + 3) / 4, n = t->width * t->height;
    switch (t->color) {
    case 0x0C: return bw * bh * 8;                          /* DXT1 */
    case 0x0E: case 0x0F: return bw * bh * 16;              /* DXT3, DXT5 */
    case 0x00: case 0x01: case 0x0B: case 0x19: return n;   /* 8-bit, swizzled */
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: return n * 2;
    case 0x06: case 0x07: return n * 4;
    default:
        if (t->pitch) return t->pitch * t->height;          /* linear formats */
        return n / 2;                                       /* unknown: a safe lower bound */
    }
}

/* ---- texture quality (GB_D3D11_ANISO) ----------------------------------
 *
 * The game's swizzled and compressed textures carry their mip levels after
 * the top one (the format word's level count; most match textures have 4),
 * each half the size, at least one texel, compressed levels in whole 4x4
 * blocks: the toolkit's tex_face_stride layout. With GB_D3D11_ANISO the whole
 * chain is uploaded and sampled with the filter the game sets (trilinear on
 * nearly everything), plus anisotropic filtering above 1. */

/* Bytes of one level of w x h texels of a format whose layout is known here,
 * 0 for the rest (those keep their top level only). */
static uint32_t level_bytes(uint32_t color, uint32_t w, uint32_t h)
{
    switch (color) {
    case 0x0C: return ((w + 3) / 4) * ((h + 3) / 4) * 8;           /* DXT1 */
    case 0x0E: case 0x0F: return ((w + 3) / 4) * ((h + 3) / 4) * 16; /* DXT3, DXT5 */
    case 0x00: case 0x01: case 0x0B: case 0x19: return w * h;       /* 8-bit, swizzled */
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: return w * h * 2;
    case 0x06: case 0x07: return w * h * 4;
    }
    return 0;
}

/* The mip levels this back end may upload for t: 1 unless GB_D3D11_ANISO is
 * on and the format's layout is known; never more than a full chain.
 *
 * The level count normally comes with the texture (the format word). But the
 * toolkit's executor keeps one pitch register per texture stage and never
 * clears it, and it reports one level for any texture whose stage still holds
 * a pitch from an earlier linear texture (the patch: levels = pitch ? 1 :
 * levels). In a match that is every stage-0 texture (pitch 256 or 512 left
 * over), whose format words say 4 levels. For those the count is unknown
 * here: *verify is set and the full chain is offered, to be checked level by
 * level against the memory (verify_levels). A stage with no pitch reports the
 * game's own count, used as it is. */
static uint32_t tex_levels(const Nv2aTexture *t, int *verify)
{
    uint32_t n = t->levels, full = 1, s = t->width > t->height ? t->width : t->height;
    *verify = 0;
    if (!G.tex_q || !level_bytes(t->color, t->width, t->height))
        return 1;
    while (s > 1) { s >>= 1; full++; }
    if (t->pitch) {                     /* swizzled or compressed with a stale pitch: count lost */
        *verify = full > 1;
        return full;
    }
    if (n <= 1)
        return 1;
    return n < full ? n : full;
}

/* Level lv's width, height and byte offset from the top level. */
static void level_at(const Nv2aTexture *t, uint32_t lv, uint32_t *w, uint32_t *h, uint32_t *off)
{
    uint32_t k, o = 0;
    for (k = 0; k < lv; k++) {
        uint32_t lw = (t->width >> k) ? (t->width >> k) : 1, lh = (t->height >> k) ? (t->height >> k) : 1;
        o += level_bytes(t->color, lw, lh);
    }
    *w = (t->width >> lv) ? (t->width >> lv) : 1;
    *h = (t->height >> lv) ? (t->height >> lv) : 1;
    *off = o;
}

/* Bytes of the levels uploaded, for change detection: a rewrite of any level
 * is seen. */
static uint32_t chain_bytes(const Nv2aTexture *t, uint32_t levels)
{
    uint32_t w, h, off;
    if (levels <= 1)
        return raw_bytes(t);
    level_at(t, levels, &w, &h, &off);
    return off;
}

/* One DXT1/3/5 level of w x h texels to 0xAARRGGBB, for verify_levels only
 * (the GPU samples the blocks themselves). */
static uint32_t rgb565(uint32_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static uint32_t mix3(uint32_t a, uint32_t b, int wa, int wb, int d)
{
    uint32_t o = 0;
    int sh;
    for (sh = 0; sh < 24; sh += 8)
        o |= (uint32_t)((((a >> sh) & 255) * wa + ((b >> sh) & 255) * wb) / d) << sh;
    return o;
}

static void bc_decode_level(uint32_t color, const uint8_t *src, uint32_t w, uint32_t h, uint32_t *out)
{
    uint32_t bx, by, bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = color == 0x0C ? 8 : 16;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + (by * bw + bx) * bytes, *cb = b + (bytes - 8);
            uint32_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8, pal[4], alpha[16];
            uint32_t idx = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24, k;
            pal[0] = rgb565(c0) | 0xFF000000u;
            pal[1] = rgb565(c1) | 0xFF000000u;
            if (color != 0x0C || c0 > c1) {
                pal[2] = mix3(pal[0], pal[1], 2, 1, 3) | 0xFF000000u;
                pal[3] = mix3(pal[0], pal[1], 1, 2, 3) | 0xFF000000u;
            } else {
                pal[2] = mix3(pal[0], pal[1], 1, 1, 2) | 0xFF000000u;
                pal[3] = 0;                                     /* DXT1: transparent black */
            }
            for (k = 0; k < 16; k++)
                alpha[k] = pal[(idx >> (k * 2)) & 3] >> 24;
            if (color == 0x0E) {                                /* DXT3: 4 bits a texel */
                for (k = 0; k < 16; k++) {
                    uint32_t a4 = (b[k / 2] >> ((k & 1) * 4)) & 15;
                    alpha[k] = a4 << 4 | a4;
                }
            } else if (color == 0x0F) {                         /* DXT5: two ends, 3-bit indices */
                uint32_t a0 = b[0], a1 = b[1], ap[8], j;
                uint64_t ai = 0;
                for (j = 0; j < 6; j++) ai |= (uint64_t)b[2 + j] << (8 * j);
                ap[0] = a0; ap[1] = a1;
                for (j = 2; j < 8; j++)
                    ap[j] = a0 > a1 ? (a0 * (8 - j) + a1 * (j - 1)) / 7
                          : j < 6 ? (a0 * (6 - j) + a1 * (j - 1)) / 5 : (j == 6 ? 0 : 255);
                for (k = 0; k < 16; k++)
                    alpha[k] = ap[(ai >> (3 * k)) & 7];
            }
            for (k = 0; k < 16; k++) {
                uint32_t x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
                if (x < w && y < h)
                    out[(size_t)y * w + x] = (pal[(idx >> (k * 2)) & 3] & 0x00FFFFFFu) | alpha[k] << 24;
            }
        }
}

/* Mean difference, in 0..255 per channel, between a level (cw x ch) and its
 * parent (pw x ph) halved by averaging 2x2 texels. */
static double mip_diff(const uint32_t *p, uint32_t pw, uint32_t ph, const uint32_t *c, uint32_t cw, uint32_t ch)
{
    uint64_t sum = 0;
    uint32_t x, y;
    int sh;
    for (y = 0; y < ch; y++)
        for (x = 0; x < cw; x++) {
            uint32_t x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : x0;
            uint32_t y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : y0;
            uint32_t a = p[(size_t)y0 * pw + x0], b = p[(size_t)y0 * pw + x1];
            uint32_t d = p[(size_t)y1 * pw + x0], e = p[(size_t)y1 * pw + x1], v = c[(size_t)y * cw + x];
            for (sh = 0; sh < 32; sh += 8) {
                int m = (int)((((a >> sh) & 255) + ((b >> sh) & 255) + ((d >> sh) & 255) + ((e >> sh) & 255) + 2) / 4);
                int n = (int)((v >> sh) & 255);
                sum += (uint64_t)(m > n ? m - n : n - m);
            }
        }
    return (double)sum / ((double)cw * ch * 4.0);
}

/* GB_D3D11_ANISO, a texture whose level count was lost (tex_levels): how
 * many of the `want` levels in memory are the game's own mip levels. Each
 * level is compared with the level above it halved; a real mip level is
 * close to that (box, tent or similar filters; compression), the data of
 * another resource is not. Counted from the top, up to the first level that
 * does not match.
 *
 * Two limits (mean difference per channel, of 255), from a match on
 * 2026-11-16 (209 textures): the first level decides
 * whether there is a chain at all (real chains at most 5.2, one-level
 * textures at least 14.4: limit 9); later levels only where it ends (tiny
 * compressed levels up to 11.0, the end of a chain at least 28.2: limit 18).
 * Every chain found had exactly 4 levels, as the format words in the traced
 * frame say. */
static double mip_match_limit(int first)
{
    static double lim[2] = { -1.0, -1.0 };
    if (lim[0] < 0.0) {
        lim[0] = 9.0;
        lim[1] = 18.0;
    }
    return lim[first ? 0 : 1];
}

static DXGI_FORMAT bc_format(uint32_t color);

static uint32_t *G_vbuf;
static size_t G_vbuf_cap;

/* What verify_levels found, remembered: the texture cache drops textures
 * unused for ten seconds, and a match brings many of them back again and
 * again (2,071 checks for about 209 textures in one lap), each check a
 * decode that cost long frames. An entry is reused only if the levels it
 * found still hold the same bytes. */
#define GB_MIP_MEMO 4096
typedef struct { uint32_t offset, wh, color_want, levels; uint64_t hash; } GbMipMemo;
static GbMipMemo G_memo[GB_MIP_MEMO];

static uint64_t hash_bytes(const uint8_t *p, size_t n);
static uint32_t chain_bytes(const Nv2aTexture *t, uint32_t levels);
static uint32_t verify_levels_now(const Nv2aTexture *t, const uint8_t *raw, uint32_t want);

static uint32_t verify_levels(const Nv2aTexture *t, const uint8_t *raw, uint32_t want)
{
    uint32_t wh = t->width << 16 | t->height, cw = t->color << 8 | want, k, i, slot, n;
    k = (t->offset * 2654435761u) ^ wh ^ (cw * 40503u);
    slot = k & (GB_MIP_MEMO - 1);
    for (i = 0; i < 8; i++) {
        GbMipMemo *m = &G_memo[(k + i) & (GB_MIP_MEMO - 1)];
        if (!m->levels) { slot = (k + i) & (GB_MIP_MEMO - 1); break; }
        if (m->offset == t->offset && m->wh == wh && m->color_want == cw) {
            if (hash_bytes(raw, chain_bytes(t, m->levels)) == m->hash) {
                return m->levels;
            }
            slot = (k + i) & (GB_MIP_MEMO - 1);
            break;
        }
    }
    n = verify_levels_now(t, raw, want);
    G_memo[slot].offset = t->offset;
    G_memo[slot].wh = wh;
    G_memo[slot].color_want = cw;
    G_memo[slot].levels = n;
    G_memo[slot].hash = hash_bytes(raw, chain_bytes(t, n));
    return n;
}

static uint32_t verify_levels_now(const Nv2aTexture *t, const uint8_t *raw, uint32_t want)
{
    DXGI_FORMAT bc = bc_format(t->color);
    size_t need = 0, at = 0, prev_at = 0;
    uint32_t lv, w, h, off, pw = 0, ph = 0, ok = 1;
    double dl[16];
    int ndl = 0;

    for (lv = 0; lv < want; lv++) {
        level_at(t, lv, &w, &h, &off);
        need += (size_t)w * h;
    }
    if (G_vbuf_cap < need) {
        free(G_vbuf);
        G_vbuf = (uint32_t *)malloc(need * 4);
        G_vbuf_cap = G_vbuf ? need : 0;
        if (!G_vbuf)
            return 1;
    }
    for (lv = 0; lv < want; lv++) {
        level_at(t, lv, &w, &h, &off);
        if (bc != DXGI_FORMAT_UNKNOWN) {
            bc_decode_level(t->color, raw + off, w, h, G_vbuf + at);
        } else {
            Nv2aTexture lt = *t;
            lt.offset = t->offset + off;
            lt.width = w;
            lt.height = h;
            lt.levels = 1;
            if (!nv2a_backend_decode_texture(&lt, G_vbuf + at))
                break;
        }
        if (lv > 0) {
            double d = mip_diff(G_vbuf + prev_at, pw, ph, G_vbuf + at, w, h);
            if (ndl < 16) dl[ndl++] = d;
            if (d > mip_match_limit(lv == 1))
                break;
        }
        ok = lv + 1;
        prev_at = at;
        at += (size_t)w * h;
        pw = w;
        ph = h;
    }
    return ok;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, k;
    while (n >= 8) {
        memcpy(&k, p, 8);
        h = (h ^ k) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
        p += 8; n -= 8;
    }
    if (n) {
        k = 0;
        memcpy(&k, p, n);
        h = (h ^ k) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    return h;
}

/* A palettised texture changes when its palette does. */
static uint64_t palette_hash(const Nv2aTexture *t)
{
    const uint8_t *p;
    if (t->color != 0x0B || !t->palette || !(p = guest_ptr(t->palette, 1024)))
        return 0;
    return hash_bytes(p, 1024) * 0x9E3779B97F4A7C15ull;
}

static DXGI_FORMAT bc_format(uint32_t color)
{
    switch (color) {
    case 0x0C: return DXGI_FORMAT_BC1_UNORM;
    case 0x0E: return DXGI_FORMAT_BC2_UNORM;
    case 0x0F: return DXGI_FORMAT_BC3_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

/* Fill or refill a cached texture from guest memory. DXT data goes to the GPU
 * as it is (the same block formats); everything else is decoded to 32 bits,
 * directly for the plain linear formats and through the toolkit's decoder for
 * the rest. Returns 0 if the format cannot be read. */
static int texture_upload(GbTexture *c, const Nv2aTexture *t, const uint8_t *raw)
{
    DXGI_FORMAT bc = bc_format(t->color);
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[16];
    const void *pixels;
    UINT row, levels = c->levels > 1 && c->levels <= 16 ? c->levels : 1, lv;

    if (bc != DXGI_FORMAT_UNKNOWN && (t->width & 3) == 0 && (t->height & 3) == 0) {
        pixels = raw;
        row = (t->width / 4) * (t->color == 0x0C ? 8 : 16);
        for (lv = 1; lv < levels; lv++) {   /* GB_D3D11_ANISO: the levels as they are */
            uint32_t lw, lh, off;
            level_at(t, lv, &lw, &lh, &off);
            init[lv].pSysMem = raw + off;
            init[lv].SysMemPitch = ((lw + 3) / 4) * (t->color == 0x0C ? 8 : 16);
            init[lv].SysMemSlicePitch = 0;
        }
    } else {
        size_t n = (size_t)t->width * t->height, i, need = n, at;
        uint32_t x, y;
        for (lv = 1; lv < levels; lv++) {
            uint32_t lw, lh, off;
            level_at(t, lv, &lw, &lh, &off);
            need += (size_t)lw * lh;
        }
        if (G.decode_cap < need) {
            free(G.decode);
            G.decode = (uint32_t *)malloc(need * 4);
            G.decode_cap = G.decode ? need : 0;
            if (!G.decode)
                return 0;
        }
        /* GB_D3D11_ANISO: each level after the top one decoded on its own, as
         * a texture of that level's address and size (each is swizzled on its
         * own; a palette is shared by all of them). */
        at = n;
        for (lv = 1; lv < levels; lv++) {
            Nv2aTexture lt = *t;
            uint32_t off;
            level_at(t, lv, &lt.width, &lt.height, &off);
            lt.offset = t->offset + off;
            lt.levels = 1;
            if (!nv2a_backend_decode_texture(&lt, G.decode + at))
                return 0;
            init[lv].pSysMem = G.decode + at;
            init[lv].SysMemPitch = lt.width * 4;
            init[lv].SysMemSlicePitch = 0;
            at += (size_t)lt.width * lt.height;
        }
        if ((t->color == 0x12 || t->color == 0x1E) && t->pitch >= t->width * 4) {
            for (y = 0; y < t->height; y++)
                memcpy(G.decode + (size_t)y * t->width, raw + (size_t)y * t->pitch, t->width * 4);
            if (t->color == 0x1E)
                for (i = 0; i < n; i++) G.decode[i] |= 0xFF000000u;
        } else if (t->color == 0x11 && t->pitch >= t->width * 2) {     /* R5G6B5 */
            for (y = 0; y < t->height; y++) {
                const uint16_t *src = (const uint16_t *)(raw + (size_t)y * t->pitch);
                uint32_t *dst = G.decode + (size_t)y * t->width;
                for (x = 0; x < t->width; x++) {
                    uint32_t v = src[x], r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                    dst[x] = 0xFF000000u | ((r << 3 | r >> 2) << 16)
                           | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
                }
            }
        } else if (!nv2a_backend_decode_texture(t, G.decode)) {
            return 0;
        }
        bc = DXGI_FORMAT_B8G8R8A8_UNORM;
        pixels = G.decode;
        row = t->width * 4;
    }
    init[0].pSysMem = pixels; init[0].SysMemPitch = row; init[0].SysMemSlicePitch = 0;
    if (c->tex) {
        for (lv = 0; lv < levels; lv++)
            ID3D11DeviceContext_UpdateSubresource(G.ctx, (ID3D11Resource *)c->tex, lv, NULL,
                                                  init[lv].pSysMem, init[lv].SysMemPitch, 0);
        G.st_tex_upd++;
        return 1;
    }
    memset(&td, 0, sizeof td);
    td.Width = t->width; td.Height = t->height; td.MipLevels = levels; td.ArraySize = 1;
    td.Format = bc;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, init, &c->tex)))
        return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)c->tex, NULL, &c->srv))) {
        ID3D11Texture2D_Release(c->tex);
        c->tex = NULL;
        return 0;
    }
    G.st_tex_new++;
    return 1;
}

static unsigned tex_bucket(const Nv2aTexture *t)
{
    uint32_t k = t->offset * 2654435761u ^ (t->width << 16) ^ t->height ^ (t->color << 8);
    return (k ^ (k >> 15)) & (GB_TEX_BUCKETS - 1);
}

/* Drop textures not used for ten seconds and rebuild the lookup table. */
static void texture_sweep(void)
{
    int i, n = 0;
    for (i = 0; i < G.ntextures; i++) {
        GbTexture *c = &G.textures[i];
        if (G.frame - c->used_frame > 600) {
            if (c->srv) ID3D11ShaderResourceView_Release(c->srv);
            if (c->tex) ID3D11Texture2D_Release(c->tex);
        } else {
            G.textures[n++] = *c;
        }
    }
    G.ntextures = n;
    memset(G.buckets, 0xFF, sizeof G.buckets);
    for (i = 0; i < n; i++) {
        Nv2aTexture k;
        unsigned b;
        k.offset = G.textures[i].offset; k.width = G.textures[i].w;
        k.height = G.textures[i].h; k.color = G.textures[i].color;
        b = tex_bucket(&k);
        G.textures[i].next = G.buckets[b];
        G.buckets[b] = i;
    }
}

/* The view to sample for this batch's texture, or NULL to draw untextured.
 * *levels_out: the view's mip levels (1 for a render target). */
static ID3D11ShaderResourceView *texture_view(const Nv2aTexture *t, const GbTarget *drawing_into,
                                              uint32_t *levels_out)
{
    const uint8_t *raw;
    uint32_t bytes, levels;
    unsigned b;
    GbTexture *c = NULL;
    int i, verify;

    *levels_out = 1;
    G.view_hash0 = 0;
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    /* A surface this back end has drawn into recently, used as a texture:
     * its pixels are on the GPU, not in guest memory. */
    for (i = 0; i < GB_MAX_TARGETS; i++) {
        GbTarget *rt = &G.targets[i];
        if (rt->tex && rt->va == phys(t->offset) && rt->w == t->width
            && rt->h >= t->height && G.frame - rt->last_frame <= 2) {
            if (rt == drawing_into)
                return NULL;
            target_resolve(rt);             /* GB_D3D11_MSAA: bring the companion up to date */
            return rt->srv;
        }
    }
    levels = tex_levels(t, &verify);
    for (;;) {                              /* a chain that runs past guest memory: fewer levels */
        bytes = chain_bytes(t, levels);
        raw = guest_ptr(t->offset, bytes);
        if (raw || levels <= 1)
            break;
        levels--;
    }
    if (!raw)
        return NULL;
    b = tex_bucket(t);
    for (i = G.buckets[b]; i >= 0; i = G.textures[i].next) {
        GbTexture *e = &G.textures[i];
        if (e->offset == t->offset && e->w == t->width && e->h == t->height
            && e->color == t->color && e->pitch == t->pitch && e->palette == t->palette
            && e->want == levels) {
            c = e;
            break;
        }
    }
    if (c) {
        *levels_out = c->levels;
        bytes = chain_bytes(t, c->levels);  /* the levels uploaded: a rewrite of any is seen */
        c->used_frame = G.frame;
        if (c->checked_frame != G.frame) {      /* once a frame: has the title rewritten it? */
            uint64_t h = hash_bytes(raw, bytes) ^ palette_hash(t);
            G.st_hash_bytes += bytes;
            c->checked_frame = G.frame;
            if (h != c->hash) {
                c->hash = h;
                c->hash0 = hash_bytes(raw, chain_bytes(t, 1));
                if (!texture_upload(c, t, raw))
                    return NULL;
            }
        }
        G.view_hash0 = c->hash0;
        return c->srv;
    }
    if (G.ntextures == GB_MAX_TEXTURES) {
        texture_sweep();
        if (G.ntextures == GB_MAX_TEXTURES)
            return NULL;
    }
    c = &G.textures[G.ntextures];
    memset(c, 0, sizeof *c);
    c->offset = t->offset; c->w = t->width; c->h = t->height;
    c->pitch = t->pitch; c->color = t->color; c->palette = t->palette;
    c->want = levels;
    c->levels = verify ? verify_levels(t, raw, levels) : levels;
    bytes = chain_bytes(t, c->levels);
    c->hash = hash_bytes(raw, bytes) ^ palette_hash(t);
    c->hash0 = hash_bytes(raw, chain_bytes(t, 1));
    G.st_hash_bytes += bytes;
    c->checked_frame = c->used_frame = G.frame;
    *levels_out = c->levels;
    if (!texture_upload(c, t, raw)) {
        static uint32_t told[8];
        static int ntold;
        *levels_out = 1;
        for (i = 0; i < ntold && told[i] != t->color; i++)
            ;
        if (i == ntold && ntold < 8) {
            told[ntold++] = t->color;
            gb_log("[D3D11] texture format 0x%02X cannot be read: drawn untextured\n", t->color);
        }
        return NULL;
    }
    c->next = G.buckets[b];
    G.buckets[b] = G.ntextures++;
    G.view_hash0 = c->hash0;
    return c->srv;
}

/* ---- state objects ----------------------------------------------------- */

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

static ID3D11BlendState *blend_state(const Nv2aRenderState *rs)
{
    uint32_t key[6];
    D3D11_BLEND_DESC bd;
    D3D11_RENDER_TARGET_BLEND_DESC *r = &bd.RenderTarget[0];
    int i;

    key[0] = rs->blend_enable != 0;
    key[1] = key[0] ? rs->blend_src : 0;
    key[2] = key[0] ? rs->blend_dst : 0;
    key[3] = key[0] ? rs->blend_eq : 0;
    key[4] = rs->color_mask & 0x01010101u;
    key[5] = 0;
    for (i = 0; i < G.nblends; i++)
        if (!memcmp(G.blends[i].key, key, sizeof key))
            return G.blends[i].obj;
    if (G.nblends == GB_MAX_STATES)
        return G.blends[0].obj;
    memset(&bd, 0, sizeof bd);
    r->BlendEnable = key[0];
    r->SrcBlend = blend_factor(rs->blend_src, 0);
    r->DestBlend = blend_factor(rs->blend_dst, 0);
    r->SrcBlendAlpha = blend_factor(rs->blend_src, 1);
    r->DestBlendAlpha = blend_factor(rs->blend_dst, 1);
    r->BlendOp = r->BlendOpAlpha = blend_op(rs->blend_eq);
    r->RenderTargetWriteMask =
          ((rs->color_mask & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
        | ((rs->color_mask & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
        | ((rs->color_mask & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
        | ((rs->color_mask & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    memcpy(G.blends[G.nblends].key, key, sizeof key);
    ID3D11Device_CreateBlendState(G.dev, &bd, &G.blends[G.nblends].obj);
    return G.blends[G.nblends++].obj;
}

/* GL_NEVER 0x200 .. GL_ALWAYS 0x207 are D3D11's 1 .. 8 in the same order. */
static D3D11_COMPARISON_FUNC compare(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (D3D11_COMPARISON_FUNC)(f - 0x200 + 1)
                                      : D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

static ID3D11DepthStencilState *depth_state(const Nv2aRenderState *rs, int have_depth)
{
    uint32_t key[10];
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;

    memset(key, 0, sizeof key);
    key[0] = have_depth && rs->depth_test_enable;
    key[1] = key[0] ? rs->depth_func : 0;
    key[2] = key[0] && rs->depth_write;
    key[3] = have_depth && rs->stencil_enable;
    if (key[3]) {
        key[4] = rs->stencil_func; key[5] = rs->stencil_func_mask & 0xFF;
        key[6] = rs->stencil_write_mask & 0xFF; key[7] = rs->stencil_fail;
        key[8] = rs->stencil_zfail; key[9] = rs->stencil_zpass;
    }
    for (i = 0; i < G.ndstates; i++)
        if (!memcmp(G.dstates[i].key, key, sizeof key))
            return G.dstates[i].obj;
    if (G.ndstates == GB_MAX_STATES)
        return G.dstates[0].obj;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = key[0];
    dd.DepthFunc = compare(rs->depth_func);
    dd.DepthWriteMask = key[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = key[3];
    dd.StencilReadMask = (UINT8)key[5];
    dd.StencilWriteMask = (UINT8)key[6];
    dd.FrontFace.StencilFunc = compare(rs->stencil_func);
    dd.FrontFace.StencilFailOp = stencil_op(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = stencil_op(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp = stencil_op(rs->stencil_zpass);
    dd.BackFace = dd.FrontFace;
    memcpy(G.dstates[G.ndstates].key, key, sizeof key);
    ID3D11Device_CreateDepthStencilState(G.dev, &dd, &G.dstates[G.ndstates].obj);
    return G.dstates[G.ndstates++].obj;
}

static D3D11_TEXTURE_ADDRESS_MODE address(uint32_t a)
{
    switch (a) {
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_CLAMP;
}

/* levels: the mip levels of the view sampled (texture_view), 1 for the top
 * level only. Above 1 (GB_D3D11_ANISO, a 3D batch) the game's mip mode and
 * level-of-detail bias apply, and anisotropic filtering with G.tex_q > 1. */
static ID3D11SamplerState *sampler_state(const Nv2aTexture *t, uint32_t levels)
{
    /* SET_TEXTURE_FILTER: MIN in bits 16-23 (1 nearest, 2 linear, 3 and 4
     * nearest and linear within the nearest level, 5 and 6 the same blended
     * between two levels, 7 convolution), MAG in bits 24-27 (1 nearest,
     * 2 linear), level-of-detail bias in bits 0-12. */
    uint32_t min = (t->filter >> 16) & 0xFF, mag = (t->filter >> 24) & 0x0F;
    uint32_t key[5];
    D3D11_SAMPLER_DESC sd;
    int i, minlin = !(min == 1 || min == 3 || min == 5), maglin = mag != 1;
    int mip = levels > 1 && min >= 3 && min <= 6 ? (min >= 5 ? 2 : 1) : 0;   /* 1 nearest level, 2 blended */
    int aniso = mip && minlin && G.tex_q > 1 ? G.tex_q : 0;
    int32_t bias = 0;

    if (mip) {
        /* Signed, 8 fraction bits (as other emulators read it). 0 on every
         * texture of the traced match; the first other value is logged. */
        bias = (int32_t)((t->filter & 0x1FFFu) << 19) >> 19;
    }
    key[0] = t->addr_u; key[1] = t->addr_v; key[2] = (uint32_t)(minlin | maglin << 1);
    key[3] = (uint32_t)(mip | aniso << 2);
    key[4] = mip ? (uint32_t)bias : 0;
    for (i = 0; i < G.nsamplers; i++)
        if (!memcmp(G.samplers[i].key, key, sizeof key))
            return G.samplers[i].obj;
    if (G.nsamplers == GB_MAX_STATES)
        return G.samplers[0].obj;
    memset(&sd, 0, sizeof sd);
    sd.Filter = minlin ? (maglin ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT
                                 : D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT)
                       : (maglin ? D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT
                                 : D3D11_FILTER_MIN_MAG_MIP_POINT);
    if (mip == 2)                       /* the MIP_LINEAR twin of each filter above is one higher */
        sd.Filter = (D3D11_FILTER)(sd.Filter + 1);
    if (aniso) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = (UINT)aniso;
    }
    sd.AddressU = address(t->addr_u);
    sd.AddressV = address(t->addr_v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias = bias / 256.0f;
    /* No mip mode: the top level only, also from a view that has more (a 2D
     * batch drawn with a texture a 3D batch uploaded with its levels). */
    sd.MaxLOD = mip ? D3D11_FLOAT32_MAX : 0.0f;
    memcpy(G.samplers[G.nsamplers].key, key, sizeof key);
    ID3D11Device_CreateSamplerState(G.dev, &sd, &G.samplers[G.nsamplers].obj);
    return G.samplers[G.nsamplers++].obj;
}

/* ---- the three hooks --------------------------------------------------- */

static void gb_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    GbTarget *t;

    G.fl_clears++;
    if (!gb_init() || !(t = target_for(s)))
        return;
    if (flags & 0xF0) {
        float c[4];
        c[0] = ((argb >> 16) & 0xFF) / 255.0f;
        c[1] = ((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (argb & 0xFF) / 255.0f;
        c[3] = ((argb >> 24) & 0xFF) / 255.0f;
        ID3D11DeviceContext_ClearRenderTargetView(G.ctx, t->rtv, c);
        t->dirty = 1;
        if (!G.present || G.present->last_frame != G.frame)
            G.present = t;                  /* a frame that only clears still shows */
    }
    if ((flags & 0x3) && rs->zeta_va) {
        GbDepth *d = depth_for(rs->zeta_va, t->rw, t->rh);
        if (d)
            ID3D11DeviceContext_ClearDepthStencilView(G.ctx, d->dsv,
                ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0),
                (float)(zstencil >> 8) / 16777215.0f, (UINT8)(zstencil & 0xFF));
    }
}

/* D3DCOLOR (0xAARRGGBB) to r, g, b, a. */
static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

static int finite3(const Nv2aVertex *v)
{
    /* x - x is 0 for every finite x and NaN for infinities and NaNs. */
    float t = (v->x - v->x) + (v->y - v->y) + (v->z - v->z);
    return t == 0.0f;
}

/* The game's window clip (SET_WINDOW_CLIP) as a scissor rectangle in the
 * target's real pixels. 0: the batch is not limited (no clip sent, the whole
 * surface, or an exclusive clip, which one scissor cannot express). `sq` is how
 * far the batch's x positions are pulled toward the centre (1: not at all): the
 * rectangle moves with them. Scaled as gb_draw scales positions, so it holds at
 * every internal resolution and picture shape. */
static int window_clip(const Nv2aSurface *s, const Nv2aRenderState *rs, const GbTarget *t,
                       float sq, D3D11_RECT *r)
{
    double cx = 0.5 * (double)t->w, fx = (double)t->rw / (double)t->w, fy = (double)t->rh / (double)t->h;

    if (!rs->window_clip_set || rs->window_clip_exclusive)
        return 0;
    if (!rs->window_clip_x0 && !rs->window_clip_y0
        && rs->window_clip_x1 >= s->width && rs->window_clip_y1 >= s->height)
        return 0;
    r->left   = (LONG)((cx + ((double)rs->window_clip_x0 - cx) * sq) * fx + 0.5);
    r->right  = (LONG)((cx + ((double)rs->window_clip_x1 - cx) * sq) * fx + 0.5);
    r->top    = (LONG)((double)rs->window_clip_y0 * fy + 0.5);
    r->bottom = (LONG)((double)rs->window_clip_y1 * fy + 0.5);
    if (r->right < r->left) r->right = r->left;
    if (r->bottom < r->top) r->bottom = r->top;
    return 1;
}

static void gb_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    const Nv2aBatchExt *x = b->ext;
    const Nv2aVertexExt *xv = x ? x->vertices : NULL;
    /* The combiners decide the colour when the title has programmed them and
     * the batch came through a vertex program (so there are stage coordinates,
     * specular and fog to feed them). Otherwise: stage 0 times diffuse. */
    int combine = xv != NULL && x->combiner != NULL;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4];
    D3D11_MAPPED_SUBRESOURCE map;
    GbConstants kb;
    GbTarget *t;
    GbDepth *d = NULL;
    GbVertex *out;
    float sx, sy, su[4], sv[4], bf[4], sq, cx;
    uint32_t i, n = 0, bytes = b->count * (uint32_t)sizeof(GbVertex), flags = 0;
    UINT stride = sizeof(GbVertex), offset;
    int st, flat = -1;                  /* every vertex w = 1 (menus, HUD, text, movies); -1 not checked */
    uint32_t lv;
    uint64_t tex0_hash = 0;             /* level-0 hash of stage 0's texture (anchored_2d) */
    ID3D11PixelShader *ps;
    D3D11_RECT clip_rect;
    int clipped;

    G.fl_calls++;
    if (!gb_init() || b->count < 3 || bytes > GB_VB_BYTES || !(t = target_for(s)))
        return;
    if (rs->zeta_va && (rs->depth_test_enable || rs->stencil_enable))
        d = depth_for(rs->zeta_va, t->rw, t->rh);
    bind_target(t, d);
    if (G.tex_q || G.msaa > 1) {
        /* Mip levels, anisotropic filtering and alpha-to-coverage are for the
         * 3D scene only: a flat batch is drawn exactly as before. */
        flat = 1;
        for (i = 0; i < b->count; i++)
            if (b->vertices[i].rhw != 1.0f) { flat = 0; break; }
    }

    memset(&kb, 0, sizeof kb);
    for (st = 0; st < 4; st++) {
        smp[st] = G.present_sampler;
        su[st] = sv[st] = 1.0f;
    }
    if (combine) {
        const Nv2aCombiner *rc = x->combiner;
        for (st = 0; st < 4; st++) {
            uint32_t mode = (rc->stage_program >> (st * 5)) & 0x1F;
            const Nv2aTexture *tx = &x->stage[st];
            /* Modes 1 to 3 sample the stage's texture (cube maps as flat). */
            if (mode < 1 || mode > 3 || !((x->stage_valid >> st) & 1))
                continue;
            if ((srv[st] = texture_view(tx, t, &lv)) == NULL)
                continue;                   /* unreadable: reads white, as on the CPU path */
            if (st == 0)
                tex0_hash = G.view_hash0;
            smp[st] = sampler_state(tx, flat || G.ab_off ? 1 : lv);
            if (G.tex_q && !flat) {
                if (lv > 1) G.st_tex_mip++; else G.st_tex_one3d++;
            }
            flags |= 16u << st;
            if (!((x->stage_normalized >> st) & 1)) {       /* coordinates in texels */
                su[st] = 1.0f / (float)tx->width;
                sv[st] = 1.0f / (float)tx->height;
            }
        }
        for (st = 0; st < 8; st++) {
            kb.cw[st][0] = rc->color_icw[st]; kb.cw[st][1] = rc->alpha_icw[st];
            kb.cw[st][2] = rc->color_ocw[st]; kb.cw[st][3] = rc->alpha_ocw[st];
            unpack_argb(rc->factor0[st], kb.f0[st]);
            unpack_argb(rc->factor1[st], kb.f1[st]);
        }
        kb.fin[0] = rc->final0; kb.fin[1] = rc->final1;
        kb.fin[2] = rc->control; kb.fin[3] = rc->stage_program;
        unpack_argb(rc->final_c0, kb.fc0);
        unpack_argb(rc->final_c1, kb.fc1);
        kb.fogc[0] = (x->fog_color & 0xFF) / 255.0f;         /* red is in the low byte here */
        kb.fogc[1] = ((x->fog_color >> 8) & 0xFF) / 255.0f;
        kb.fogc[2] = ((x->fog_color >> 16) & 0xFF) / 255.0f;
        kb.misc[3] = x->clip_plane_mode;
        flags |= 1;
    } else if (b->texture && (srv[0] = texture_view(b->texture, t, &lv)) != NULL) {
        tex0_hash = G.view_hash0;
        smp[0] = sampler_state(b->texture, flat || G.ab_off ? 1 : lv);
        su[0] = 1.0f / (float)b->texture->width;
        sv[0] = 1.0f / (float)b->texture->height;
        flags |= 2;
    }
    kb.misc[0] = rs->alpha_test_enable ? (uint32_t)compare(rs->alpha_func) : 0;
    kb.misc[1] = rs->alpha_ref & 0xFF;
    kb.misc[2] = flags;

    /* Surface pixels and 1/w back to clip space: (x, y, z) * w with w itself,
     * so the GPU's divide lands on the same pixel and depth and interpolates
     * colour and texture coordinates with perspective. */
    if (G.vb_used + bytes > GB_VB_BYTES) {
        G.vb_used = 0;
        if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.vb, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &map)))
            return;
    } else if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.vb, 0,
                                              D3D11_MAP_WRITE_NO_OVERWRITE, 0, &map))) {
        return;
    }
    out = (GbVertex *)((uint8_t *)map.pData + G.vb_used);
    sx = 2.0f / (float)t->w;
    sy = 2.0f / (float)t->h;
    /* GB_ASPECT: the target is wider than the game thinks, and its 3D view
     * was widened to fill it. Flat batches (every vertex w = 1: menus, HUD,
     * text, movies; the game's 3D never has that) are squeezed toward the
     * centre so they keep their 4:3 shape. Left wide: a batch spanning the
     * whole width in a frame that also draws 3D (fades, flashes, overlays
     * over the court), which would otherwise leave lit bands at the sides.
     * In a frame without 3D (intro, title, most menus) everything is kept
     * at 4:3, full-screen backgrounds and movies included: black bars. */
    sq = 1.0f;
    /* A 3D batch in this frame (its vertices are not all w = 1): the overlay
     * keeps the corner mark off any screen with a 3D scene. One vertex is
     * enough to tell, and only until the first such batch of the frame. */
    if (b->count && b->vertices[0].rhw != 1.0f) {
        if (!G.panel_only)
            G.has3d_cur = 1;
        G.fr_verts3d += b->count;
    }
    if (G.wide > 1.0 + 1e-6) {
        float lo = 1e30f, hi = -1e30f;
        flat = 1;
        for (i = 0; i < b->count; i++) {
            if (b->vertices[i].rhw != 1.0f) { flat = 0; break; }
            if (b->vertices[i].x < lo) lo = b->vertices[i].x;
            if (b->vertices[i].x > hi) hi = b->vertices[i].x;
        }
        if (!flat) {
            if (!G.panel_only)
                G.has3d_cur = 1;
        } else if (AS.auto2d && !G.has3d_cur && !(G.hist3d & 15u)) {
            /* GB_ASPECT_2D=auto: no 3D batch so far this frame nor in the
             * last four, so no 3D scene on screen: stretched. The HUD is drawn
             * after the court, so a match's first frame is already right; the
             * four frames keep a screen that draws 3D now and then steady,
             * at the price of up to four squeezed frames after a 3D scene. */
            G.as_st++;
        } else if (AS.keep2d) {
            int full = lo <= 0.01f * (float)t->w && hi >= 0.99f * (float)t->w;
            if (full && (G.has3d_cur || G.has3d_prev)) {
                G.as_full++;
            } else if (tex0_hash && anchored_2d(tex0_hash) && (G.has3d_cur || G.has3d_prev)) {
                G.as_anchor++;                  /* placed on the wide view by the game: as it is */
            } else {
                sq = (float)(1.0 / G.wide);
                G.as_sq++;
            }
        }
    }
    /* The clip moves with the batch's own squeeze. A menu panel's 3D needs
     * nothing more: the game derives the clip from the viewport, so a viewport
     * narrowed for 2D kept at 4:3 sends a clip already narrowed the same way. */
    clipped = window_clip(s, rs, t, sq, &clip_rect);
    cx = 0.5f * (float)t->w;
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *v = &b->vertices[i];
        int k;
        if (!finite3(&v[0]) || !finite3(&v[1]) || !finite3(&v[2])) {
            G.st_dropped++;
            continue;
        }
        for (k = 0; k < 3; k++) {
            float w = (v[k].rhw > 0.0f && v[k].rhw - v[k].rhw == 0.0f) ? 1.0f / v[k].rhw : 1.0f;
            float z = v[k].z * (1.0f / 16777215.0f);
            if (w - w != 0.0f) w = 1.0f;
            float px = sq != 1.0f ? (v[k].x - cx) * sq + cx : v[k].x;
            out[n].x = (px * sx - 1.0f) * w;
            out[n].y = (1.0f - v[k].y * sy) * w;
            out[n].z = z * w;
            out[n].w = w;
            out[n].color = v[k].diffuse;
            if (combine) {
                const Nv2aVertexExt *e = &xv[i + (uint32_t)k];
                memcpy(out[n].spec, e->specular, sizeof out[n].spec);
                out[n].fog = e->fog;
                for (st = 0; st < 4; st++) {
                    out[n].tex[st][0] = e->tex[st][0] * su[st];
                    out[n].tex[st][1] = e->tex[st][1] * sv[st];
                    out[n].tex[st][2] = e->tex[st][2];
                    out[n].tex[st][3] = e->tex[st][3];
                }
            } else {
                memset(out[n].spec, 0, sizeof out[n].spec);
                memset(out[n].tex, 0, sizeof out[n].tex);
                out[n].fog = 1.0f;
                out[n].tex[0][0] = v[k].u * su[0];
                out[n].tex[0][1] = v[k].v * sv[0];
                out[n].tex[0][3] = 1.0f;
            }
            n++;
        }
    }
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.vb, 0);
    if (!n)
        return;
    offset = G.vb_used;
    G.vb_used += n * (uint32_t)sizeof(GbVertex);

    if (SUCCEEDED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &kb, sizeof kb);
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.cb, 0);
    }
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;

    ID3D11DeviceContext_IASetInputLayout(G.ctx, G.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(G.ctx, 0, 1, &G.vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.vs, NULL, 0);
    /* GB_D3D11_MSAA: a cut-out 3D batch (alpha test GREATER or GEQUAL)
     * smooths its edges across the samples. In a match every such batch also
     * blends; coverage and blending combine (the samples covered are blended
     * as before), which matters where the test cuts high (reference 192: a
     * hard edge at three quarters opacity). */
    ps = G.ps;
    ID3D11DeviceContext_PSSetShader(G.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.cb);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(G.ctx, 0, 4, smp);
    ID3D11DeviceContext_RSSetState(G.ctx, clipped ? G.raster_clip : G.raster);
    if (clipped)
        ID3D11DeviceContext_RSSetScissorRects(G.ctx, 1, &clip_rect);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, blend_state(rs), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, depth_state(rs, d != NULL),
                                               rs->stencil_ref & 0xFF);
    ID3D11DeviceContext_Draw(G.ctx, n, 0);

    t->dirty = 1;
    G.present = t;
    G.st_draws++;
    G.st_tris += n / 3;
    G.fr_tris += n / 3;
    G.fl_draws++;
    G.fl_tris += n / 3;
    /* A signature of what the frame drew, in order: two frames that draw the
     * same batches with the same textures get the same number. */
    G.fl_sig = (G.fl_sig ^ (b->count + ((uint64_t)(b->texture ? b->texture->offset : 0) << 20)))
               * 0x100000001B3ull;
}


/* Frame drop (recomp_manual.c, "Frame drop"): the game's thread
 * leaves out the heavy draws of a frame when the executor is behind, so that
 * a step rate the PC cannot draw at costs pictures and not game speed. The
 * game's thread records, per swap, whether that frame was dropped; the flip
 * reads the same slot and does not present a dropped frame (it was cleared
 * and never drawn: the last picture stays up). */
static volatile LONG gb_flip_late;          /* thousandths of a period, set by the limiter */
static volatile LONG gb_flip_count;         /* flips so far */
static volatile LONG gb_flip_tris;          /* triangles in the last frame that was not dropped */
static volatile unsigned char gb_drop_ring[64];

int gb_d3d11_flip_late(void)  { return (int)gb_flip_late; }
int gb_d3d11_flip_count(void) { return (int)gb_flip_count; }
int gb_d3d11_flip_tris(void)  { return (int)gb_flip_tris; }
void gb_d3d11_mark_swap(unsigned swap_index, int dropped)
{
    gb_drop_ring[swap_index & 63u] = (unsigned char)(dropped != 0);
}

static void limit_frame_rate(void)
{
    LARGE_INTEGER now;
    LONGLONG period, left;

    if (G.period_ns <= 0)
        return;
    period = (LONGLONG)(G.period_ns * (double)G.qpf.QuadPart / 1e9);
    G.next.QuadPart += period;
    QueryPerformanceCounter(&now);
    /* How far behind its slot this flip is, in thousandths of a frame period,
     * for the frame drop (gb_d3d11_flip_late): 0 when on time. */
    InterlockedExchange(&gb_flip_late, now.QuadPart > G.next.QuadPart
                        ? (LONG)((now.QuadPart - G.next.QuadPart) * 1000 / period) : 0);
    if (now.QuadPart > G.next.QuadPart + 2 * period) {      /* far behind: start again from now */
        G.next = now;
        G.st_late++;
        InterlockedExchange(&gb_flip_late, 0);
        return;
    }
    if (now.QuadPart >= G.next.QuadPart) {
        G.st_late++;                    /* the frame took longer than its slot: no wait */
        return;
    }
    /* Sleep to within a millisecond of the deadline, then spin. The sleep is a
     * high-resolution waitable timer, not Sleep(): Sleep is only as fine as
     * the system timer tick, and Windows 11 drops a process back to the
     * 15.6 ms tick while its window is covered or minimised, which turns a
     * 60 Hz limit into something between 32 and 64. */
    left = G.next.QuadPart - now.QuadPart;
    if (G.timer && left * 1000 / G.qpf.QuadPart >= 2) {
        LARGE_INTEGER due;
        due.QuadPart = -((left * 10000000 / G.qpf.QuadPart) - 10000);  /* 100 ns units, relative, 1 ms early */
        if (SetWaitableTimer(G.timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(G.timer, 100);
    }
    for (;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= G.next.QuadPart)
            break;
        YieldProcessor();
    }
    {
        double over = (double)(now.QuadPart - G.next.QuadPart) * 1000.0 / (double)G.qpf.QuadPart;
        G.st_over_ms += over;
        if (over > G.st_over_max)
            G.st_over_max = over;
    }
}


/* ---- presenting -------------------------------------------------------- */

/* The window thread reported a new client size: resize the swap chain's
 * buffers to it. A minimised window keeps its buffers (its size is 0). */
static void sync_window(void)
{
    LONG gen = G.size_gen;
    ID3D11Texture2D *back = NULL;
    UINT w, h;
    HRESULT hr;

    if (G.fixed_window || gen == G.seen_size_gen)
        return;
    G.seen_size_gen = gen;
    w = (UINT)G.cl_w;
    h = (UINT)G.cl_h;
    if (G.minimized || !w || !h || (w == G.back_w && h == G.back_h && G.back_rtv))
        return;
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    if (G.back_rtv) {
        ID3D11RenderTargetView_Release(G.back_rtv);
        G.back_rtv = NULL;
    }
    ID3D11DeviceContext_Flush(G.ctx);   /* drops the context's deferred references to the buffers */
    hr = IDXGISwapChain_ResizeBuffers(G.swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
        fail("ResizeBuffers (keeping the old size)", hr);
    else {
        G.back_w = w;
        G.back_h = h;
    }
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(G.swap, 0, &IID_ID3D11Texture2D, (void **)&back))) {
        if (FAILED(ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)back, NULL, &G.back_rtv)))
            G.back_rtv = NULL;          /* nothing is shown until the next resize */
        ID3D11Texture2D_Release(back);
    }
    compute_view();
}

/* Which monitor the window is on, its refresh rate, and so how to present.
 * Checked when the window has moved or the display settings changed. */
static void read_display(void)
{
    LONG gen = G.mon_gen;
    MONITORINFOEXA mi;
    DEVMODEA dm;
    HMONITOR m;
    int hz = 0;

    if (gen == G.seen_mon_gen && G.mon)
        return;
    G.seen_mon_gen = gen;
    m = MonitorFromWindow(G.hwnd, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    {
        /* This thread is not DPI aware, so Windows would report a 4K monitor
         * at 150% as 2560x1440. Read it as the window thread sees it. */
        typedef void *(WINAPI *SetCtxFn)(void *);
        SetCtxFn set_ctx = (SetCtxFn)(void *)GetProcAddress(GetModuleHandleA("user32.dll"),
                                                             "SetThreadDpiAwarenessContext");
        void *old = set_ctx ? set_ctx((void *)(intptr_t)-4) : NULL;
        if (GetMonitorInfoA(m, (MONITORINFO *)&mi) && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hz = (int)dm.dmDisplayFrequency;
        if (set_ctx && old)
            set_ctx(old);
    }
    if (m == G.mon && hz == G.mon_hz)
        return;
    G.mon = m;
    G.mon_hz = hz;
    /* The frame limit keeps game time at 60 frames a second whatever happens
     * here. A sync interval only decides how each frame meets the display: it
     * holds every frame for n refreshes, which is even only when the rate is a
     * whole multiple of 60. "auto" uses it then and nowhere else. */
    if (G.vsync_req >= 0)
        G.sync = (UINT)G.vsync_req;
    else
        G.sync = (hz >= 60 && hz <= 240 && hz % 60 == 0) ? (UINT)(hz / 60) : 0;
    G.st_hz_qpc0 = 0;
    gb_log("[D3D11] on monitor %s, %ldx%ld at %d Hz: present with sync interval %u\n",
            mi.szDevice, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            hz, G.sync);
}



/* The game's picture into the window's buffer, then the port's overlay over
 * it (gb_overlay.c). Shared by a game frame (gb_flip) and the start screen's
 * own frames while the game is held (gb_d3d11_reflip). */
static void show_picture(void)
{
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };
    D3D11_VIEWPORT vp;
    target_resolve(G.present);          /* GB_D3D11_MSAA: the samples into the picture shown */
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &G.back_rtv, NULL);
    /* Black first: the bars, and a flip-model buffer starts undefined. */
    ID3D11DeviceContext_ClearRenderTargetView(G.ctx, G.back_rtv, black);
    vp.TopLeftX = G.view_x; vp.TopLeftY = G.view_y;
    vp.Width = G.view_w; vp.Height = G.view_h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &vp);
    if (G.present && G.present->srv) {
        /* How many samples a window pixel averages: 1 unless the picture is
         * larger than its rectangle (ps_present). */
        D3D11_MAPPED_SUBRESOURCE map;
        float pp[4];
        double rx = (double)G.present->rw / G.view_w, ry = (double)G.present->rh / G.view_h;
        pp[0] = 1.0f / G.view_w;
        pp[1] = 1.0f / G.view_h;
        pp[2] = rx > 1.001 ? (float)(rx > 8.0 ? 8 : (int)ceil(rx)) : 1.0f;
        pp[3] = ry > 1.001 ? (float)(ry > 8.0 ? 8 : (int)ceil(ry)) : 1.0f;
        if (SUCCEEDED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.cb_present, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, pp, sizeof pp);
            ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.cb_present, 0);
        }
        ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 1, 1, &G.cb_present);
        ID3D11DeviceContext_IASetInputLayout(G.ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(G.ctx, G.vs_present, NULL, 0);
        ID3D11DeviceContext_PSSetShader(G.ctx, G.ps_present, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 1, &G.present->srv);
        ID3D11DeviceContext_PSSetSamplers(G.ctx, 0, 1, &G.present_sampler);
        ID3D11DeviceContext_RSSetState(G.ctx, G.raster);
        ID3D11DeviceContext_OMSetBlendState(G.ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, NULL, 0);
        ID3D11DeviceContext_Draw(G.ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, none);
    }
    gb_overlay_frame(G.ctx, G.back_rtv, (IDXGISwapChain *)G.swap, (int)G.back_w, (int)G.back_h,
                     G.view_x, G.view_y, G.view_w, G.view_h,
                     G.fr_verts3d);
    G.bound_target = NULL;              /* the overlay moved the render target */
    G.bound_depth = NULL;
}

static void gb_flip(void)
{
    ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };
    int dropped_frame = 0;

    if (!gb_init())
        return;
    sync_window();
    read_display();
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, none);
    G.bound_target = NULL;
    G.bound_depth = NULL;
    {
        /* A frame the game's thread dropped (see gb_drop_ring): keep the last
         * picture. The count is of flips since the back end came up, and the
         * game's thread counts swaps the same way. */
        LONG n = InterlockedIncrement(&gb_flip_count) - 1;
        dropped_frame = gb_drop_ring[(unsigned)n & 63u];
        gb_drop_ring[(unsigned)n & 63u] = 0;
        if (dropped_frame)
            G.st_frames_dropped++;
        else
            InterlockedExchange(&gb_flip_tris, (LONG)G.fr_tris);
        G.fr_tris = 0;
    }
    if (!G.back_rtv || G.minimized || dropped_frame)
        goto presented;                 /* nothing to show it in: the game runs on */
    show_picture();
    IDXGISwapChain_Present(G.swap, G.sync, 0);
presented:
    G.as_3d += G.has3d_cur ? 1u : 0u;
    {
        /* GB_ASPECT_2D=auto: count the frames where the 2D state changed */
        int was = (G.hist3d & 15u) != 0, now = ((G.hist3d << 1 | (uint32_t)G.has3d_cur) & 15u) != 0;
        if (was != now) {
            G.as_flips++;
        }
    }
    G.hist3d = G.hist3d << 1 | (uint32_t)(G.has3d_cur != 0);
    {
        /* Which of the game's 3D views were set up in the last 15 frames
         * (the game runs a frame or two ahead of this point). */
        long sc = views_scene, pa = views_panel;
        if (sc != G.views_scene) { G.views_scene = sc; G.scene_frame = G.frame; G.scene_seen = 1; }
        if (pa != G.views_panel) { G.views_panel = pa; G.panel_frame = G.frame; G.panel_seen = 1; }
        G.panel_only = G.panel_seen && G.frame - G.panel_frame < 15u
                       && !(G.scene_seen && G.frame - G.scene_frame < 15u);
    }
    G.has3d_prev = G.has3d_cur;
    G.has3d_cur = 0;
    G.fr_verts3d = 0;
    if (AS.window && !G.minimized && G.back_rtv) {
        /* GB_ASPECT=window: follow the window's shape once it has held still
         * for a quarter of a second. */
        double want = window_aspect() * 3.0 / 4.0, now = now_ms();
        if (fabs(want - G.wide) > 0.005) {
            if (fabs(want - G.wide_want) > 0.005) {
                G.wide_want = want;
                G.wide_want_ms = now;
            } else if (now - G.wide_want_ms > 250.0) {
                release_targets();
                G.wide = want;
                compute_view();
            }
        }
    }
    if (G.res_window && !G.minimized && G.back_rtv) {
        /* The window's size changed the scale: switch once it has held still
         * for a quarter of a second, not on every step of a drag. */
        double want = res_for_window(), now = now_ms();
        if (fabs(want - G.res) > 0.005) {
            if (fabs(want - G.res_want) > 0.005) {
                G.res_want = want;
                G.res_want_ms = now;
            } else if (now - G.res_want_ms > 250.0) {
                release_targets();
                G.res = want;
            }
        }
    }

    G.frame++;
    if ((G.frame & 255) == 0)
        texture_sweep();
    limit_frame_rate();
}


/* ---- graphics chip ------------------------------------------------------
 *
 * A laptop with two graphics chips gives a program the integrated one unless
 * the program asks otherwise. These two exported values are the standard way
 * to ask: NVIDIA's driver (Optimus) and AMD's (PowerXpress) look for them in
 * the .exe when the process starts and then run it on the dedicated chip. They
 * must be exported from the program itself, not from a DLL (check with an
 * export listing, for example llvm-objdump -p on the .exe). Always
 * on; GB_D3D11_ADAPTER (gb_init_once) picks another
 * adapter for testing. Windows' own per-program graphics setting still wins
 * over both. */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* One lock around everything that uses the device context: the executor's
 * three calls, and the start screen's own frames presented from the game's
 * thread while the game is held at its first movie (gb_d3d11_reflip). */
static CRITICAL_SECTION gb_lock;



static void gb_clear_locked(const Nv2aSurface *s, const Nv2aRenderState *rs,
                            uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    EnterCriticalSection(&gb_lock);
    gb_clear(s, rs, flags, argb, zstencil);
    LeaveCriticalSection(&gb_lock);
}

static void gb_draw_locked(const Nv2aSurface *s, const Nv2aBatch *b)
{
    EnterCriticalSection(&gb_lock);
    gb_draw(s, b);
    LeaveCriticalSection(&gb_lock);
}

static void gb_flip_locked(void)
{
    EnterCriticalSection(&gb_lock);
    gb_flip();
    LeaveCriticalSection(&gb_lock);
}

int gb_d3d11_reflip(void)
{
    int shown = 0;
    EnterCriticalSection(&gb_lock);
    if (G.ok && G.back_rtv && !G.minimized && G.swap) {
        sync_window();
        if (G.back_rtv) {
            show_picture();
            IDXGISwapChain_Present(G.swap, G.sync, 0);
            shown = 1;
        }
    }
    LeaveCriticalSection(&gb_lock);
    return shown;
}

int gb_d3d11_install(void)
{
    static const Nv2aBackend backend = { gb_clear_locked, gb_draw_locked, gb_flip_locked };
    InitializeCriticalSectionAndSpinCount(&gb_lock, 4000);
    nv2a_backend_register(&backend);
    return 1;
}
