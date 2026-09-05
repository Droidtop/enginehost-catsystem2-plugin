/*
 * Text, drawn with the game's own font.
 *
 * Retail CatSystem2 releases ship the face named in startup.xml beside their
 * archives, and the English ones map punctuation through it, so the text is
 * only right when that font is the one drawn with. Glyphs are rasterised with
 * stb_truetype, which is one vendored header, so the engine needs no font
 * library and the Android wrapper needs nothing but this.
 */
#ifndef CS2_FONT_H
#define CS2_FONT_H

#include "cs2.h"

typedef struct cs2_font cs2_font;

/* Opens a TrueType or OpenType file at a pixel height. NULL when it will not open. */
cs2_font *cs2_font_open(const char *path, int pixel_height);
void cs2_font_free(cs2_font *font);

int cs2_font_height(const cs2_font *font);

/* How wide a UTF-8 string would be drawn. */
int cs2_font_measure(const cs2_font *font, const char *text);

/*
 * Draws UTF-8 text into a canvas of width * height 0xAARRGGBB pixels, with x
 * and y at the left of the baseline's line box. Returns the height used.
 */
int cs2_font_draw(const cs2_font *font, uint32_t *canvas, int width, int height,
                  int x, int y, const char *text, uint32_t colour);

#endif
