/*
 * What the port draws over the game's picture in the Direct3D 11 back end:
 * the project's start screen and corner mark, the frame counter and the
 * screenshot key. See gb_overlay.c.
 */
#ifndef GB_OVERLAY_H
#define GB_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct IDXGISwapChain;

/* Called by the back end once per shown picture, after the game's picture is
 * drawn into the window's buffer and before it is presented. win_w, win_h:
 * the buffer's size; vx, vy, vw, vh: the rectangle the game's picture fills;
 * verts3d: vertices the game drew in 3D batches this frame. */
void gb_overlay_frame(struct ID3D11DeviceContext *ctx, struct ID3D11RenderTargetView *rtv,
                      struct IDXGISwapChain *swap, int win_w, int win_h,
                      float vx, float vy, float vw, float vh, unsigned verts3d);

/* Release every Direct3D object the overlay holds (device lost, shutdown). */
void gb_overlay_release(void);

/* The movie player's update (recomp_manual.c) reports the file it is playing,
 * e.g. "xlp0005.nfx"; the overlay uses it to know which screen is up. */
void gb_overlay_note_movie(const char *base);

/* Milliseconds of the start screen still to show; 0 once it has finished or
 * when it never started (no Direct3D 11 window). */
unsigned gb_overlay_start_left_ms(void);

/* F12: save the next picture to the screenshots folder. */
void gb_overlay_request_shot(void);

#ifdef __cplusplus
}
#endif

#endif
