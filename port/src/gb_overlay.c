/*
 * The port's own picture over the game's, drawn by the Direct3D 11 back end
 * into the window's buffer after the game's picture and before it is shown:
 *
 *   - the start screen: the project's mark, its name, the version and one
 *     line saying the project is not affiliated with or endorsed by the
 *     game's publisher, developer or league; 3.4 s from the first picture,
 *     always (no switch: the project is identified wherever it is played);
 *   - the corner mark: the mark and the name at the top-right of the title
 *     screen, the main menu and the screens that share its background, and
 *     the boot loading screen; never over a 3D scene (a match, its pause
 *     menu, replays), always (no switch);
 *   - the frame counter: pictures shown per second, top-left, GB_SHOW_FPS=1
 *     (a player setting, off by default);
 *   - F12: the shown picture saved as a PNG in the screenshots folder, with
 *     a short notice on screen.
 *
 * Everything is drawn in the window's own pixels, after the game's picture is
 * scaled to the window: sharp at every internal resolution and window size,
 * and the game's memory, textures and draw calls are never touched. Sizes are
 * fractions of the window's or the picture's height, with a floor in pixels
 * so a small window stays legible. Timing is by the clock, so fades look the
 * same at 60 and 120 pictures a second.
 *
 * The mark is computed per pixel (a rounded square cut by a diagonal gap,
 * violet to teal); text comes from fonts that ship with Windows, drawn with
 * GDI into a small texture when the window's size changes. No font, image or
 * other file is shipped for any of it.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gb_log.h"
#include "gb_overlay.h"
#include "gb_save.h"

#ifndef GB_VERSION_STR
#define GB_VERSION_STR "0.1.0"
#endif

#define START_TOTAL_MS 3400.0           /* the start screen, fade of its background included */
#define OV_SCENE_VERTS 40000u           /* 3D vertices in a frame above which it is a court, not a backdrop */

/* Windows' image encoder, named here so no extra import library is needed. */
static const GUID OV_CLSID_WICImagingFactory1 = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID OV_IID_IWICImagingFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID OV_GUID_ContainerFormatPng = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID OV_GUID_WICPixelFormat24bppBGR = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };

/* ── shaders ──────────────────────────────────────────────────────────────
 * One quad per element; the pixel shader works in window pixels
 * (SV_Position). Modes: 0 rounded plate, 1 soft glow, 2 the mark, 3 the
 * seam of light, 4 text from the atlas. */
static const char OV_HLSL[] =
"cbuffer O : register(b0) { float4 rect; float4 vp; float4 col; float4 a; float4 b; uint4 mode; };\n"
"Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
"struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vs_ov(uint id : SV_VertexID) {\n"
"    VO o; float2 k = float2(id & 1, id >> 1);\n"
"    float2 p = lerp(rect.xy, rect.zw, k);\n"
"    o.pos = float4(p.x / vp.x * 2 - 1, 1 - p.y / vp.y * 2, 0, 1); o.uv = k; return o; }\n"
"static const float3 VIO = float3(159, 144, 255) / 255.0;\n"
"static const float3 TEAL = float3(18, 194, 176) / 255.0;\n"
"static const float3 MID = float3(110, 160, 235) / 255.0;\n"
"static const float3 DARK = float3(11, 12, 20) / 255.0;\n"
"float4 ps_ov(VO i) : SV_Target {\n"
"    float2 p = i.pos.xy; uint m = mode.x;\n"
"    if (m == 0u) {\n"                                           /* a = centre, half size; b.x = radius */
"        float2 q = abs(p - a.xy) - a.zw + b.x;\n"
"        float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - b.x;\n"
"        return float4(col.rgb, col.a * saturate(0.5 - d)); }\n"
"    if (m == 1u) {\n"                                           /* a = centre, radii: fades to nothing at the edge */
"        float r = length((p - a.xy) / a.zw);\n"
"        return float4(col.rgb, col.a * saturate(1 - r)); }\n"
"    if (m == 2u) {\n"                                           /* a = corner, size, gap open 0..1; b = alpha, form */
"        float s = a.z, px = 1.0 / s; float2 uv = (p - a.xy) / s;\n"
"        float r = 7.5 / 31.0; float2 q = abs(uv - 0.5) - (0.5 - r);\n"
"        float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;\n"
"        float inside = saturate(0.5 - d / px);\n"
"        float ea = 1 - 0.2 * a.w, eb = 1 + 0.2 * a.w, t = uv.x + uv.y;\n"
"        float pa = saturate(0.5 - (t - ea) * 0.70710678 / px);\n"
"        float pb = saturate(0.5 + (t - eb) * 0.70710678 / px);\n"
"        float piece = saturate(pa + pb);\n"
"        float3 c = lerp(VIO, TEAL, saturate((t * 0.5 + 1.0 / 18.0) / (20.0 / 18.0)));\n"
"        float ap = inside * piece, ag = b.y < 0.5 ? inside * (1 - piece) : 0;\n"
"        float al = ap + ag; if (al <= 0.0) discard;\n"
"        return float4((c * ap + DARK * ag) / al, al * b.x); }\n"
"    if (m == 3u) {\n"                                           /* a = centre, half-length, height; b = alpha, ends */
"        float2 d = p - a.xy;\n"
"        float along = dot(d, float2(0.70710678, -0.70710678));\n"
"        float perp = abs(dot(d, float2(0.70710678, 0.70710678)));\n"
"        float tf = abs(along) / a.z; if (tf >= 1.0) discard;\n"
"        float ends = lerp(b.y, 1.0, saturate((1 - tf) / 0.22));\n"
"        float w = max(1.2, 0.0022 * a.w);\n"
"        float core = saturate(w * 0.5 + 0.5 - perp);\n"
"        float glow = 0.163 * saturate(1 - perp / (0.034 * a.w));\n"
"        float g = saturate(along / a.z * 0.5 + 0.5);\n"
"        float3 c = lerp(lerp(TEAL, MID, saturate(g * 2)), VIO, saturate(g * 2 - 1));\n"
"        return float4(c, saturate(glow + core * 0.65 * (1 - glow)) * ends * b.x); }\n"
"    float2 uv = lerp(b.xy, b.zw, i.uv);\n"                       /* text: b = atlas rectangle */
"    return float4(col.rgb, col.a * tx.Sample(sm, uv).a);\n"
"}\n";

