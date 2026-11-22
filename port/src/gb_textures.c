/*
 * gb_textures.c - textures by fingerprint.
 *
 * A mod may hold a folder named textures with pictures named by the
 * fingerprint of a game texture:
 *
 *   mods\<mod>\textures\<fingerprint>[ anything].png    (or .dds)
 *
 * The fingerprint is 16 hexadecimal digits (gb_tex_fingerprint). Subfolders
 * of textures are allowed and do not change the meaning; text after the
 * digits (separated by a space, _ or -) is for people. The renderer draws
 * the picture wherever the game draws a texture with that fingerprint, at
 * the picture's own size, and nothing of the game's files is edited, so a
 * mod of this kind carries only its maker's pictures.
 *
 * Load order is the mods folder's (gb_mods.c): when two mods name the same
 * fingerprint, the first wins and the log says so.
 *
 * The dump (GB_TEXTURE_DUMP=1, or a folder) is the maker's way to find what
 * to replace: each texture the game uses is written once as
 * <fingerprint>.png into texture_dump\ beside the mods folder, with a line
 * in index.txt saying its size, format and when it was first drawn. The
 * pictures come from the player's own disc and stay on the player's PC.
 *
 * Everything here runs on the back end's thread, except the dump's writer.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb_textures.h"
#include "gb_mods.h"
#include "gb_save.h"
#include "gb_log.h"

#define TX_MAX_FILES   65536
#define TX_MAX_SIDE    8192                 /* a picture larger than this on either side is refused */
#define TX_MAX_FILE    (256u << 20)         /* bytes of one file */
#define TX_BUDGET      (2048ull << 20)      /* bytes of pictures held on the GPU at once */
#define TX_IDLE_FRAMES 1800                 /* a picture not drawn for 30 s at 60 Hz is let go */
#define TX_LOG_LINES   200

/* Windows' image codecs, named here so no extra import library is needed. */
static const GUID TX_CLSID_WICImagingFactory1 = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID TX_IID_IWICImagingFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID TX_GUID_ContainerFormatPng = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID TX_GUID_WICPixelFormat32bppBGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

typedef struct {
    uint64_t fp;
    int      mod;                           /* index in the load order */
    WCHAR   *path;
    int      state;                         /* 0 not loaded, 1 loaded, 2 cannot be used */
    int      told;                          /* its first use is in the log */
    ID3D11ShaderResourceView *srv;
    uint32_t w, h, levels;
    uint64_t bytes;
    uint32_t used_frame;
} TxPic;

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static IWICImagingFactory  *s_wic;
static TxPic   *s_tab;                      /* open addressing on fp, s_cap a power of two */
static uint32_t s_cap, s_npics;
static uint64_t s_bytes;                    /* held now */
static int      s_lines, s_budget_told;
static uint32_t s_frame;

/* At most TX_LOG_LINES picture lines a start, so a large pack cannot fill the
 * log. The line after the last one says so: the launcher's Mods page reads
 * "texture <fp> from <mod>" to mark the pictures drawn, and after this line a
 * picture with no line of its own may have been drawn too. */
static int log_ok(void)
{
    if (s_lines > TX_LOG_LINES)
        return 0;
    if (++s_lines <= TX_LOG_LINES)
        return 1;
    gb_log("[MODS] textures: %d picture lines written; no more this start\n", TX_LOG_LINES);
    return 0;
}

/* ── the fingerprint ─────────────────────────────────────────────────────
 * Published: mod makers name files by it, so it must never change. It is
 * the back end's 64-bit hash of the level-0 bytes as the game holds them
 * (swizzled or compressed, before decoding; gb_d3d11.c hash_bytes), mixed
 * with the width, height and the game's format code, then the hash of the
 * palette for a palettised texture (a recoloured copy is another picture).
 * The levels below 0 are left out: how many of them are found depends on
 * the texture filtering setting. */
