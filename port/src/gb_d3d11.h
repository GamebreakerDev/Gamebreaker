/*
 * Direct3D 11 render back end for the toolkit's pushbuffer executor.
 * See gb_d3d11.c.
 */
#ifndef GB_D3D11_H
#define GB_D3D11_H

/* Register the back end with the executor (nv2a_backend.h). Call before the
 * title starts. The window and device are created on the first clear or draw.
 * Returns 1 if registered. */
int gb_d3d11_install(void);

/* The guest address the executor writes the fences it has reached to. */
void gb_d3d11_fence_probe(unsigned int guest_va);

/* GB_ASPECT (widescreen; recomp_manual.c): the aspect the game's 3D view
 * should have (4/3 when off), and the mode: 0 off, 1 wider view, 2 the
 * game's own 16:9 mode. Both are 4:3 / 0 without the Direct3D 11 back end. */
double gb_d3d11_aspect(void);
int gb_d3d11_aspect_mode(void);
/* With a wide aspect: how 2D is drawn (0 kept at 4:3, 1 GB_ASPECT_2D=auto,
 * 2 stretched); and a 3D view the game has just set up, 1 a menu panel's
 * (Create A Baller's player), 0 any other. */
int gb_d3d11_aspect_2d(void);
void gb_d3d11_view_set(int panel);

/* Present the last picture again with the overlay over it, from any thread
 * (the start screen's frames while the game's thread is held). Returns 1 if a
 * picture was shown. */
int gb_d3d11_reflip(void);

#endif