typedef struct { float rect[4], vp[4], col[4], a[4], b[4]; uint32_t mode[4]; } OvConst;

/* ── text ─────────────────────────────────────────────────────────────── */
enum { T_NAME_BIG, T_VER, T_LEGAL, T_NAME_CORNER, T_SAVED, T_NOTSAVED, T_DUMPON, T_DUMPOFF, T_D0, T_COUNT = T_D0 + 10 };
typedef struct { int x, y, w, h, ascent, adv; } OvText;
typedef struct { const wchar_t *s; double em; int min, weight; const wchar_t *const *face; int h_of_view; } OvSpec;

static OvSpec ov_spec(const wchar_t *s, double em, int min, int weight, const wchar_t *const *face, int h_of_view)
{
    OvSpec r;
    r.s = s; r.em = em; r.min = min; r.weight = weight; r.face = face; r.h_of_view = h_of_view;
    return r;
}

static struct {
    int tried, ok;
    ID3D11Device *dev;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *cb;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *raster;
    ID3D11SamplerState *sampler;
    ID3D11Texture2D *atlas;
    ID3D11ShaderResourceView *atlas_srv;
    int atlas_w, atlas_h, atlas_key_win, atlas_key_view;
    OvText text[T_COUNT];
    float vw, vh;                       /* the window buffer, for the vertex shader */
    ID3D11DeviceContext *ctx;
    LARGE_INTEGER freq;
    /* screen state */
    int corner_branded;
    double corner_alpha, last_ms, last3d_ms, any3d_ms;
    int in_play;
    LONGLONG play_tick;
    int show_fps;
    double fps_times[256];
    int fps_n, fps_head;
    double fps_shown_at;
    int fps_value;
    /* screenshot */
    ID3D11Texture2D *staging;
    int staging_w, staging_h, staging_rgba, staging_wait;
    double notice_at;
    int notice_ok;
    int notice_id;                              /* the text shown: T_SAVED, T_NOTSAVED, T_DUMPON, T_DUMPOFF */
} OV;

static volatile LONGLONG ov_start_qpc;          /* first picture, in QueryPerformanceCounter ticks; 0 before */
static volatile LONG ov_movie_class;            /* 1 the title and main-menu group, 2 another movie */
static volatile LONGLONG ov_movie_tick;         /* GetTickCount64 at the last report */
static volatile LONG ov_any_movie;
static volatile LONG ov_shot_request;
static volatile LONG ov_shot_busy;              /* a worker is writing a file */
static volatile LONG ov_shot_result;            /* 1 saved, 2 not saved; taken by the next frame */
static volatile LONG ov_dump_note;              /* 1 texture dump on, 2 off; taken by the next frame */

static double ov_now_ms(void)
{
    LARGE_INTEGER c;
    if (!OV.freq.QuadPart) QueryPerformanceFrequency(&OV.freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)OV.freq.QuadPart;
}

/* ── the screens ──────────────────────────────────────────────────────── */
void gb_overlay_note_movie(const char *base)
{
    /* The title screen's background (xmt0015), the move from the title to
     * the main menu (xmt0000) and the main menu's background loop (xlp0005),
     * which stays behind Options, High Scores and the other screens of that
     * menu. Deeper menus have other loops (xlp0004 ...). */
    static const char *const group[] = { "xmt0015.nfx", "xmt0000.nfx", "xlp0005.nfx" };
    LONG cls = 2;
    size_t i;
    for (i = 0; i < sizeof group / sizeof group[0]; i++)
        if (_stricmp(base, group[i]) == 0)
            cls = 1;
    InterlockedExchange(&ov_movie_class, cls);
    InterlockedExchange64(&ov_movie_tick, (LONGLONG)GetTickCount64());
    InterlockedExchange(&ov_any_movie, 1);
}