uint64_t gb_tex_fingerprint(uint64_t level0_hash, uint32_t width, uint32_t height,
                            uint32_t color, uint64_t palette_hash)
{
    uint64_t x = level0_hash ^ ((uint64_t)(color & 0xFF) << 56
                                | (uint64_t)(width & 0xFFFFFFF) << 28 | (height & 0xFFFFFFF));
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x ^ palette_hash;
}

static TxPic *find(uint64_t fp)
{
    uint32_t i;
    if (!s_tab)
        return NULL;
    for (i = (uint32_t)(fp ^ fp >> 32) & (s_cap - 1); s_tab[i].path; i = (i + 1) & (s_cap - 1))
        if (s_tab[i].fp == fp)
            return &s_tab[i];
    return NULL;
}

/* ── the scan ─────────────────────────────────────────────────────────── */

typedef struct { uint64_t fp; int mod; WCHAR *path; } TxFound;
static TxFound *s_found;
static int s_nfound, s_capfound;

static int hexval(WCHAR c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

/* The fingerprint a file name gives, or 0 when it gives none: 16 hex digits
 * at the start, then the end of the name or a space, _ or -, and the
 * extension .png or .dds. */
static int name_fp(const WCHAR *name, uint64_t *fp)
{
    const WCHAR *dot = wcsrchr(name, L'.');
    uint64_t v = 0;
    int i;

    if (!dot || (_wcsicmp(dot, L".png") && _wcsicmp(dot, L".dds")))
        return 0;
    for (i = 0; i < 16; i++) {
        int d = hexval(name[i]);
        if (d < 0)
            return 0;
        v = v << 4 | (uint64_t)d;
    }
    if (name + 16 != dot && name[16] != L' ' && name[16] != L'_' && name[16] != L'-')
        return 0;
    *fp = v;
    return 1;
}

static void found(uint64_t fp, int mod, const WCHAR *path)
{
    if (s_nfound >= TX_MAX_FILES)
        return;
    if (s_nfound == s_capfound) {
        int cap = s_capfound ? s_capfound * 2 : 256;
        TxFound *n = (TxFound *)realloc(s_found, (size_t)cap * sizeof *n);
        if (!n)
            return;
        s_found = n;
        s_capfound = cap;
    }
    s_found[s_nfound].fp = fp;
    s_found[s_nfound].mod = mod;
    s_found[s_nfound].path = _wcsdup(path);
    if (s_found[s_nfound].path)
        s_nfound++;
}

static void shown_name(const WCHAR *path, char *out, int size)
{
    /* From the mod's folder on: "<mod>\textures\...". */
    const WCHAR *p = wcsstr(path, L"\\textures\\"), *q = p;
    while (q && q > path && q[-1] != L'\\')
        q--;
    WideCharToMultiByte(CP_UTF8, 0, q ? q : path, -1, out, size, NULL, NULL);
}

static void scan(const WCHAR *dir, int mod, int depth)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;

    if (depth > 8)
        return;
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*", dir);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        uint64_t fp;
        if (fd.cFileName[0] == L'.')
            continue;
        _snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan(sub, mod, depth + 1);
        } else if (!name_fp(fd.cFileName, &fp)) {
            char n[MAX_PATH];
            shown_name(sub, n, sizeof n);
            if (log_ok())
                gb_log("[MODS] %s: not named by a texture fingerprint (16 hex digits, then .png "
                       "or .dds); not used\n", n);
        } else if (fd.nFileSizeHigh || fd.nFileSizeLow > TX_MAX_FILE || !fd.nFileSizeLow) {
            char n[MAX_PATH];
            shown_name(sub, n, sizeof n);
            if (log_ok())
                gb_log("[MODS] %s: %s; not used\n", n, fd.nFileSizeLow ? "larger than 256 MB" : "empty");
        } else {
            found(fp, mod, sub);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void build_table(void)
{
    int i, clashes = 0;
    int per_mod[64];

    memset(per_mod, 0, sizeof per_mod);
    for (s_cap = 64; s_cap < (uint32_t)s_nfound * 2; s_cap <<= 1)
        ;
    s_tab = (TxPic *)calloc(s_cap, sizeof *s_tab);
    if (!s_tab)
        return;
    for (i = 0; i < s_nfound; i++) {
        TxFound *f = &s_found[i];
        TxPic *have = find(f->fp);
        uint32_t k;
        if (have) {
            /* Same mod twice (two names for one fingerprint): the first in
             * folder order. Another mod: the first in the load order. */
            char a[MAX_PATH], b[MAX_PATH];
            shown_name(have->path, a, sizeof a);
            shown_name(f->path, b, sizeof b);
            clashes++;
            if (log_ok())
                gb_log("[MODS] texture %016llx: %s used, %s passed over\n",
                       (unsigned long long)f->fp, a, b);
            free(f->path);
            continue;
        }
        for (k = (uint32_t)(f->fp ^ f->fp >> 32) & (s_cap - 1); s_tab[k].path; k = (k + 1) & (s_cap - 1))
            ;
        s_tab[k].fp = f->fp;
        s_tab[k].mod = f->mod;
        s_tab[k].path = f->path;
        s_npics++;
        if (f->mod >= 0 && f->mod < 64)
            per_mod[f->mod]++;
    }
    free(s_found);
    s_found = NULL;
    for (i = 0; i < gb_mods_count() && i < 64; i++)
        if (per_mod[i])
            gb_log("[MODS] %s: %d texture%s by fingerprint\n", gb_mods_name(i), per_mod[i],
                   per_mod[i] == 1 ? "" : "s");
    if (clashes)
        gb_log("[MODS] textures: %d fingerprint%s named by more than one picture; the first in the "
               "load order is used\n", clashes, clashes == 1 ? "" : "s");
}

/* ── loading a picture ────────────────────────────────────────────────── */

static uint64_t chain_bytes(uint32_t w, uint32_t h, uint32_t levels, uint32_t bpp_x8, int block)
{
    uint64_t n = 0;
    uint32_t i;
    for (i = 0; i < levels; i++) {
        uint32_t lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
        n += block ? (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * (uint32_t)block
                   : (uint64_t)lw * lh * bpp_x8 / 8;
    }
    return n;
}

static uint32_t full_levels(uint32_t w, uint32_t h)
{
    uint32_t n = 1, s = w > h ? w : h;
    while (s > 1) { s >>= 1; n++; }
    return n;
}

static int budget_ok(uint64_t bytes, const char *name)
{
    if (s_bytes + bytes <= TX_BUDGET)
        return 1;
    if (!s_budget_told) {
        s_budget_told = 1;
        gb_log("[MODS] textures: more than 2 GB of pictures at once; %s and any more not used "
               "until others are let go\n", name);
    }
    return 0;
}

/* A PNG (or anything Windows can decode) to 32-bit BGRA with every mip
 * level made on the GPU. 1 loaded, 0 cannot be used, -1 over the budget. */
static int load_wic(TxPic *p, int opaque, const char *name)
{
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    uint8_t *px = NULL;
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    uint64_t bytes;
    int ok = 0;

    if (!s_wic)
        return 0;
    hr = IWICImagingFactory_CreateDecoderFromFilename(s_wic, p->path, NULL, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(fr, &w, &h);
    if (FAILED(hr) || !w || !h) {
        if (log_ok())
            gb_log("[MODS] %s: cannot be read as a picture; not used\n", name);
        goto done;
    }
    if (w > TX_MAX_SIDE || h > TX_MAX_SIDE) {
        if (log_ok())
            gb_log("[MODS] %s: %ux%u is larger than %u on a side; not used\n", name, w, h, TX_MAX_SIDE);
        goto done;
    }
    bytes = chain_bytes(w, h, full_levels(w, h), 32, 0);
    if (!budget_ok(bytes, name)) {
        ok = -1;
        goto done;
    }
    px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!px)
        goto done;
    hr = IWICImagingFactory_CreateFormatConverter(s_wic, &conv);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)fr, &TX_GUID_WICPixelFormat32bppBGRA,
                                            WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, px);
    if (FAILED(hr)) {
        if (log_ok())
            gb_log("[MODS] %s: cannot be read as a picture; not used\n", name);
        goto done;
    }
    if (opaque) {
        size_t i, n = (size_t)w * h;
        for (i = 0; i < n; i++)
            px[i * 4 + 3] = 0xFF;
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &p->srv)))
        goto done;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)tex, 0, NULL, px, w * 4, 0);
    ID3D11DeviceContext_GenerateMips(s_ctx, p->srv);
    p->w = w; p->h = h; p->levels = full_levels(w, h); p->bytes = bytes;
    ok = 1;
