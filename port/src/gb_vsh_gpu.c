/*
 * gb_vsh_gpu.c - the title's vertex programs run on the graphics card.
 *
 * The toolkit's executor offers every batch that runs the title's vertex
 * program to the back end before transforming it (Nv2aBackend.draw_program,
 * in the project's toolkit patch). gb_d3d11.c passes each offer through the
 * functions here. A batch whose first vertex comes out with w = 1 is flat
 * (menus, HUD, text, films) and is left to the processor path, with
 * everything gb_draw does for flat batches. Any other batch is drawn by the
 * card: its distinct vertices are fetched once, and the interpreter shader
 * (gb_vsh.hlsl, embedded by CMake) runs the title's program for each.
 *
 * A player setting, off by default: GB_VSH_GPU=1. Unset, nothing here runs.
 *
 * Every build checks the card at the first 3D batches of a run: their
 * vertices are captured from the shader (stream output), read back and
 * compared with the processor's interpreter. If any vertex is outside the
 * limits below, the path switches itself off for the run and says so in
 * logs\game.log. A shader that does not compile, a buffer that cannot be
 * made, or device calls that keep failing do the same.
 *
 * Limits (a card may round a division one last bit differently from a
 * processor): a vertex in or near the picture by more than 1/16 of a pixel
 * or 4 steps of the 24-bit depth buffer; one well outside the picture by
 * more than 1e-5 of its size; a colour by more than 1 step of 255; a texture
 * coordinate by more than 1/16 of a texel. Vertices behind the eye or with
 * values that are not finite are clipped by both paths and not compared.
 */
/*
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gb_vsh_gpu.h"
#include "gb_log.h"
#include "gb_vsh_hlsl.h"        /* GB_VSH_HLSL, made by CMake from gb_vsh.hlsl */

#define VP_SLOTS      136u
#define VP_CONSTS     192u
#define VP_VB_BYTES   (24u * 1024u * 1024u)
#define VP_IB_BYTES   (4u * 1024u * 1024u)
#define VP_HASH       16384u               /* a power of two */
#define VP_MAX_IDX    65536u
#define VP_SO_VERTS   65536u               /* the most vertices one compare captures */
#define VP_CHECK_BATCHES 8u                /* the self-check compares at least this many batches, */
#define VP_CHECK_VERTS   2000u             /* and at least this many vertices, */
#define VP_CHECK_MAX     64u               /* but no more batches than this */
#define VP_FAIL_LIMIT 16u                  /* device calls failing in a row before the path is switched off */

typedef struct {                            /* cbuffer VB of gb_vsh.hlsl */
    float    imm[16][4];
    float    tscale[4][4];
    float    pscale[4];
    float    pz[4];
    uint32_t vf[4];
} VpBatch;

typedef struct {                            /* what stream output captures: VO without the clip distances */
    float pos[4], col[4], spec[4], fog, tc[4][4];
} VpOut;

/* What a compare found, over the batches it looked at. */
typedef struct {
    uint64_t batches, verts, skipped, nonfinite, out, bad_out;
    uint64_t bad_pos, bad_z, bad_col, bad_tc, bad_side;
    double max_px, max_z, max_col, max_tc, max_w_rel, max_out;
} VpCompare;

/* The processor's result for one vertex, as a compare keeps it. */
typedef struct {
    Nv2aVertex v;
    Nv2aVertexExt e;
    int ok;
} VpCpu;

/* What a compare needs besides the vertices themselves. */
typedef struct {
    float sx, sy;                           /* 2 / target width, 2 / target height */
    float su[4], sv[4];                     /* each stage's texture coordinate scale */
    double tex_w[4], tex_h[4];              /* each stage's texture size in texels, 1 without one */
    uint32_t len, start;                    /* the program, for the log */
    uint32_t n;                             /* vertices captured */
} VpCtx;