unsigned gb_overlay_start_left_ms(void)
{
    LARGE_INTEGER c, f;
    LONGLONG s = ov_start_qpc;
    double el;
    if (!s)
        return 0;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    el = (double)(c.QuadPart - s) * 1000.0 / (double)f.QuadPart;
    return el >= START_TOTAL_MS ? 0u : (unsigned)(START_TOTAL_MS - el + 0.5);
}

void gb_overlay_request_shot(void)
{
    InterlockedExchange(&ov_shot_request, 1);
}

/* ── set-up ───────────────────────────────────────────────────────────── */
static ID3DBlob *ov_compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(OV_HLSL, sizeof OV_HLSL - 1, "gb_overlay", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        gb_log("[D3D11] overlay shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int ov_init(ID3D11DeviceContext *ctx)
{
    ID3DBlob *b;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_RASTERIZER_DESC rd;
    D3D11_SAMPLER_DESC sd;
    const char *e;
    if (OV.tried)
        return OV.ok;
    OV.tried = 1;
    ID3D11DeviceContext_GetDevice(ctx, &OV.dev);
    if (!OV.dev) return 0;
    if (!(b = ov_compile("vs_ov", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(OV.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &OV.vs);
    ID3D10Blob_Release(b);
    if (!(b = ov_compile("ps_ov", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(OV.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &OV.ps);
    ID3D10Blob_Release(b);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(OvConst);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(OV.dev, &bd, NULL, &OV.cb);
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(OV.dev, &bl, &OV.blend);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(OV.dev, &rd, &OV.raster);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(OV.dev, &sd, &OV.sampler);
    if (!OV.vs || !OV.ps || !OV.cb || !OV.blend || !OV.raster || !OV.sampler)
        return 0;
    e = getenv("GB_SHOW_FPS");
    OV.show_fps = e && e[0] && e[0] != '0';
    OV.ok = 1;
    return 1;
}

void gb_overlay_release(void)
{
    if (OV.atlas_srv) ID3D11ShaderResourceView_Release(OV.atlas_srv);
    if (OV.atlas) ID3D11Texture2D_Release(OV.atlas);
    if (OV.staging) ID3D11Texture2D_Release(OV.staging);
    OV.atlas_srv = NULL; OV.atlas = NULL; OV.staging = NULL;
    OV.atlas_key_win = OV.atlas_key_view = 0;
}

/* A font from those that ship with Windows; the first face that exists. */
static HFONT ov_font(HDC dc, int em, int weight, const wchar_t *const *faces)
{
    int i;
    for (i = 0; faces[i]; i++) {
        wchar_t got[LF_FACESIZE];
        HFONT f = CreateFontW(-em, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, faces[i]);
        HGDIOBJ old;
        if (!f) continue;
        old = SelectObject(dc, f);
        got[0] = 0;
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        if (_wcsicmp(got, faces[i]) == 0 || !faces[i + 1])
            return f;
        DeleteObject(f);
    }
    return NULL;
}

/* The strings, drawn white on black with GDI's greyscale smoothing, kept as
 * the alpha of a white texture. Built again when the window's height or the
 * picture's height changes. */
static void ov_build_text(int win_h, int view_h)
{
    static const wchar_t *const display[] = { L"Segoe UI Variable Display Semibold", L"Segoe UI Variable Display", L"Segoe UI Semibold", L"Segoe UI", NULL };
    static const wchar_t *const textf[] = { L"Segoe UI Variable Text", L"Segoe UI", NULL };
    static const wchar_t *const mono[] = { L"Cascadia Mono", L"Consolas", NULL };
    OvSpec spec[T_COUNT];
    wchar_t ver[64], digit[T_COUNT][2];
    HDC dc;
    HFONT fonts[T_COUNT];
    int i, w = 0, h = 0, y = 0;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    uint8_t *px;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;

    if (OV.atlas && OV.atlas_key_win == win_h && OV.atlas_key_view == view_h)
        return;
    if (OV.atlas_srv) { ID3D11ShaderResourceView_Release(OV.atlas_srv); OV.atlas_srv = NULL; }
    if (OV.atlas) { ID3D11Texture2D_Release(OV.atlas); OV.atlas = NULL; }
    OV.atlas_key_win = win_h;
    OV.atlas_key_view = view_h;

    {
        char a[64];
        snprintf(a, sizeof a, "Version %s", GB_VERSION_STR);
        MultiByteToWideChar(CP_UTF8, 0, a, -1, ver, 64);
    }
    memset(spec, 0, sizeof spec);
    spec[T_NAME_BIG]    = ov_spec(L"Gamebreaker", 0.095, 12, 600, display, 0);
    spec[T_VER]         = ov_spec(ver, 0.027, 11, 400, textf, 0);
    spec[T_LEGAL]       = ov_spec(L"Not affiliated with or endorsed by the game's publisher, developer or league", 0.022, 10, 400, textf, 0);
    spec[T_NAME_CORNER] = ov_spec(L"Gamebreaker", 0.042, 14, 600, display, 1);
    spec[T_SAVED]       = ov_spec(L"Screenshot saved", 0.026, 13, 400, textf, 0);
    spec[T_NOTSAVED]    = ov_spec(L"Screenshot not saved", 0.026, 13, 400, textf, 0);
    spec[T_DUMPON]      = ov_spec(L"Texture dump on", 0.026, 13, 400, textf, 0);
    spec[T_DUMPOFF]     = ov_spec(L"Texture dump off", 0.026, 13, 400, textf, 0);
    for (i = 0; i < 10; i++) {
        digit[i][0] = (wchar_t)(L'0' + i); digit[i][1] = 0;
        spec[T_D0 + i] = ov_spec(digit[i], 0.030, 13, 600, mono, 0);
    }

    dc = CreateCompatibleDC(NULL);
    for (i = 0; i < T_COUNT; i++) {
        int em = (int)floor((spec[i].h_of_view ? view_h : win_h) * spec[i].em + 0.5);
        SIZE sz;
        TEXTMETRICW tm;
        HGDIOBJ old;
        if (em < spec[i].min) em = spec[i].min;
        fonts[i] = ov_font(dc, em, spec[i].weight, spec[i].face);
        old = SelectObject(dc, fonts[i]);
        GetTextExtentPoint32W(dc, spec[i].s, (int)wcslen(spec[i].s), &sz);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        OV.text[i].x = 0;
        OV.text[i].y = h;
        OV.text[i].w = sz.cx + 4 + em / 8;          /* room for overhang */
        OV.text[i].h = sz.cy + 4;
        OV.text[i].ascent = tm.tmAscent + 2;
        OV.text[i].adv = sz.cx;
        if (OV.text[i].w > w) w = OV.text[i].w;
        h += OV.text[i].h + 2;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                     /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ oldb = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)w * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (i = 0; i < T_COUNT; i++) {
            HGDIOBJ old = SelectObject(dc, fonts[i]);
            TextOutW(dc, 2, OV.text[i].y + 2, spec[i].s, (int)wcslen(spec[i].s));
            SelectObject(dc, old);
        }
        GdiFlush();
        px = (uint8_t *)bits;
        for (y = 0; y < w * h; y++) {               /* coverage into alpha, white colour */
            uint8_t c = px[y * 4 + 1];
            px[y * 4 + 0] = px[y * 4 + 1] = px[y * 4 + 2] = 255;
            px[y * 4 + 3] = c;
        }
        memset(&td, 0, sizeof td);
        td.Width = (UINT)w; td.Height = (UINT)h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = bits; init.SysMemPitch = (UINT)w * 4; init.SysMemSlicePitch = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(OV.dev, &td, &init, &OV.atlas)))
            ID3D11Device_CreateShaderResourceView(OV.dev, (ID3D11Resource *)OV.atlas, NULL, &OV.atlas_srv);
        OV.atlas_w = w; OV.atlas_h = h;
        SelectObject(dc, oldb);
    }
    if (bmp) DeleteObject(bmp);
    for (i = 0; i < T_COUNT; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
    DeleteDC(dc);
}

/* ── drawing ──────────────────────────────────────────────────────────── */
static void ov_quad(uint32_t mode, float x0, float y0, float x1, float y1, const float col[4], const float a[4], const float b[4])
{
    D3D11_MAPPED_SUBRESOURCE map;
    OvConst k;
    memset(&k, 0, sizeof k);
    k.rect[0] = x0; k.rect[1] = y0; k.rect[2] = x1; k.rect[3] = y1;
    k.vp[0] = OV.vw; k.vp[1] = OV.vh;
    if (col) memcpy(k.col, col, sizeof k.col);
    if (a) memcpy(k.a, a, sizeof k.a);
    if (b) memcpy(k.b, b, sizeof k.b);
    k.mode[0] = mode;
    if (FAILED(ID3D11DeviceContext_Map(OV.ctx, (ID3D11Resource *)OV.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    memcpy(map.pData, &k, sizeof k);
    ID3D11DeviceContext_Unmap(OV.ctx, (ID3D11Resource *)OV.cb, 0);
    ID3D11DeviceContext_Draw(OV.ctx, 4, 0);
}

static void rgba(float o[4], int r, int g, int b, double a)
{
    o[0] = r / 255.0f; o[1] = g / 255.0f; o[2] = b / 255.0f; o[3] = (float)(a < 0 ? 0 : a > 1 ? 1 : a);
}

static void ov_plate(double x, double y, double w, double h, double r, int cr, int cg, int cb, double alpha)
{
    float c[4], a[4], b[4] = { 0, 0, 0, 0 };
    rgba(c, cr, cg, cb, alpha);
    a[0] = (float)(x + w / 2); a[1] = (float)(y + h / 2); a[2] = (float)(w / 2); a[3] = (float)(h / 2);
    b[0] = (float)r;
    ov_quad(0, (float)x - 1, (float)y - 1, (float)(x + w) + 1, (float)(y + h) + 1, c, a, b);
}

static void ov_glow(double cx, double cy, double rx, double ry, int cr, int cg, int cb, double alpha)
{
    float c[4], a[4];
    rgba(c, cr, cg, cb, alpha);
    a[0] = (float)cx; a[1] = (float)cy; a[2] = (float)rx; a[3] = (float)ry;
    ov_quad(1, (float)(cx - rx), (float)(cy - ry), (float)(cx + rx), (float)(cy + ry), c, a, NULL);
}

static void ov_mark(double x, double y, double s, double open, double alpha, int clear_gap)
{
    float a[4], b[4] = { 0, 0, 0, 0 };
    a[0] = (float)x; a[1] = (float)y; a[2] = (float)s; a[3] = (float)open;
    b[0] = (float)alpha; b[1] = clear_gap ? 1.0f : 0.0f;
    ov_quad(2, (float)x - 1, (float)y - 1, (float)(x + s) + 1, (float)(y + s) + 1, NULL, a, b);
}

/* The line of light through (cx, cy) at the gap's angle, half-length L. */
static void ov_seam(double cx, double cy, double L, double h, double alpha, double ends, int win_w, int win_h)
{
    float a[4], b[4] = { 0, 0, 0, 0 };
    a[0] = (float)cx; a[1] = (float)cy; a[2] = (float)L; a[3] = (float)h;
    b[0] = (float)alpha; b[1] = (float)ends;
    ov_quad(3, 0, 0, (float)win_w, (float)win_h, NULL, a, b);
}

/* Text: left edge x, baseline y. Returns its width. */
static double ov_text(int id, double x, double baseline, int cr, int cg, int cb, double alpha)
{
    const OvText *t = &OV.text[id];
    float c[4], b[4];
    double x0 = floor(x - 2 + 0.5), y0 = floor(baseline - t->ascent + 0.5);
    if (!OV.atlas_srv || alpha <= 0.002)
        return t->adv;
    rgba(c, cr, cg, cb, alpha);
    b[0] = (float)t->x / OV.atlas_w; b[1] = (float)t->y / OV.atlas_h;
    b[2] = (float)(t->x + t->w) / OV.atlas_w; b[3] = (float)(t->y + t->h) / OV.atlas_h;
    ov_quad(4, (float)x0, (float)y0, (float)(x0 + t->w), (float)(y0 + t->h), c, NULL, b);
    return t->adv;
}
static double ov_text_w(int id) { return OV.text[id].adv; }

/* ── the start screen ─────────────────────────────────────────────────────
 * The mark's gap continued as one line of light across the screen. Its
 * motion, all eased at both ends and dim: the mark appears closed (0.2 to
 * 0.8 s), splits open along its gap (0.7 to 1.45 s), and the light grows out
 * of the gap to both edges (0.85 to 2.05 s); the name slides a short way in
 * and fades up, then the version and the line; it holds; everything fades by
 * 3.2 s, the light last; the background then fades to the game's picture by
 * 3.4 s. */
static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double ease3(double x) { x = clamp01(x); return x * x * (3 - 2 * x); }
static double ease5(double x) { x = clamp01(x); return x * x * x * (x * (x * 6 - 15) + 10); }
static double span3(double t, double a, double b) { return ease3((t - a) / (b - a)); }
static double span5(double t, double a, double b) { return ease5((t - a) / (b - a)); }

static void ov_start_screen(double t, int W, int H)
{
    double out = 1 - span3(t, 2.55, 3.15);
    double mark = span3(t, 0.20, 0.80) * out, open = span5(t, 0.70, 1.45);
    double seamL = span5(t, 0.85, 2.05), seamA = span3(t, 0.85, 1.40) * (1 - span3(t, 2.60, 3.20));
    double name = span3(t, 1.25, 1.85) * out, slide = 1 - span5(t, 1.25, 1.95);
    double ver = span3(t, 1.45, 2.00) * out, legal = span3(t, 1.60, 2.15) * out;
    double glow = span3(t, 0.90, 1.80) * out, bg = 1 - span3(t, 3.00, 3.40);
    double sz = 0.22 * H, mx = W / 2.0 - 0.30 * H, my = 0.46 * H;
    double full = sqrt((double)W * W + (double)H * H);
    double tx, em;

    ov_plate(0, 0, W, H, 0, 7, 8, 13, bg);
    if (glow > 0.001) ov_glow(mx, my, 0.55 * H, 0.45 * H, 159, 144, 255, glow * 20.0 / 255.0);
    if (seamA > 0.001 && seamL * full > 2) ov_seam(mx, my, seamL * full, H, seamA, 0.0, W, H);
    if (mark > 0.001) ov_mark(mx - sz / 2, my - sz / 2, sz, open, mark, 1);
    tx = mx + sz / 2 + 0.07 * H - slide * 0.025 * H;
    em = 0.095 * H;
    ov_text(T_NAME_BIG, tx, my - 0.012 * H, 238, 240, 248, name);
    em = 0.027 * H;
    ov_text(T_VER, tx + 0.004 * H, my + 0.03 * H + 0.70 * em, 169, 175, 196, ver);
    em = 0.022 * H;
    ov_text(T_LEGAL, W / 2.0 - ov_text_w(T_LEGAL) / 2, 0.925 * H - 0.24 * em, 111, 118, 145, legal);
}

/* ── the corner mark: the mark and the name, top-right of the picture ──── */
static void ov_corner(double X, double Y, double Wd, double Hd, double alpha)
{
    double m = Hd * 0.032 > 10 ? Hd * 0.032 : 10, s = Hd * 0.05 > 20 ? Hd * 0.05 : 20;
    double em = Hd * 0.042 > 14 ? Hd * 0.042 : 14;
    double right = X + Wd - m, top = Y + m, gap = 0.32 * s, tw = ov_text_w(T_NAME_CORNER);
    double e = ease3(alpha);
    ov_glow(right - (s + gap + tw) / 2, top + s / 2, (s + gap + tw) * 0.75, s * 1.3, 0, 0, 0, e * 120.0 / 255.0);
    ov_mark(right - s, top, s, 1.0, e * 0.92, 0);
    ov_text(T_NAME_CORNER, right - s - gap - tw, top + s / 2 + 0.35 * em, 238, 240, 248, e * 0.88);
}

/* ── frame counter and notice ─────────────────────────────────────────── */
static double ov_hud_unit(int H) { return H * 0.03 > 13 ? H * 0.03 : 13; }

static double ov_counter(int H)
{
    double m = H * 0.018 > 8 ? H * 0.018 : 8, em = ov_hud_unit(H), w = 0, bw, bh, x;
    char num[16];
    int i, n = snprintf(num, sizeof num, "%d", OV.fps_value);
    for (i = 0; i < n; i++) w += ov_text_w(T_D0 + (num[i] - '0'));
    bw = w + 0.9 * em; bh = 1.32 * em;
    ov_plate(m, m, bw, bh, 0.18 * bh, 7, 8, 13, 150.0 / 255.0);
    x = m + 0.45 * em;
    for (i = 0; i < n; i++)
        x += ov_text(T_D0 + (num[i] - '0'), x, m + bh / 2 + 0.36 * em, 238, 240, 248, 0.92);
    return m + bh;
}

static void ov_notice(int H, double below, double t)
{
    double m = H * 0.018 > 8 ? H * 0.018 : 8, em = H * 0.026 > 13 ? H * 0.026 : 13;
    double a = t < 1200 ? 1.0 : 1.0 - (t - 1200) / 300.0;
    int id = OV.notice_id;
    double bw = ov_text_w(id) + 1.6 * em, bh = 1.7 * em, y = below > 0 ? below + 0.4 * em : m;
    if (a <= 0) return;
    a = ease3(a);
    ov_plate(m, y, bw, bh, 0.18 * bh, 7, 8, 13, a * 170.0 / 255.0);
    ov_plate(m + 0.35 * em, y + 0.28 * bh, 0.12 * em > 2 ? 0.12 * em : 2, 0.44 * bh, 0, 125, 170, 230, a);
    ov_text(id, m + 0.75 * em, y + bh / 2 + 0.36 * em, 238, 240, 248, a * 0.95);
}

/* ── screenshots ──────────────────────────────────────────────────────────
 * Copied on the GPU when asked, read back a frame later (no wait), and
 * written as a PNG by a worker thread with Windows' own image encoder. */
typedef struct { uint8_t *bgr; int w, h; char path[MAX_PATH]; } OvShot;

static DWORD WINAPI ov_shot_worker(LPVOID p)
{
    OvShot *s = (OvShot *)p;
    IWICImagingFactory *fac = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID fmt = OV_GUID_WICPixelFormat24bppBGR;
    wchar_t wpath[MAX_PATH];
    HRESULT hr, co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MultiByteToWideChar(CP_ACP, 0, s->path, -1, wpath, MAX_PATH);
    hr = CoCreateInstance(&OV_CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER, &OV_IID_IWICImagingFactory, (void **)&fac);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(fac, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(fac, &OV_GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, (UINT)s->w, (UINT)s->h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt);
    if (SUCCEEDED(hr) && memcmp(&fmt, &OV_GUID_WICPixelFormat24bppBGR, sizeof fmt) != 0) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, (UINT)s->h, (UINT)s->w * 3, (UINT)(s->w * 3 * s->h), s->bgr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (props) IPropertyBag2_Release(props);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (fac) IWICImagingFactory_Release(fac);
    if (FAILED(hr)) DeleteFileA(s->path);
    if (SUCCEEDED(co)) CoUninitialize();
    free(s->bgr);
    free(s);
    InterlockedExchange(&ov_shot_result, SUCCEEDED(hr) ? 1 : 2);
    InterlockedExchange(&ov_shot_busy, 0);
    return 0;
}

/* GB_SCREENSHOT_DIR, else "screenshots" beside the saves folder (the launcher
 * passes <install>\saves, so <install>\screenshots), else inside it. */
static int ov_shot_dir(char *out, size_t n)
{
    const char *e = getenv("GB_SCREENSHOT_DIR"), *root;
    char tmp[MAX_PATH];
    size_t len;
    char *slash;
    if (e && e[0]) {
        snprintf(out, n, "%s", e);
    } else {
        root = gb_save_root();
        if (!root || !root[0]) return 0;
        snprintf(tmp, sizeof tmp, "%s", root);
        len = strlen(tmp);
        while (len && (tmp[len - 1] == '\\' || tmp[len - 1] == '/')) tmp[--len] = 0;
        slash = strrchr(tmp, '\\');
        if (!slash) slash = strrchr(tmp, '/');
        if (slash && _stricmp(slash + 1, "saves") == 0) {
            *slash = 0;
            snprintf(out, n, "%s\\screenshots", tmp);
        } else {
            snprintf(out, n, "%s\\screenshots", tmp);
        }
    }
    CreateDirectoryA(out, NULL);
    return (GetFileAttributesA(out) & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

static void ov_shot_read(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    OvShot *s;
    SYSTEMTIME lt;
    char dir[MAX_PATH];
    int x, y, k;
    HRESULT hr = ID3D11DeviceContext_Map(OV.ctx, (ID3D11Resource *)OV.staging, 0, D3D11_MAP_READ,
                                         D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++OV.staging_wait < 8)
        return;                                     /* next frame */
    s = (OvShot *)calloc(1, sizeof *s);
    if (SUCCEEDED(hr) && s && (s->bgr = (uint8_t *)malloc((size_t)OV.staging_w * OV.staging_h * 3)) != NULL
        && ov_shot_dir(dir, sizeof dir)) {
        s->w = OV.staging_w; s->h = OV.staging_h;
        for (y = 0; y < s->h; y++) {
            const uint8_t *src = (const uint8_t *)map.pData + (size_t)y * map.RowPitch;
            uint8_t *dst = s->bgr + (size_t)y * s->w * 3;
            for (x = 0; x < s->w; x++) {
                if (OV.staging_rgba) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; }
                else { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }
                src += 4; dst += 3;
            }
        }
        GetLocalTime(&lt);
        for (k = 1; k < 100; k++) {
            if (k == 1)
                snprintf(s->path, sizeof s->path, "%s\\Gamebreaker %04u-%02u-%02u %02u-%02u-%02u.png", dir,
                         lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
            else
                snprintf(s->path, sizeof s->path, "%s\\Gamebreaker %04u-%02u-%02u %02u-%02u-%02u (%d).png", dir,
                         lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, k);
            if (GetFileAttributesA(s->path) == INVALID_FILE_ATTRIBUTES)
                break;
        }
        ID3D11DeviceContext_Unmap(OV.ctx, (ID3D11Resource *)OV.staging, 0);
        InterlockedExchange(&ov_shot_busy, 1);
        {
            HANDLE th = CreateThread(NULL, 0, ov_shot_worker, s, 0, NULL);
            if (th) CloseHandle(th);
            else { free(s->bgr); free(s); InterlockedExchange(&ov_shot_busy, 0); InterlockedExchange(&ov_shot_result, 2); }
        }
    } else {
        if (SUCCEEDED(hr)) ID3D11DeviceContext_Unmap(OV.ctx, (ID3D11Resource *)OV.staging, 0);
        if (s) { free(s->bgr); free(s); }
        InterlockedExchange(&ov_shot_result, 2);
    }
    ID3D11Texture2D_Release(OV.staging);
    OV.staging = NULL;
}

static void ov_shot_copy(IDXGISwapChain *swap, int win_w, int win_h, double vx, double vy, double vw, double vh)
{
    ID3D11Texture2D *back = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX box;
    int x0 = (int)floor(vx + 0.5), y0 = (int)floor(vy + 0.5);
    int x1 = (int)floor(vx + vw + 0.5), y1 = (int)floor(vy + vh + 0.5);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > win_w) x1 = win_w;
    if (y1 > win_h) y1 = win_h;
    if (x1 - x0 < 8 || y1 - y0 < 8 || FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) {
        InterlockedExchange(&ov_shot_result, 2);
        return;
    }
    ID3D11Texture2D_GetDesc(back, &td);
    OV.staging_rgba = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Width = (UINT)(x1 - x0); td.Height = (UINT)(y1 - y0);
    td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(OV.dev, &td, NULL, &OV.staging))) {
        box.left = (UINT)x0; box.top = (UINT)y0; box.front = 0;
        box.right = (UINT)x1; box.bottom = (UINT)y1; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(OV.ctx, (ID3D11Resource *)OV.staging, 0, 0, 0, 0,
                                                  (ID3D11Resource *)back, 0, &box);
        OV.staging_w = x1 - x0; OV.staging_h = y1 - y0; OV.staging_wait = 0;
    } else {
        InterlockedExchange(&ov_shot_result, 2);
    }
    ID3D11Texture2D_Release(back);
}

/* ── per frame ────────────────────────────────────────────────────────── */
void gb_overlay_frame(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, IDXGISwapChain *swap,
                      int win_w, int win_h, float vx, float vy, float vw, float vh, unsigned verts3d)
{
    /* A match: the loading screens draw a dark city behind them in 3D, about
     * 24,000 vertices a frame; a court, from its fly-in on, 54,000 to 145,000.
     * Once a court has been seen the match lasts until the menus' movies come
     * back or nothing is drawn in 3D for a second, so a sparse camera angle
     * cannot bring the mark back during play. */
    int scene3d;
    D3D11_VIEWPORT vp;
    double now, t, dt, below = 0;
    int start_on, branded;
    LONGLONG tick;
    LONG r;

    if (!ctx || !rtv || win_w < 16 || win_h < 16 || !ov_init(ctx))
        return;
    OV.ctx = ctx;
    now = ov_now_ms();
    if (!ov_start_qpc) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        InterlockedExchange64(&ov_start_qpc, c.QuadPart);
        OV.last_ms = now;
    }
    t = now - (double)ov_start_qpc * 1000.0 / (double)OV.freq.QuadPart;
    dt = now - OV.last_ms;
    OV.last_ms = now;
    start_on = t < START_TOTAL_MS;

    if (OV.staging)                                 /* a screenshot asked for last frame */
        ov_shot_read();

    ov_build_text(win_h, (int)(vh + 0.5));
    OV.vw = (float)win_w; OV.vh = (float)win_h;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)win_w; vp.Height = (float)win_h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, OV.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &OV.cb);
    ID3D11DeviceContext_PSSetShader(ctx, OV.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &OV.cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &OV.atlas_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &OV.sampler);
    ID3D11DeviceContext_RSSetState(ctx, OV.raster);
    ID3D11DeviceContext_OMSetBlendState(ctx, OV.blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);

    /* Which screen. The title and main-menu group by the last movie reported
     * (the title's background plays once and then stands still, so the last
     * one counts until another starts). A loading screen as no movie for more
     * than 0.6 s outside that group: every menu has a moving background, and
     * leaving the menus for a match plays one last transition, then stops for
     * the loading card and the controls screen until the court is drawn. The
     * boot loading screen as anything before the first movie. Never during a
     * match (above): its fly-in, play, pause menu and replays. */
    tick = ov_movie_tick;
    if (verts3d)
        OV.any3d_ms = now;
    if (verts3d > OV_SCENE_VERTS && !OV.in_play) {
        OV.in_play = 1;
        OV.play_tick = tick;
    } else if (OV.in_play && (tick != OV.play_tick || now - OV.any3d_ms > 1000.0)) {
        OV.in_play = 0;
    }
    scene3d = OV.in_play;
    if (scene3d)
        OV.last3d_ms = now;
    branded = !scene3d && now - OV.last3d_ms > 500.0 && (ov_movie_class == 1 || !ov_any_movie
                           || (LONGLONG)GetTickCount64() - tick > 600);
    if (branded != OV.corner_branded) {
        OV.corner_branded = branded;
    }
    if (dt < 0 || dt > 250) dt = 16.7;
    OV.corner_alpha += branded ? dt / 400.0 : -dt / 250.0;
    OV.corner_alpha = clamp01(OV.corner_alpha);

    if (start_on)
        ov_start_screen(t / 1000.0, win_w, win_h);
    else if (OV.corner_alpha > 0.001)
        ov_corner(vx, vy, vw, vh, OV.corner_alpha);

    /* The picture as shown, the corner mark with it; not the counter or the
     * notice, which come after. */
    if (ov_shot_request && !OV.staging && !ov_shot_busy) {
        InterlockedExchange(&ov_shot_request, 0);
        ov_shot_copy(swap, win_w, win_h, vx, vy, vw, vh);
    }

    if (OV.show_fps) {
        int i, k = 0;
        OV.fps_times[OV.fps_head] = now;
        OV.fps_head = (OV.fps_head + 1) % 256;
        if (OV.fps_n < 256) OV.fps_n++;
        if (now - OV.fps_shown_at >= 500) {
            for (i = 0; i < OV.fps_n; i++)
                if (now - OV.fps_times[i] < 1000.0) k++;
            OV.fps_value = k;
            OV.fps_shown_at = now;
        }
        below = ov_counter(win_h);
    }
    r = InterlockedExchange(&ov_shot_result, 0);
    if (r) { OV.notice_ok = r == 1; OV.notice_id = r == 1 ? T_SAVED : T_NOTSAVED; OV.notice_at = now; }
    r = InterlockedExchange(&ov_dump_note, 0);
    if (r) { OV.notice_id = r == 1 ? T_DUMPON : T_DUMPOFF; OV.notice_at = now; }
    if (OV.notice_at > 0 && now - OV.notice_at < 1500)
        ov_notice(win_h, below, now - OV.notice_at);

    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    }
}

void gb_overlay_notice_dump(int on)
{
    InterlockedExchange(&ov_dump_note, on ? 1 : 2);
}