done:
    if (tex) ID3D11Texture2D_Release(tex);   /* the view keeps it */
    if (conv) IWICFormatConverter_Release(conv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    free(px);
    return ok;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A DDS file: block-compressed (BC1, BC2, BC3, BC7) or 32-bit, used as it
 * is with the mip levels it holds (none are made). */
static int load_dds(TxPic *p, const char *name)
{
    HANDLE f = CreateFileW(p->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size = 0, got = 0;
    uint8_t *d = NULL;
    uint32_t w, h, levels, pfflags, fourcc, bits, rmask, amask, data, i, block = 0, bpp = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[16];
    ID3D11Texture2D *tex = NULL;
    uint64_t at, bytes;
    int ok = 0;
    const char *why = "not a DDS file this game can use";

    if (f == INVALID_HANDLE_VALUE)
        goto bad;
    size = GetFileSize(f, NULL);
    if (size < 128 || size > TX_MAX_FILE || !(d = (uint8_t *)malloc(size))
        || !ReadFile(f, d, size, &got, NULL) || got != size || memcmp(d, "DDS ", 4) || rd32(d + 4) != 124)
        goto bad;
    h = rd32(d + 12); w = rd32(d + 16); levels = rd32(d + 28);
    pfflags = rd32(d + 80); fourcc = rd32(d + 84); bits = rd32(d + 88);
    rmask = rd32(d + 92); amask = rd32(d + 104);
    data = 128;
    if (!levels) levels = 1;
    if (levels > 16) levels = 16;
    if ((pfflags & 4) && fourcc == 0x31545844u) { fmt = DXGI_FORMAT_BC1_UNORM; block = 8; }       /* DXT1 */
    else if ((pfflags & 4) && fourcc == 0x33545844u) { fmt = DXGI_FORMAT_BC2_UNORM; block = 16; } /* DXT3 */
    else if ((pfflags & 4) && fourcc == 0x35545844u) { fmt = DXGI_FORMAT_BC3_UNORM; block = 16; } /* DXT5 */
    else if ((pfflags & 4) && fourcc == 0x30315844u && size >= 148) {                             /* DX10 */
        uint32_t g = rd32(d + 128);
        data = 148;
        if (rd32(d + 132) != 3)                 /* a 2D texture */
            goto bad;
        switch (g) {
        case 71: case 72: fmt = DXGI_FORMAT_BC1_UNORM; block = 8; break;
        case 74: case 75: fmt = DXGI_FORMAT_BC2_UNORM; block = 16; break;
        case 77: case 78: fmt = DXGI_FORMAT_BC3_UNORM; block = 16; break;
        case 98: case 99: fmt = DXGI_FORMAT_BC7_UNORM; block = 16; break;
        case 87: case 91: fmt = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 32; break;
        case 28: case 29: fmt = DXGI_FORMAT_R8G8B8A8_UNORM; bpp = 32; break;
        }
    } else if ((pfflags & 0x40) && bits == 32) {                                                  /* plain RGB(A) */
        if (rmask == 0x00FF0000u) { fmt = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 32; }
        else if (rmask == 0x000000FFu) { fmt = DXGI_FORMAT_R8G8B8A8_UNORM; bpp = 32; }
        if (!amask || !(pfflags & 1))
            fmt = fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8X8_UNORM : fmt;
    }
    if (fmt == DXGI_FORMAT_UNKNOWN || !w || !h)
        goto bad;
    if (w > TX_MAX_SIDE || h > TX_MAX_SIDE) {
        why = "larger than 8192 on a side";
        goto bad;
    }
    if (block && ((w & 3) || (h & 3))) {
        why = "a compressed picture must be a multiple of 4 wide and high";
        goto bad;
    }
    while (levels > 1 && data + chain_bytes(w, h, levels, bpp, (int)block) > size)
        levels--;                           /* fewer levels in the file than it says */
    bytes = chain_bytes(w, h, levels, bpp, (int)block);
    if (data + bytes > size) {
        why = "shorter than its header says";
        goto bad;
    }
    if (!budget_ok(bytes, name)) {
        ok = -1;
        goto done;
    }
    at = data;
    for (i = 0; i < levels; i++) {
        uint32_t lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
        init[i].pSysMem = d + at;
        init[i].SysMemPitch = block ? ((lw + 3) / 4) * block : lw * bpp / 8;
        init[i].SysMemSlicePitch = 0;
        at += chain_bytes(lw, lh, 1, bpp, (int)block);
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = levels; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, init, &tex))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &p->srv))) {
        why = "the graphics card would not take it";
        goto bad;
    }
    p->w = w; p->h = h; p->levels = levels; p->bytes = bytes;
    ok = 1;
    goto done;
