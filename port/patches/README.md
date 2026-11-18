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

Files: `src/kernel/nv2a_backend.h`, `src/kernel/nv2a_pb_exec.c`. 114 lines
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

