/*
 * gb_vsh_gpu.h - the title's vertex programs run on the graphics card
 * (GB_VSH_GPU=1). See gb_vsh_gpu.c. Used by gb_d3d11.c only.
 */
#ifndef GB_VSH_GPU_H
#define GB_VSH_GPU_H

#include <stdint.h>
#include "nv2a_backend.h"   /* toolkit, src/kernel */

struct ID3D11Device;
struct ID3D11DeviceContext;

/* The geometry of one batch, as gb_vsh_gpu_geometry uploaded it. */
typedef struct {
    uint32_t topology;       /* a D3D11_PRIMITIVE_TOPOLOGY */
    uint32_t nidx;           /* indices to draw */
    uint32_t ibase;          /* first index in the index buffer */
    uint32_t nverts;         /* distinct vertices */
    uint32_t vbase;          /* byte offset of the batch's vertices */
    uint32_t vstride;        /* bytes a vertex */
    uint32_t tris;           /* triangles, leaving out those that name a vertex twice */
} GbVshGeometry;

/* Once, with the device. Reads GB_VSH_GPU; with it unset, does nothing and
 * returns 0. With it set, compiles the shader and makes the buffers; on any
 * failure says so in the log and returns 0 (the processor path stays in use). */
int gb_vsh_gpu_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx);

/* 1 while the path is in use: set up, and not switched off since. */
int gb_vsh_gpu_on(void);

/* Should the card draw this batch? 0 for a flat batch (its first vertex
 * comes out with w = 1: menus, HUD, text, films), for a program the shader
 * cannot take, and when the path is off. `frame`: the back end's frame
 * number (for the diagnostic switches). */
int gb_vsh_gpu_take(const Nv2aProgramBatch *pb, uint32_t frame);

/* The batch's indices and the attributes its program reads, into the
 * shader's buffers. 0 on failure: nothing is bound, and the processor path
 * draws the batch. */
int gb_vsh_gpu_geometry(const Nv2aProgramBatch *pb, GbVshGeometry *g);

/* The program, the constants and this batch's values; binds the input
 * layout, the buffers and the vertex shader. `tw`, `th`: the target's size in
 * the game's pixels; `su`, `sv`: each stage's texture coordinate scale. For
 * the first 3D batches of a run it first runs the self-check: if the card's
 * results differ from the processor's beyond the limits, the path switches
 * itself off for the run and this returns 0. */
int gb_vsh_gpu_bind(const Nv2aProgramBatch *pb, const GbVshGeometry *g, uint32_t tw, uint32_t th,
                    const float su[4], const float sv[4]);

/* Draw what gb_vsh_gpu_bind bound. */
void gb_vsh_gpu_draw(const GbVshGeometry *g);

/* Once a frame, from the back end's flip. */
void gb_vsh_gpu_frame(uint32_t frame);


#endif