bad:
    if (log_ok())
        gb_log("[MODS] %s: %s; not used\n", name, why);
done:
    if (tex) ID3D11Texture2D_Release(tex);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(d);
    return ok;
}

/* Formats the game draws without alpha: a picture replacing one is drawn
 * opaque, as the game's own would be. */
static int opaque_format(uint32_t color)
{
    switch (color) {
    case 0x03: case 0x05: case 0x07: case 0x11: case 0x1C: case 0x1E:
        return 1;
    }
    return 0;
}

ID3D11ShaderResourceView *gb_tex_replacement(uint64_t fp, uint32_t color, uint32_t *levels)
{
    TxPic *p = find(fp);
    char name[MAX_PATH];

    if (!p || p->state == 2)
        return NULL;
    if (p->state == 0) {
        LARGE_INTEGER q0, q1, qf;
        const WCHAR *dot = wcsrchr(p->path, L'.');
        int ok;
        shown_name(p->path, name, sizeof name);
        QueryPerformanceCounter(&q0);
        ok = dot && !_wcsicmp(dot, L".dds") ? load_dds(p, name) : load_wic(p, opaque_format(color), name);
        QueryPerformanceCounter(&q1);
        QueryPerformanceFrequency(&qf);
        if (ok <= 0) {
            /* Over the budget (-1): tried again the next time the game
             * uploads that texture. Otherwise never again this run. */
            p->state = ok < 0 ? 0 : 2;
            return NULL;
        }
        p->state = 1;
        s_bytes += p->bytes;
        if (!p->told) {
            p->told = 1;
            if (log_ok())
                gb_log("[MODS] texture %016llx from %s (%ux%u, %u level%s, %.1f ms)\n",
                       (unsigned long long)fp, gb_mods_name(p->mod), p->w, p->h, p->levels,
                       p->levels == 1 ? "" : "s",
                       (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart);
        }
    }
    p->used_frame = s_frame;
    *levels = p->levels;
    ID3D11ShaderResourceView_AddRef(p->srv);
    return p->srv;
}

void gb_tex_frame(uint32_t frame)
{
    uint32_t i;
    s_frame = frame;
    if (!s_tab || frame % 600)
        return;
    /* The back end's own cache holds its references; this lets go of the
     * table's, so a picture no screen draws any more leaves the GPU once the
     * cache drops it too. */
    for (i = 0; i < s_cap; i++) {
        TxPic *p = &s_tab[i];
        if (p->state == 1 && frame - p->used_frame > TX_IDLE_FRAMES) {
            ID3D11ShaderResourceView_Release(p->srv);
            p->srv = NULL;
            p->state = 0;
            s_bytes -= p->bytes;
            s_budget_told = 0;
        }
    }
}

/* ── the dump ─────────────────────────────────────────────────────────── */

typedef struct TxJob {
    struct TxJob *next;
    uint64_t fp;
    uint32_t w, h, color, levels, frame;
    int scene3d;
    double seconds;
    uint32_t *px;
} TxJob;

static int      s_dump;                    /* the dump's folder is known (Ctrl+F9 can switch it on) */
static volatile LONG s_dump_on;            /* writing now: GB_TEXTURE_DUMP at start, then Ctrl+F9 */
static WCHAR    s_dump_dir[MAX_PATH];
static uint64_t *s_seen;                    /* fingerprints written this run */
static uint32_t s_seen_cap, s_nseen;
static CRITICAL_SECTION s_qlock;
static HANDLE   s_qevent;
static TxJob   *s_qhead, *s_qtail;
static LONG     s_qcount;
static LARGE_INTEGER s_t0;

int gb_tex_dump_wanted(uint64_t fp)
{
    uint32_t i;
    if (!s_dump || !s_dump_on || !fp)
        return 0;
    if (s_nseen * 2 >= s_seen_cap) {
        uint32_t cap = s_seen_cap ? s_seen_cap * 2 : 4096, k;
        uint64_t *n = (uint64_t *)calloc(cap, sizeof *n);
        if (!n)
            return 0;
        for (k = 0; k < s_seen_cap; k++)
            if (s_seen[k]) {
                uint32_t j = (uint32_t)(s_seen[k] ^ s_seen[k] >> 32) & (cap - 1);
                while (n[j]) j = (j + 1) & (cap - 1);
                n[j] = s_seen[k];
            }
        free(s_seen);
        s_seen = n;
        s_seen_cap = cap;
    }
    for (i = (uint32_t)(fp ^ fp >> 32) & (s_seen_cap - 1); s_seen[i]; i = (i + 1) & (s_seen_cap - 1))
        if (s_seen[i] == fp)
            return 0;
    s_seen[i] = fp;
    s_nseen++;
    return 1;
}

static const char *format_name(uint32_t c)
{
    switch (c) {
    case 0x00: return "Y8";
    case 0x01: return "AY8";
    case 0x02: return "A1R5G5B5";
    case 0x03: return "X1R5G5B5";
    case 0x04: return "A4R4G4B4";
    case 0x05: return "R5G6B5";
    case 0x06: return "A8R8G8B8";
    case 0x07: return "X8R8G8B8";
    case 0x0B: return "P8 (palette)";
    case 0x0C: return "DXT1";
    case 0x0E: return "DXT3";
    case 0x0F: return "DXT5";
    case 0x11: return "linear R5G6B5";
    case 0x12: return "linear A8R8G8B8";
    case 0x19: return "A8";
    case 0x1C: return "linear X1R5G5B5";
    case 0x1E: return "linear X8R8G8B8";
    }
    return "other";
}

int gb_tex_dump_ready(void)
{
    return s_dump;
}

static int dump_start(void);

int gb_tex_dump_toggle(void)
{
    LONG now;
    if (!s_dump)
        return -1;
    now = !s_dump_on;
    if (now && !dump_start())
        return -1;
    InterlockedExchange(&s_dump_on, now);
    gb_log("[MODS] texture dump %s (Ctrl+F9)\n", now ? "on" : "off");
    return (int)now;
}

void gb_tex_dump(uint64_t fp, uint32_t width, uint32_t height, uint32_t color, uint32_t levels,
                 const uint32_t *bgra, uint32_t frame, int scene3d)
{
    TxJob *j;
    LARGE_INTEGER now, qf;
    size_t n = (size_t)width * height;

    if (!s_dump || !s_dump_on || !n || s_qcount > 512)    /* the writer far behind: let some go */
        return;
    j = (TxJob *)calloc(1, sizeof *j);
    if (!j || !(j->px = (uint32_t *)malloc(n * 4))) {
        free(j);
        return;
    }
    memcpy(j->px, bgra, n * 4);
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&qf);
    j->fp = fp; j->w = width; j->h = height; j->color = color; j->levels = levels;
    j->frame = frame; j->scene3d = scene3d;
    j->seconds = (double)(now.QuadPart - s_t0.QuadPart) / (double)qf.QuadPart;
    EnterCriticalSection(&s_qlock);
    if (s_qtail) s_qtail->next = j; else s_qhead = j;
    s_qtail = j;
    s_qcount++;
    LeaveCriticalSection(&s_qlock);
    SetEvent(s_qevent);
}

