#include "paint.h"

/*
 * A byte out of a product of bytes: the same answer as dividing by 255, without
 * dividing. Exact for everything a blend can produce, which is 0 to 255 times
 * 255; the engine's own check of that is in the history of this file.
 */
static inline uint32_t over_255(uint32_t value) {
    return (value + (value >> 8) + 1) >> 8;
}

static inline uint32_t mix(uint32_t under, uint32_t over, uint32_t alpha) {
    uint32_t inverse = 255u - alpha;
    uint32_t red = over_255(((over >> 16) & 0xffu) * alpha + ((under >> 16) & 0xffu) * inverse);
    uint32_t green = over_255(((over >> 8) & 0xffu) * alpha + ((under >> 8) & 0xffu) * inverse);
    uint32_t blue = over_255((over & 0xffu) * alpha + (under & 0xffu) * inverse);
    return 0xff000000u | (red << 16) | (green << 8) | blue;
}

void cs2_paint_pixels(uint32_t *canvas, int width, int height, int x, int y,
                      const uint32_t *pixels, int pixels_width, int pixels_height, int alpha) {
    if (canvas == NULL || pixels == NULL || alpha <= 0) return;
    if (pixels_width <= 0 || pixels_height <= 0) return;
    /* Clipped once: the rows and columns of the picture that land on the canvas. */
    int from_row = y < 0 ? -y : 0;
    int to_row = height - y < pixels_height ? height - y : pixels_height;
    int from_column = x < 0 ? -x : 0;
    int to_column = width - x < pixels_width ? width - x : pixels_width;
    if (from_row >= to_row || from_column >= to_column) return;
    uint32_t scale = (uint32_t) (alpha > 255 ? 255 : alpha);
    for (int row = from_row; row < to_row; row++) {
        const uint32_t *from = pixels + (size_t) row * pixels_width;
        uint32_t *to = canvas + (size_t) (y + row) * width + x;
        for (int column = from_column; column < to_column; column++) {
            uint32_t over = from[column];
            uint32_t alpha_of_it = over >> 24;
            if (scale != 255u) alpha_of_it = over_255(alpha_of_it * scale);
            if (alpha_of_it == 0) continue;
            if (alpha_of_it == 255u) {
                to[column] = 0xff000000u | (over & 0x00ffffffu);
                continue;
            }
            to[column] = mix(to[column], over, alpha_of_it);
        }
    }
}

void cs2_paint_fill(uint32_t *canvas, int width, int height, int x, int y,
                    int across, int down, uint32_t colour) {
    if (canvas == NULL || across <= 0 || down <= 0) return;
    uint32_t alpha = colour >> 24;
    if (alpha == 0) return;
    int from_row = y < 0 ? 0 : y;
    int to_row = y + down > height ? height : y + down;
    int from_column = x < 0 ? 0 : x;
    int to_column = x + across > width ? width : x + across;
    if (from_row >= to_row || from_column >= to_column) return;
    uint32_t solid = 0xff000000u | (colour & 0x00ffffffu);
    for (int row = from_row; row < to_row; row++) {
        uint32_t *to = canvas + (size_t) row * width;
        for (int column = from_column; column < to_column; column++) {
            to[column] = alpha == 255u ? solid : mix(to[column], colour, alpha);
        }
    }
}