static struct {
    int tried, on;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vb, *ib, *cb_prog, *cb_const, *cb_batch;
    ID3D11GeometryShader *so_gs;
    ID3D11Buffer *so_buf, *so_read;
    uint32_t vb_used, ib_used;
    uint32_t prog_copy[VP_SLOTS][4];
    float const_copy[VP_CONSTS][4];
    int prog_sent, const_sent;
    /* the program in use: which inputs it reads */
    uint32_t rev, start, reads, len;
    int rev_valid, prog_ok;
    /* index remap */
    struct { uint32_t gen, index, slot; } hash[VP_HASH];
    uint32_t gen;
    uint32_t *src;                          /* distinct source indices */
    uint32_t *ridx;                         /* remapped indices, before the topology step */
    VpCpu *cpu;                             /* the processor's results for a compare */
    uint32_t slot_of[16], fetched[16], nf;  /* the batch's fetched attributes */
    /* the self-check */
    uint32_t check_left;                    /* batches the self-check may still compare (0: done) */
    uint32_t check_min;                     /* batches it must compare */
    VpCompare self;
    uint32_t fails;                         /* device calls failed in a row */
} VP;


static ID3DBlob *vp_compile(void)
{
    ID3DBlob *code = NULL, *err = NULL;
    const char *text = GB_VSH_HLSL;
    size_t len = sizeof GB_VSH_HLSL - 1;
    HRESULT hr;

    hr = D3DCompile(text, len, "gb_vsh", NULL, NULL, "vsp", "vs_5_0",
                    D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        const char *msg = err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "";
        gb_log("[VSH-GPU] the graphics card's vertex shader did not compile (0x%08lX): vertex programs stay on"
               " the processor. %.300s\n", (unsigned long)hr, msg);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static void vp_release(void)
{
#define VP_REL(p, kind) do { if (p) { kind##_Release(p); (p) = NULL; } } while (0)
    VP_REL(VP.vs, ID3D11VertexShader);
    VP_REL(VP.layout, ID3D11InputLayout);
    VP_REL(VP.vb, ID3D11Buffer);
    VP_REL(VP.ib, ID3D11Buffer);
    VP_REL(VP.cb_prog, ID3D11Buffer);
    VP_REL(VP.cb_const, ID3D11Buffer);
    VP_REL(VP.cb_batch, ID3D11Buffer);
    VP_REL(VP.so_gs, ID3D11GeometryShader);
    VP_REL(VP.so_buf, ID3D11Buffer);
    VP_REL(VP.so_read, ID3D11Buffer);
#undef VP_REL
}

/* The path off for the rest of the run, with one line in the log saying why. */
static void vp_off(const char *why)
{
    if (!VP.on)
        return;
    VP.on = 0;
    gb_log("[VSH-GPU] %s: vertex programs run on the processor for the rest of this run\n", why);
}

int gb_vsh_gpu_init(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    static const char *const sem = "ATTR";
    static const D3D11_SO_DECLARATION_ENTRY so[] = {
        { 0, "SV_Position", 0, 0, 4, 0 }, { 0, "COLOR", 0, 0, 4, 0 }, { 0, "COLOR", 1, 0, 4, 0 },
        { 0, "FOG", 0, 0, 1, 0 }, { 0, "TEXCOORD", 0, 0, 4, 0 }, { 0, "TEXCOORD", 1, 0, 4, 0 },
        { 0, "TEXCOORD", 2, 0, 4, 0 }, { 0, "TEXCOORD", 3, 0, 4, 0 },
    };
    D3D11_INPUT_ELEMENT_DESC il[16];
    D3D11_BUFFER_DESC bd;
    LARGE_INTEGER f, t0, t1;
    UINT stride = sizeof(VpOut);
    ID3DBlob *b;
    HRESULT hr;
    const char *e;
    int a;

    if (VP.tried)
        return VP.on;
    VP.tried = 1;
    e = getenv("GB_VSH_GPU");
    if (!e || !e[0] || e[0] == '0')
        return 0;
    VP.dev = dev;
    VP.ctx = ctx;
    VP.check_left = VP_CHECK_MAX;
    VP.check_min = VP_CHECK_BATCHES;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (!(b = vp_compile()))
        return 0;
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b),
                                         NULL, &VP.vs);
    for (a = 0; a < 16; a++) {
        memset(&il[a], 0, sizeof il[a]);
        il[a].SemanticName = sem;
        il[a].SemanticIndex = (UINT)a;
        il[a].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[a].InputSlot = (UINT)a;          /* one slot an attribute: its offset in the vertex is set per batch */
        il[a].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    }
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(dev, il, 16, ID3D10Blob_GetBufferPointer(b),
                                            ID3D10Blob_GetBufferSize(b), &VP.layout);
    /* The same shader with its outputs captured, for the self-check. */
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(dev, ID3D10Blob_GetBufferPointer(b),
            ID3D10Blob_GetBufferSize(b), so, 8, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &VP.so_gs);
    ID3D10Blob_Release(b);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VP_SO_VERTS * (UINT)sizeof(VpOut);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &VP.so_buf);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &VP.so_read);
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = VP_VB_BYTES;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &VP.vb);
    bd.ByteWidth = VP_IB_BYTES;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &VP.ib);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof VP.prog_copy;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &VP.cb_prog);
    bd.ByteWidth = sizeof VP.const_copy;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &VP.cb_const);
    bd.ByteWidth = sizeof(VpBatch);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &VP.cb_batch);
    if (SUCCEEDED(hr)) {
        VP.src = (uint32_t *)malloc(VP_MAX_IDX * sizeof *VP.src);
        VP.ridx = (uint32_t *)malloc(VP_MAX_IDX * sizeof *VP.ridx);
        VP.cpu = (VpCpu *)malloc(VP_SO_VERTS * sizeof *VP.cpu);
        if (!VP.src || !VP.ridx || !VP.cpu)
            hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        gb_log("[VSH-GPU] the graphics card's vertex program path could not be set up (0x%08lX): vertex programs"
               " stay on the processor\n", (unsigned long)hr);
        vp_release();
        return 0;
    }
    VP.vb_used = VP_VB_BYTES;               /* the first map discards */
    VP.ib_used = VP_IB_BYTES;
    VP.on = 1;
    QueryPerformanceCounter(&t1);
    gb_log("[VSH-GPU] vertex programs run on the graphics card (shader ready in %.0f ms); checked against the"
           " processor at the first 3D batches\n", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
    return 1;
}