static int write_png(IWICImagingFactory *fac, const WCHAR *path, const TxJob *j)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID fmt = TX_GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = IWICImagingFactory_CreateStream(fac, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(fac, &TX_GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, j->w, j->h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt);
    if (SUCCEEDED(hr) && memcmp(&fmt, &TX_GUID_WICPixelFormat32bppBGRA, sizeof fmt) != 0) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, j->h, j->w * 4, j->w * 4 * j->h, (BYTE *)j->px);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (props) IPropertyBag2_Release(props);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (FAILED(hr)) DeleteFileW(path);
    return SUCCEEDED(hr);
}

static DWORD WINAPI dump_writer(LPVOID unused)
{
    IWICImagingFactory *fac = NULL;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    WCHAR index[MAX_PATH];
    (void)unused;
    CoCreateInstance(&TX_CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER, &TX_IID_IWICImagingFactory,
                     (void **)&fac);
    _snwprintf_s(index, MAX_PATH, _TRUNCATE, L"%s\\index.txt", s_dump_dir);
    for (;;) {
        TxJob *j;
        WaitForSingleObject(s_qevent, INFINITE);
        for (;;) {
            WCHAR path[MAX_PATH];
            FILE *f;
            int wrote = 0;
            EnterCriticalSection(&s_qlock);
            j = s_qhead;
            if (j) {
                s_qhead = j->next;
                if (!s_qhead) s_qtail = NULL;
                s_qcount--;
            }
            LeaveCriticalSection(&s_qlock);
            if (!j)
                break;
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%016llx.png", s_dump_dir, (unsigned long long)j->fp);
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                wrote = 2;                  /* written by an earlier run */
            else if (fac)
                wrote = write_png(fac, path, j);
            if (wrote == 1 && _wfopen_s(&f, index, L"a") == 0 && f) {
                fprintf(f, "%016llx\t%ux%u\t%s\t%u\t%.1f\t%u\t%s\n", (unsigned long long)j->fp,
                        j->w, j->h, format_name(j->color), j->levels, j->seconds, j->frame,
                        j->scene3d ? "3D" : "2D");
                fclose(f);
            }
            free(j->px);
            free(j);
        }
    }
    /* not reached: the thread ends with the program */
#if 0
    if (fac) IWICImagingFactory_Release(fac);
    if (SUCCEEDED(co)) CoUninitialize();
#endif
    (void)co;
    return 0;
}

