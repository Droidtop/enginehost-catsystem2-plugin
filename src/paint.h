/*
 * Putting pixels on the canvas: the one place this engine blends.
 *
 * Three places drew the same way with three copies of the same loop - the scene
 * being composed, the front end's layouts and the planes - and all three paid
 * for it twice over: a bounds test on every pixel, and three divisions by 255
 * to mix each one. A frame is six hundred thousand pixels and a fade covers all
 * of them, so that is the whole of what composing costs once the pictures are
 * decoded.
 *
 * So it is written once, here: the rectangle is clipped before the loop rather
 * than inside it, and the division is the exact same answer reached by adding
 * and shifting, which is what it costs on a handheld's processor. Byte for byte
 * the canvas comes out as it did: the four standing regressions are the proof
 * of that, and they are why this is an optimisation rather than a rewrite.
 *
 * Everything is 0xAARRGGBB, and the canvas is left opaque.
 */
#ifndef CS2_PAINT_H
#define CS2_PAINT_H

#include <stddef.h>
#include <stdint.h>

/*
 * A block of pixels at x,y, each mixed by its own alpha scaled by this one
 * (255 for "as it is"). Pixels outside the canvas are not drawn.
 */
void cs2_paint_pixels(uint32_t *canvas, int width, int height, int x, int y,
                      const uint32_t *pixels, int pixels_width, int pixels_height, int alpha);

/* A rectangle of one colour, mixed by that colour's own alpha. */
void cs2_paint_fill(uint32_t *canvas, int width, int height, int x, int y,
                    int across, int down, uint32_t colour);

#endif