int gb_vsh_gpu_on(void)
{
    return VP.on;
}

/* Which inputs the program reads, from the start slot to its FINAL
 * instruction; 0 when there is none (no program to run). The sources an
 * operation reads are those of the toolkit's interpreter. */
static int vp_scan(const Nv2aProgramBatch *pb)
{
    uint32_t s, reads = 0, n = 0;
    int final = 0;

    for (s = pb->start_slot; s < VP_SLOTS; s++) {
        const uint32_t *ins = pb->program[s];
        uint32_t mac = (ins[1] >> 21) & 15u, ilu = (ins[1] >> 25) & 7u, r = 0, k;
        uint32_t mux[3];
        mux[0] = (ins[2] >> 26) & 3u;
        mux[1] = (ins[2] >> 11) & 3u;
        mux[2] = (ins[3] >> 28) & 3u;
        if (mac) r |= 1;
        if (mac == 2 || (mac >= 4 && mac <= 12)) r |= 2;
        if (mac == 3 || mac == 4 || ilu) r |= 4;
        for (k = 0; k < 3; k++) {
            if (!((r >> k) & 1u))
                continue;
            if (mux[k] == 2)
                reads |= 1u << ((ins[1] >> 9) & 15u);
        }
        n++;
        if (ins[3] & 1u) { final = 1; break; }
    }
    VP.rev = pb->program_revision;
    VP.start = pb->start_slot;
    VP.rev_valid = 1;
    VP.prog_ok = final;
    VP.reads = reads;
    VP.len = n;
    return final;
}

int gb_vsh_gpu_take(const Nv2aProgramBatch *pb, uint32_t frame)
{
    Nv2aVertex v0;
    Nv2aVertexExt e0;

    (void)frame;
    if (!VP.on)
        return 0;
    if (pb->count < 3 || pb->count > VP_MAX_IDX || !pb->ext || !pb->ext->combiner) {
        return 0;
    }
    if (!VP.rev_valid || VP.rev != pb->program_revision || VP.start != pb->start_slot)
        vp_scan(pb);
    if (!VP.prog_ok) {
        return 0;
    }
    /* Flat or not: the first vertex, transformed on the processor exactly as
     * the executor would. w = 1 (rhw = 1): the whole batch goes to gb_draw. */
    if (!nv2a_backend_transform(pb->indices[0], &v0, &e0)) {
        return 0;
    }
    if (v0.rhw == 1.0f) {
        return 0;
    }
    return 1;
}