/* The dump's folder and writer, made the first time the dump is switched on
 * (at start with GB_TEXTURE_DUMP, or by Ctrl+F9 in the game). Any thread;
 * once. 1 ready, 0 not possible. */
static volatile LONG s_started;             /* 0 not yet, 1 being made, 2 ready, 3 failed */

static int dump_start(void)
{
    char dir[MAX_PATH], shown[MAX_PATH];
    WCHAR index[MAX_PATH];
    HANDLE th;
    LONG st = InterlockedCompareExchange(&s_started, 1, 0);

    if (st == 2) return 1;
    if (st != 0) {                          /* another thread is making it, or it failed */
        while (s_started == 1) Sleep(1);
        return s_started == 2;
    }
    WideCharToMultiByte(CP_ACP, 0, s_dump_dir, -1, dir, sizeof dir, NULL, NULL);
    CreateDirectoryW(s_dump_dir, NULL);
    if (GetFileAttributesW(s_dump_dir) == INVALID_FILE_ATTRIBUTES
        || !(GetFileAttributesW(s_dump_dir) & FILE_ATTRIBUTE_DIRECTORY)) {
        gb_log("[MODS] texture dump: cannot make the folder %s\n", gb_log_user_path(dir, shown, sizeof shown));
        InterlockedExchange(&s_started, 3);
        return 0;
    }
    _snwprintf_s(index, MAX_PATH, _TRUNCATE, L"%s\\index.txt", s_dump_dir);
    if (GetFileAttributesW(index) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = NULL;
        if (_wfopen_s(&f, index, L"w") == 0 && f) {
            fprintf(f, "fingerprint\tsize\tformat\tlevels\tfirst drawn (s)\tframe\tscene\n");
            fclose(f);
        }
    }
    InitializeCriticalSection(&s_qlock);
    s_qevent = CreateEventW(NULL, FALSE, FALSE, NULL);
    th = s_qevent ? CreateThread(NULL, 0, dump_writer, NULL, 0, NULL) : NULL;
    if (!th) {
        InterlockedExchange(&s_started, 3);
        return 0;
    }
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
    gb_log("[MODS] texture dump: each texture drawn while it is on is written once to %s\n",
           gb_log_user_path(dir, shown, sizeof shown));
    InterlockedExchange(&s_started, 2);
    return 1;
}

