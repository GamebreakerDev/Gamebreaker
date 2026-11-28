# Patches to xboxrecomp

Gamebreaker builds against [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
(MIT, sp00nznet and contributors) at commit `343b6d8` (v0.17.0), cloned next
to `port\`. The clone is not part of this repository. The few changes this
project needs inside the toolkit are kept here as patch files, so that anyone
can see exactly what differs and rebuild from a clean clone.

`apply.ps1` applies them (and is run by `port\regen.ps1`). It is safe to run
twice.

Each patch is a candidate to send upstream. None has been sent yet: that is
the maintainer's decision.

## 0001-backend-batch-ext.patch

All the files it touches: `src/kernel/nv2a_backend.h`, `nv2a_pb_exec.c`,
`nv2a_vsh_interp.c`, `nv2a_vsh_interp.h`, `xbox_memory_layout.c` (the C
runtime), and `tools/disasm/engine.py`, `tools/disasm/loader.py` (the
disassembler, since 0.1.3). The parts are described one by one below.

The first part: `src/kernel/nv2a_backend.h`, `src/kernel/nv2a_pb_exec.c`. 114 lines
added and 20 removed (the patch file is 242 lines), additive apart from the
level count below: a render back end that does not read the new fields
behaves exactly as before, and nothing changes when no back end is registered.

What it adds to the render back-end interface:

- `Nv2aBatch.ext` (`Nv2aBatchExt`): the textures of all four stages, which of
  them are usable, the register-combiner state the title programmed
  (`Nv2aCombiner`, already a public toolkit type), the fog colour and the
  clip-plane modes.
- `Nv2aBatchExt.vertices` (`Nv2aVertexExt`): for each vertex, the texture
  coordinates of the four stages, the specular colour and the fog factor that
  the vertex program produced.
- `Nv2aTexture.palette`: the address of a palettised texture's palette, which
  `nv2a_backend_decode_texture` now uses (it was hard-wired to "none").
- `Nv2aRenderState.window_clip_*` (18 lines, added 2026-11-17): rectangle 0 of
  the title's `SET_WINDOW_CLIP_*` methods and its type, in surface pixels. The
  executor already kept these methods in its register mirror and counted them
  as unhandled; it now reports them to a back end. The toolkit's CPU rasteriser
  does not apply the clip.

What it corrects (one line, added 2026-11-16): `Nv2aTexture.levels`, the
texture's mip-level count. The toolkit reported one level for any texture
whose stage still held a pitch (`levels = pitch ? 1 : levels`). The pitch
register is written only for linear textures and never cleared, so after the
first linear texture every swizzled one on that stage lost its levels (in a
match, every stage-0 texture: 1 reported, 4 in the format word). The line now
asks the format instead: swizzled and block-compressed formats report the
format word's count, linear ones 1 (`tex_size_from_format`, already in the
toolkit). Checked in a match against the back end's own check of the levels
in memory: 226 of 226 textures agree (82 with 4 levels, 144 with 1). The
back end's mip levels and anisotropic filtering depend on this count.

Why: the stock interface hands a back end texture stage 0 and the diffuse
colour only. In a match, 109 of 215 draw batches use two texture stages and a
three-stage combiner, and the menus draw their button icons from palettised
textures. Without this the Direct3D 11 back end (`port\src\gb_d3d11.c`) could
not draw either correctly; the toolkit's own header says to "extend Nv2aBatch
when that matters". The back end exists for speed: on the toolkit's CPU
rasteriser a match runs at 8 to 10 frames a second; on Direct3D 11 it holds
60.

### Four vertices at once (added for 0.1.3)

Files: `src/kernel/nv2a_vsh_interp.c`, `nv2a_vsh_interp.h`,
`nv2a_pb_exec.c`. `nv2a_vsh_run4` runs the decoded vertex program on four
vertices together with SSE2 (every x86-64 processor has it; nothing else is
required), each register held as four vectors, one per component. The
executor's batch loop now lists the slots to transform, runs them four at a
time, then fills the repeat-vertex cache's copies. Every step is the same
IEEE operation in the same order as the one-at-a-time interpreter, so the
outputs are the same; the one exception that can occur is the sign or payload
of a NaN when both operands of an add or multiply are NaNs (x86 returns the
first operand's, and a compiler may swap either code's operands), which
nothing downstream can tell apart. A program allowed to write constants back
still runs one vertex at a time.

Switches (the toolkit's own style, environment variables):
`RECOMP_VSH_SIMD=0` the old path; `RECOMP_VSH_SIMD=check` runs both on every
vertex and logs `[VSH-SIMD]` totals with each report; `RECOMP_VSH_TIME=1` logs
`[VSH-TIME]`, the time program batches take to transform. The test
`port\tests\build_vsh_simd_test.ps1` compares both paths on random programs.

### Vertex programs offered to the back end (added for 0.1.3)

Files: `src/kernel/nv2a_backend.h`, `nv2a_pb_exec.c`, `nv2a_vsh_interp.c`,
`nv2a_vsh_interp.h`. 137 lines added, none removed or changed. One optional
member at the end of `Nv2aBackend`, `draw_program`, and one struct,
`Nv2aProgramBatch`: before the executor transforms a batch that runs the
title's vertex program, it offers the batch (indices, which attributes come
from arrays, the current attribute values, program memory and its start slot,
the 192 constants, the surface scale, the fog unit, render and pixel state)
to a back end that set the member. If the back end returns 1 it has drawn the
batch itself; if 0, or if the member is NULL, the executor carries on exactly
as before. Not offered: a program allowed to write constants back, points and
lines. Two helpers for use during the call, `nv2a_backend_fetch_attr` (one
attribute of one vertex, as the executor's interpreter reads it) and
`nv2a_backend_transform` (one vertex run on the CPU, as the executor would
hand it to `draw`), and one read-only accessor, `nv2a_vsh_program`. A back end
that leaves `draw_program` NULL, such as the toolkit's CPU rasteriser, behaves
exactly as without this.

Why: `port\src\gb_vsh_gpu.c` runs the program on the graphics card for 3D
batches (the player setting `GB_VSH_GPU=1`), which takes most of the
executor's work away. The executor is the one thread that limits the frame
rate.

### A faster disassembly (added for 0.1.3)

Files: `tools/disasm/loader.py`, `tools/disasm/engine.py`. 33 lines added,
none removed or changed. Two results are kept instead of worked out again:
`BinaryImage.get_section_data` makes each section's bytes once (a disassembly
asks for a section about 160,000 times, and copying 2.7 MB each time was most
of the step), and `DisasmEngine.block_tail_jump` answers each address once
(the function detector asks again for the same addresses). Each kept result
depends only on what it is kept for: the section's bytes are an immutable
`bytes` object read once from the file (an image whose bytes are not `bytes`
is not memoised), and a tail jump only on the image's bytes and the two
arguments; the memo is dropped when an engine is given another image.

Measured on this title: the disassembly takes about 61 s instead of about
115 s, and every output (the five JSON files, the `.asm` listings and the
lift made from them) is byte for byte the same. The toolkit's own
disassembler tests (`tools/disasm/test_*.py`, 153 cases) give the same
results with and without it.

Why: a player's first setup and every update that lifts again spend most
of their time in the disassembly, on one processor core.