/* A device call failed: the processor draws this batch, and after
 * VP_FAIL_LIMIT in a row the path goes off. */
static int vp_failed(const char *what, HRESULT hr)
{
    if (++VP.fails >= VP_FAIL_LIMIT) {
        char why[128];
        snprintf(why, sizeof why, "the graphics card's %s failed %u times in a row (0x%08lX)", what,
                 VP.fails, (unsigned long)hr);
        vp_off(why);
    }
    return 0;
}

static HRESULT vp_map(ID3D11Buffer *buf, D3D11_MAP how, D3D11_MAPPED_SUBRESOURCE *map)
{
    return ID3D11DeviceContext_Map(VP.ctx, (ID3D11Resource *)buf, 0, how, 0, map);
}

int gb_vsh_gpu_geometry(const Nv2aProgramBatch *pb, GbVshGeometry *g)
{
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;
    uint32_t i, n = pb->count, nverts = 0, nidx = 0, tris = 0, maxidx, vstride, a;
    uint32_t *ix;
    const uint32_t *r;

    /* Each distinct vertex index gets a slot the first time it appears. */
    if (++VP.gen == 0) {
        memset(VP.hash, 0, sizeof VP.hash);
        VP.gen = 1;
    }
    for (i = 0; i < n; i++) {
        uint32_t id = pb->indices[i], h = (id ^ (id >> 14)) & (VP_HASH - 1u);
        if (VP.hash[h].gen == VP.gen && VP.hash[h].index == id) {
            VP.ridx[i] = VP.hash[h].slot;
        } else {
            VP.hash[h].gen = VP.gen;
            VP.hash[h].index = id;
            VP.hash[h].slot = nverts;
            VP.src[nverts] = id;
            VP.ridx[i] = nverts++;
        }
    }
    /* The attributes the program reads from arrays, one float4 each. */
    VP.nf = 0;
    for (a = 0; a < 16; a++) {
        VP.slot_of[a] = 0;
        if (((VP.reads >> a) & 1u) && ((pb->arrays >> a) & 1u)) {
            VP.slot_of[a] = VP.nf;
            VP.fetched[VP.nf++] = a;
        }
    }
    vstride = (VP.nf ? VP.nf : 1u) * 16u;
    maxidx = n * 3u;                        /* fans and quad strips: three indices a triangle */
    if ((uint64_t)nverts * vstride > VP_VB_BYTES || (uint64_t)maxidx * 4u > VP_IB_BYTES)
        return 0;

    /* indices */
    if (VP.ib_used + maxidx * 4u > VP_IB_BYTES) {
        VP.ib_used = 0;
        hr = vp_map(VP.ib, D3D11_MAP_WRITE_DISCARD, &map);
    } else {
        hr = vp_map(VP.ib, D3D11_MAP_WRITE_NO_OVERWRITE, &map);
    }
    if (FAILED(hr))
        return vp_failed("index buffer", hr);
    g->ibase = VP.ib_used;
    ix = (uint32_t *)((uint8_t *)map.pData + g->ibase);
    r = VP.ridx;
    g->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (pb->prim) {
    case 5:                                 /* triangles */
        nidx = n - n % 3u;
        memcpy(ix, r, nidx * 4u);
        for (i = 0; i + 2 < n; i += 3)
            tris += r[i] != r[i+1] && r[i+1] != r[i+2] && r[i] != r[i+2];
        break;
    case 6:                                 /* strip: as it is */
        g->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        nidx = n;
        memcpy(ix, r, nidx * 4u);
        for (i = 0; i + 2 < n; i++)
            tris += r[i] != r[i+1] && r[i+1] != r[i+2] && r[i] != r[i+2];
        break;
    case 7: case 10:                        /* fan, polygon */
        for (i = 1; i + 1 < n; i++) {
            ix[nidx++] = r[0]; ix[nidx++] = r[i]; ix[nidx++] = r[i+1];
            tris++;
        }
        break;
    case 8:                                 /* quads */
        for (i = 0; i + 3 < n; i += 4) {
            ix[nidx++] = r[i]; ix[nidx++] = r[i+1]; ix[nidx++] = r[i+2];
            ix[nidx++] = r[i]; ix[nidx++] = r[i+2]; ix[nidx++] = r[i+3];
            tris += 2;
        }
        break;
    default:                                /* 9: quad strip */
        for (i = 0; i + 3 < n; i += 2) {
            ix[nidx++] = r[i]; ix[nidx++] = r[i+1]; ix[nidx++] = r[i+3];
            ix[nidx++] = r[i]; ix[nidx++] = r[i+3]; ix[nidx++] = r[i+2];
            tris += 2;
        }
        break;
    }
    ID3D11DeviceContext_Unmap(VP.ctx, (ID3D11Resource *)VP.ib, 0);
    VP.ib_used += nidx * 4u;
    if (!nidx)
        return 0;

    /* vertices: the fetched attributes of each distinct vertex */
    if (VP.vb_used + nverts * vstride > VP_VB_BYTES) {
        VP.vb_used = 0;
        hr = vp_map(VP.vb, D3D11_MAP_WRITE_DISCARD, &map);
    } else {
        hr = vp_map(VP.vb, D3D11_MAP_WRITE_NO_OVERWRITE, &map);
    }
    if (FAILED(hr))
        return vp_failed("vertex buffer", hr);
    g->vbase = VP.vb_used;
    {
        float *o = (float *)((uint8_t *)map.pData + g->vbase);
        uint32_t k, j;
        if (!VP.nf) {
            memset(o, 0, (size_t)nverts * 16u);
        } else {
            for (k = 0; k < nverts; k++)
                for (j = 0; j < VP.nf; j++, o += 4)
                    nv2a_backend_fetch_attr(VP.fetched[j], VP.src[k], o);
        }
    }
    ID3D11DeviceContext_Unmap(VP.ctx, (ID3D11Resource *)VP.vb, 0);
    VP.vb_used += nverts * vstride;
    g->nidx = nidx;
    g->nverts = nverts;
    g->vstride = vstride;
    g->tris = tris;
    return 1;
}