/* The dump's folder: texture_dump beside the mods folder, or the folder
 * GB_TEXTURE_DUMP names. GB_TEXTURE_DUMP=1 (or a folder) also starts with
 * the dump on; Ctrl+F9 toggles it either way. */
static void dump_init(void)
{
    const char *e = getenv("GB_TEXTURE_DUMP");
    char dir[MAX_PATH], *cut;

    QueryPerformanceCounter(&s_t0);
    if (e && e[0] && strcmp(e, "0") && strcmp(e, "1")) {
        GetFullPathNameA(e, sizeof dir, dir, NULL);
    } else {
        snprintf(dir, sizeof dir, "%s", gb_save_root());
        cut = strrchr(dir, '\\');
        if (cut)
            *cut = 0;
        strncat_s(dir, sizeof dir, "\\texture_dump", _TRUNCATE);
    }
    MultiByteToWideChar(CP_ACP, 0, dir, -1, s_dump_dir, MAX_PATH);
    s_dump = 1;
    if (e && e[0] && strcmp(e, "0") && dump_start()) {
        InterlockedExchange(&s_dump_on, 1);
        gb_log("[MODS] texture dump on at start (Ctrl+F9 to toggle)\n");
    }
}

/* ── start ────────────────────────────────────────────────────────────── */

int gb_tex_init(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    int i, n = gb_mods_count();

    s_dev = dev;
    s_ctx = ctx;
    for (i = 0; i < n; i++) {
        WCHAR dir[MAX_PATH], tdir[MAX_PATH];
        DWORD a;
        if (!gb_mods_folder(i, dir, MAX_PATH))
            continue;
        _snwprintf_s(tdir, MAX_PATH, _TRUNCATE, L"%s\\textures", dir);
        a = GetFileAttributesW(tdir);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            scan(tdir, i, 0);
    }
    if (s_nfound)
        build_table();
    if (s_npics) {
        HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        (void)co;                           /* already set up on this thread is as good */
        if (FAILED(CoCreateInstance(&TX_CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER,
                                    &TX_IID_IWICImagingFactory, (void **)&s_wic)))
            s_wic = NULL;                   /* DDS pictures still work */
    }
    dump_init();
    return 1;                           /* Ctrl+F9 can switch the dump on at any time */
}
