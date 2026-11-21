/* Textures by fingerprint (gb_textures.c): pictures in a mod's textures
 * folder drawn in place of the game's textures they name, and the maker's
 * dump that writes each texture the game uses under its fingerprint. Only
 * the Direct3D 11 back end (gb_d3d11.c) calls these, on its own thread. */
#ifndef GB_TEXTURES_H
#define GB_TEXTURES_H

#include <stdint.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

/* After the device exists. Returns 1 (Ctrl+F9 can switch the dump on at
 * any time, so the back end always works out fingerprints). */
int gb_tex_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx);

/* The published fingerprint of a texture: level0_hash is the back end's
 * hash of the level-0 bytes as they are in the game's memory (hash_bytes),
 * palette_hash its hash of the palette (0 for formats without one). */
uint64_t gb_tex_fingerprint(uint64_t level0_hash, uint32_t width, uint32_t height,
                            uint32_t color, uint64_t palette_hash);

/* The picture that replaces fp, with a reference the caller releases, or
 * NULL. *levels: its mip levels. color: the game's format, so a picture for
 * a format without alpha is drawn opaque. */
struct ID3D11ShaderResourceView *gb_tex_replacement(uint64_t fp, uint32_t color, uint32_t *levels);

/* Dump: 1 when fp should be written (dump on, not written yet this run).
 * gb_tex_dump copies bgra (width x height, 0xAARRGGBB) and writes it later
 * on its own thread. */
int  gb_tex_dump_wanted(uint64_t fp);
void gb_tex_dump(uint64_t fp, uint32_t width, uint32_t height, uint32_t color, uint32_t levels,
                 const uint32_t *bgra, uint32_t frame, int scene3d);

/* Ctrl+F9 in the game: switches the dump on or off. Returns 1 now on, 0 now
 * off, -1 when it cannot be switched on (no folder). Any thread. */
int  gb_tex_dump_toggle(void);
int  gb_tex_dump_ready(void);                   /* 1 when the dump's folder is known */

/* Once a frame: frees pictures unused for a while. */
void gb_tex_frame(uint32_t frame);

#endif