static int vp_upload(ID3D11Buffer *cb, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr = vp_map(cb, D3D11_MAP_WRITE_DISCARD, &map);

    if (FAILED(hr))
        return vp_failed("constant buffer", hr);
    memcpy(map.pData, data, bytes);
    ID3D11DeviceContext_Unmap(VP.ctx, (ID3D11Resource *)cb, 0);
    return 1;
}

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

static void vp_ctx(VpCtx *x, const Nv2aProgramBatch *pb, uint32_t nverts, uint32_t tw, uint32_t th,
                   const float su[4], const float sv[4])
{
    int st;

    x->sx = 2.0f / (float)tw;
    x->sy = 2.0f / (float)th;
    for (st = 0; st < 4; st++) {
        x->su[st] = su[st];
        x->sv[st] = sv[st];
        x->tex_w[st] = pb->ext->stage[st].width ? pb->ext->stage[st].width : 1.0;
        x->tex_h[st] = pb->ext->stage[st].height ? pb->ext->stage[st].height : 1.0;
    }
    x->len = VP.len;
    x->start = VP.start;
    x->n = nverts > VP_SO_VERTS ? VP_SO_VERTS : nverts;   /* the first VP_SO_VERTS of a very large batch */
}

/* The batch's distinct vertices run through the shader with stream output and
 * copied to `read`. The vertex buffer holds them in order, so a point list
 * over them captures each once. */
