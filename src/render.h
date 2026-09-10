/*
 * Draws what the scene player leaves behind onto one canvas.
 *
 * The game is authored for a fixed virtual screen - 1024x576 for Labyrinth of
 * Grisaia - so everything is composed at that size and whoever shows it scales
 * it. The pictures come out of the run's own store of decoded ones, which is
 * shared with the front end's layouts: a screen is the same few pictures over
 * and over and decoding one twice is the whole of what made this slow.
 *
 * Text is drawn with the game's own font, which retail releases ship beside
 * their archives: the English ones map punctuation through it, so the text is
 * only right when that font is the one drawn with.
 */
#ifndef CS2_RENDER_H
#define CS2_RENDER_H

#include "pictures.h"
#include "scene.h"

typedef struct cs2_render cs2_render;

/* The files and the pictures must outlive the renderer. Works with no font,
   drawing no text. */
cs2_render *cs2_render_new(cs2_files *files, cs2_pictures *pictures, int width, int height);
void cs2_render_free(cs2_render *render);

int cs2_render_width(const cs2_render *render);
int cs2_render_height(const cs2_render *render);

/*
 * Composes one frame. The canvas belongs to the renderer and is valid until the
 * next call. status may be NULL; when it is not, it is drawn along the top.
 */
const uint32_t *cs2_render_frame(cs2_render *render, const cs2_scene *scene, const char *status);

#endif