static void vp_capture(uint32_t n, ID3D11Buffer *read, int whole)
{
    ID3D11Buffer *none = NULL;
    UINT zero = 0;
    D3D11_BOX box = { 0, 0, 0, 0, 1, 1 };

    ID3D11DeviceContext_GSSetShader(VP.ctx, VP.so_gs, NULL, 0);
    ID3D11DeviceContext_SOSetTargets(VP.ctx, 1, &VP.so_buf, &zero);
    ID3D11DeviceContext_IASetPrimitiveTopology(VP.ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_Draw(VP.ctx, n, 0);
    ID3D11DeviceContext_SOSetTargets(VP.ctx, 1, &none, &zero);
    ID3D11DeviceContext_GSSetShader(VP.ctx, NULL, NULL, 0);
    if (whole) {
        ID3D11DeviceContext_CopyResource(VP.ctx, (ID3D11Resource *)read, (ID3D11Resource *)VP.so_buf);
    } else {
        box.right = n * (UINT)sizeof(VpOut);
        ID3D11DeviceContext_CopySubresourceRegion(VP.ctx, (ID3D11Resource *)read, 0, 0, 0, 0,
                                                  (ID3D11Resource *)VP.so_buf, 0, &box);
    }
}

/* The same vertices on the processor, as gb_draw would receive them. */
static void vp_cpu(VpCpu *cpu, uint32_t n)
{
    uint32_t k;

    for (k = 0; k < n; k++)
        cpu[k].ok = nv2a_backend_transform(VP.src[k], &cpu[k].v, &cpu[k].e);
}

/* Compare each card vertex with the processor's by the limits at the top of
 * this file. Returns the number of vertices over the limits. */
static long vp_compare_data(const VpOut *gpu, const VpCpu *cpu, const VpCtx *x, VpCompare *c)
{
    uint64_t bad0 = c->bad_pos + c->bad_z + c->bad_col + c->bad_tc + c->bad_out + c->bad_side;
    float sx = x->sx, sy = x->sy;
    uint32_t k;
    int st;

    c->batches++;
    for (k = 0; k < x->n; k++) {
        const VpOut *o = gpu + k;
        const Nv2aVertex *v = &cpu[k].v;
        const Nv2aVertexExt *e = &cpu[k].e;
        float w, cpos[4], col[4];
        double dpx, dz, dc = 0, dt = 0, gw = o->pos[3];
        int j;

        if (!cpu[k].ok) { c->skipped++; continue; }
        /* gb_draw's own arithmetic */
        w = (v->rhw > 0.0f && v->rhw - v->rhw == 0.0f) ? 1.0f / v->rhw : 1.0f;
        if (w - w != 0.0f) w = 1.0f;
        cpos[0] = (v->x * sx - 1.0f) * w;
        cpos[1] = (1.0f - v->y * sy) * w;
        cpos[2] = v->z * (1.0f / 16777215.0f) * w;
        cpos[3] = w;
        {
            /* In front of the eye and finite, on each side. Where both say no,
             * both paths clip the vertex and it is not compared. Where they
             * disagree, the two paths would clip different triangles: a
             * failure, unless the two w are the same within the limit (a
             * vertex on the clipping plane itself). */
            double cw = v->rhw != 0.0f ? 1.0 / (double)v->rhw : 0.0;   /* the processor's own w */
            int cpu_front = v->rhw > 0.0f && (v->x - v->x) + (v->y - v->y) + (v->z - v->z) == 0.0f && cw > 0.001;
            int gpu_front = gw > 0.001 && gw - gw == 0.0 && o->pos[0] - o->pos[0] == 0.0f
                            && o->pos[1] - o->pos[1] == 0.0f && o->pos[2] - o->pos[2] == 0.0f;
            if (!cpu_front || !gpu_front) {
                if (cpu_front != gpu_front && !(fabs(gw - cw) <= 1e-5 * fabs(cw))) {
                    c->bad_side++;
                } else {
                    c->nonfinite++;         /* behind the eye or not finite on both: both paths clip, not compared */
                }
                continue;
            }
        }
        {
            /* A vertex well outside the picture (more than one screen away, or
             * outside the depth range) only steers the edges of triangles the
             * card clips: compared in clip space, relative to its size. One in
             * or near the picture: in pixels and in steps of the depth buffer. */
            double nx = (double)o->pos[0] / gw, ny = (double)o->pos[1] / gw, nz = (double)o->pos[2] / gw;
            if (fabs(nx) > 3.0 || fabs(ny) > 3.0 || nz < 0.0 || nz > 1.0) {
                double rel = 0;
                for (j = 0; j < 3; j++) {
                    double m = fabs((double)o->pos[j]) > fabs((double)cpos[j]) ? fabs((double)o->pos[j]) : fabs((double)cpos[j]);
                    double d = fabs((double)o->pos[j] - (double)cpos[j]) / (m > gw ? m : gw);
                    if (d > rel) rel = d;
                }
                c->out++;
                if (rel > c->max_out) c->max_out = rel;
                if (rel > 1e-5) c->bad_out++;
                continue;
            }
        }
        c->verts++;
        {
            double gx = ((double)o->pos[0] / gw + 1.0) / sx, gy = (1.0 - (double)o->pos[1] / gw) / sy;
            double cx = ((double)cpos[0] / w + 1.0) / sx, cy = (1.0 - (double)cpos[1] / w) / sy;
            double ex = fabs(gx - cx), ey = fabs(gy - cy);
            dpx = ex > ey ? ex : ey;
            dz = fabs((double)o->pos[2] / gw - (double)cpos[2] / w) * 16777215.0;
        }
        unpack_argb(v->diffuse, col);
        for (j = 0; j < 4; j++) {
            double d = fabs((double)o->col[j] - col[j]) * 255.0, d2 = fabs((double)o->spec[j] - e->specular[j]) * 255.0;
            if (d > dc) dc = d;
            if (d2 > dc) dc = d2;
        }
        {
            double d = fabs((double)o->fog - e->fog) * 255.0;
            if (d > dc) dc = d;
        }
        for (st = 0; st < 4; st++) {
            /* in texels where the stage has a texture, else in the coordinate's own units */
            double d0 = fabs((double)o->tc[st][0] - (double)(e->tex[st][0] * x->su[st])) * x->tex_w[st];
            double d1 = fabs((double)o->tc[st][1] - (double)(e->tex[st][1] * x->sv[st])) * x->tex_h[st];
            double d2 = fabs((double)o->tc[st][2] - e->tex[st][2]), d3 = fabs((double)o->tc[st][3] - e->tex[st][3]);
            if (d0 == d0 && d0 > dt) dt = d0;
            if (d1 == d1 && d1 > dt) dt = d1;
            if (d2 == d2 && d2 > dt) dt = d2;
            if (d3 == d3 && d3 > dt) dt = d3;
        }
        if (dpx > c->max_px) c->max_px = dpx;
        if (dz > c->max_z) c->max_z = dz;
        if (dc > c->max_col) c->max_col = dc;
        if (dt > c->max_tc) c->max_tc = dt;
        {
            double rw = fabs(gw - w) / w;
            if (rw > c->max_w_rel) c->max_w_rel = rw;
        }
        if (dpx > 1.0 / 16.0) c->bad_pos++;
        if (dz > 4.0) c->bad_z++;
        if (dc > 1.0) c->bad_col++;
        if (dt > 1.0 / 16.0) c->bad_tc++;
    }
    return (long)(c->bad_pos + c->bad_z + c->bad_col + c->bad_tc + c->bad_out + c->bad_side - bad0);
}

/* The self-check's compare: capture, wait for the card, compare. Returns the
 * number of vertices over the limits, or -1 when the read-back failed. */
static long vp_compare(const Nv2aProgramBatch *pb, const GbVshGeometry *g, uint32_t tw, uint32_t th,
                       const float su[4], const float sv[4], VpCompare *c)
{
    D3D11_MAPPED_SUBRESOURCE map;
    VpCtx x;
    long bad;

    vp_ctx(&x, pb, g->nverts, tw, th, su, sv);
    vp_capture(x.n, VP.so_read, 1);
    vp_cpu(VP.cpu, x.n);
    if (FAILED(ID3D11DeviceContext_Map(VP.ctx, (ID3D11Resource *)VP.so_read, 0, D3D11_MAP_READ, 0, &map)))
        return -1;
    bad = vp_compare_data((const VpOut *)map.pData, VP.cpu, &x, c);
    ID3D11DeviceContext_Unmap(VP.ctx, (ID3D11Resource *)VP.so_read, 0);
    return bad;
}


int gb_vsh_gpu_bind(const Nv2aProgramBatch *pb, const GbVshGeometry *g, uint32_t tw, uint32_t th,
                    const float su[4], const float sv[4])
{
    ID3D11Buffer *bufs[16], *cbs[3];
    UINT strides[16], offsets[16];
    VpBatch vb;
    int a, st;

    /* the program, the constants, and this batch's values */
    if (!VP.prog_sent || memcmp(VP.prog_copy, pb->program, sizeof VP.prog_copy) != 0) {
        memcpy(VP.prog_copy, pb->program, sizeof VP.prog_copy);
        if (!(VP.prog_sent = vp_upload(VP.cb_prog, VP.prog_copy, sizeof VP.prog_copy)))
            return 0;
    }
    if (!VP.const_sent || memcmp(VP.const_copy, pb->constants, sizeof VP.const_copy) != 0) {
        memcpy(VP.const_copy, pb->constants, sizeof VP.const_copy);
        if (!(VP.const_sent = vp_upload(VP.cb_const, VP.const_copy, sizeof VP.const_copy)))
            return 0;
    }
    memset(&vb, 0, sizeof vb);
    memcpy(vb.imm, pb->current, sizeof vb.imm);
    for (st = 0; st < 4; st++) {
        vb.tscale[st][0] = su[st];
        vb.tscale[st][1] = sv[st];
    }
    vb.pscale[0] = pb->aa_sx;
    vb.pscale[1] = pb->aa_sy;
    vb.pscale[2] = 2.0f / (float)tw;
    vb.pscale[3] = 2.0f / (float)th;
    vb.pz[0] = 1.0f / 16777215.0f;
    vb.pz[1] = pb->constants[59][2];
    vb.pz[2] = pb->fog_param[0];
    vb.pz[3] = pb->fog_param[1];
    vb.vf[0] = pb->start_slot;
    vb.vf[1] = pb->arrays;
    vb.vf[2] = pb->fog_enable ? 1u : 0u;
    vb.vf[3] = pb->fog_mode;
    if (!vp_upload(VP.cb_batch, &vb, sizeof vb))
        return 0;
    VP.fails = 0;

    for (a = 0; a < 16; a++) {
        bufs[a] = VP.vb;
        strides[a] = g->vstride;
        offsets[a] = g->vbase + VP.slot_of[a] * 16u;
    }
    cbs[0] = VP.cb_prog; cbs[1] = VP.cb_const; cbs[2] = VP.cb_batch;
    ID3D11DeviceContext_IASetInputLayout(VP.ctx, VP.layout);
    ID3D11DeviceContext_IASetVertexBuffers(VP.ctx, 0, 16, bufs, strides, offsets);
    ID3D11DeviceContext_IASetIndexBuffer(VP.ctx, VP.ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShader(VP.ctx, VP.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(VP.ctx, 1, 3, cbs);

    /* The self-check: the first batches of the run, before the card draws
     * them. The vertex buffer holds the distinct vertices in order, so a
     * point list over them captures each once. */
    if (VP.check_left) {
        long bad = vp_compare(pb, g, tw, th, su, sv, &VP.self);
        char why[384];
        if (bad != 0) {
            if (bad < 0)
                snprintf(why, sizeof why, "the graphics card's results could not be read back for the check");
            else
                snprintf(why, sizeof why, "the graphics card's vertex results differ from the processor's"
                         " (%ld of %llu vertices over the limits: position %llu, depth %llu, colour %llu,"
                         " texture coordinate %llu, well outside the picture %llu, in front of the eye on one"
                         " side only %llu)", bad, (unsigned long long)(VP.self.verts + VP.self.out),
                         (unsigned long long)VP.self.bad_pos, (unsigned long long)VP.self.bad_z,
                         (unsigned long long)VP.self.bad_col, (unsigned long long)VP.self.bad_tc,
                         (unsigned long long)VP.self.bad_out, (unsigned long long)VP.self.bad_side);
            vp_off(why);
            return 0;
        }
        if (--VP.check_left == 0
            || (VP.self.batches >= VP.check_min && VP.self.verts + VP.self.out >= VP_CHECK_VERTS)) {
            VP.check_left = 0;
            gb_log("[VSH-GPU] checked against the processor: %llu vertices in %llu batches agree (largest"
                   " difference: position %.5f px, depth %.2f steps)\n",
                   (unsigned long long)(VP.self.verts + VP.self.out), (unsigned long long)VP.self.batches,
                   VP.self.max_px, VP.self.max_z);
        }
    }
    return 1;
}

void gb_vsh_gpu_draw(const GbVshGeometry *g)
{
    ID3D11DeviceContext_IASetPrimitiveTopology(VP.ctx, (D3D11_PRIMITIVE_TOPOLOGY)g->topology);
    ID3D11DeviceContext_DrawIndexed(VP.ctx, g->nidx, g->ibase / 4u, 0);
}


void gb_vsh_gpu_frame(uint32_t frame)
{
    (void)frame;
    if (!VP.on)
        return;
}
