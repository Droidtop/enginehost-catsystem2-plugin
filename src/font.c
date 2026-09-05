#include "font.h"

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
/* The header is one file offering far more than glyph rasterising; the parts
   this engine does not call are not a defect in it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define MAX_FONT (32u * 1024 * 1024)

struct cs2_font {
    cs2_bytes file;
    stbtt_fontinfo info;
    float scale;
    int ascent;
    int descent;
    int line_gap;
};

cs2_font *cs2_font_open(const char *path, int pixel_height) {
    if (pixel_height <= 0 || pixel_height > 512) return NULL;
    cs2_font *font = calloc(1, sizeof *font);
    if (font == NULL) return NULL;
    if (cs2_read_file(path, MAX_FONT, &font->file) != 0) {
        free(font);
        return NULL;
    }
    int offset = stbtt_GetFontOffsetForIndex(font->file.data, 0);
    if (offset < 0 || !stbtt_InitFont(&font->info, font->file.data, offset)) {
        cs2_bytes_free(&font->file);
        free(font);
        cs2_set_error("%s is not a font this engine can read", path);
        return NULL;
    }
    font->scale = stbtt_ScaleForPixelHeight(&font->info, (float) pixel_height);
    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->line_gap);
    return font;
}

void cs2_font_free(cs2_font *font) {
    if (font == NULL) return;
    cs2_bytes_free(&font->file);
    free(font);
}

int cs2_font_height(const cs2_font *font) {
    if (font == NULL) return 0;
    return (int) ((font->ascent - font->descent + font->line_gap) * font->scale + 0.5f);
}

/* One code point out of a UTF-8 string, and how many bytes it took. */
static unsigned decode(const char *text, size_t *at) {
    unsigned char first = (unsigned char) text[*at];
    if (first < 0x80) {
        (*at)++;
        return first;
    }
    unsigned code;
    int extra;
    if ((first & 0xe0) == 0xc0) {
        code = first & 0x1fu;
        extra = 1;
    } else if ((first & 0xf0) == 0xe0) {
        code = first & 0x0fu;
        extra = 2;
    } else if ((first & 0xf8) == 0xf0) {
        code = first & 0x07u;
        extra = 3;
    } else {
        (*at)++;
        return 0xfffd;
    }
    for (int i = 1; i <= extra; i++) {
        unsigned char next = (unsigned char) text[*at + (size_t) i];
        if ((next & 0xc0) != 0x80) {
            (*at)++;
            return 0xfffd;
        }
        code = (code << 6) | (next & 0x3fu);
    }
    *at += (size_t) extra + 1;
    return code;
}

int cs2_font_measure(const cs2_font *font, const char *text) {
    if (font == NULL || text == NULL) return 0;
    float width = 0;
    unsigned previous = 0;
    for (size_t at = 0; text[at] != '\0'; ) {
        unsigned code = decode(text, &at);
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(&font->info, (int) code, &advance, &bearing);
        if (previous != 0) {
            width += font->scale * stbtt_GetCodepointKernAdvance(&font->info, (int) previous, (int) code);
        }
        width += font->scale * (float) advance;
        previous = code;
    }
    return (int) (width + 0.5f);
}

int cs2_font_draw(const cs2_font *font, uint32_t *canvas, int width, int height,
                  int x, int y, const char *text, uint32_t colour) {
    if (font == NULL || text == NULL) return 0;
    int line_height = cs2_font_height(font);
    int baseline = y + (int) (font->ascent * font->scale + 0.5f);
    unsigned red = (colour >> 16) & 0xff;
    unsigned green = (colour >> 8) & 0xff;
    unsigned blue = colour & 0xff;
    float pen = (float) x;
    unsigned previous = 0;

    for (size_t at = 0; text[at] != '\0'; ) {
        unsigned code = decode(text, &at);
        if (previous != 0) {
            pen += font->scale * stbtt_GetCodepointKernAdvance(&font->info, (int) previous, (int) code);
        }
        int glyph_width = 0;
        int glyph_height = 0;
        int offset_x = 0;
        int offset_y = 0;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(&font->info, font->scale, font->scale,
            (int) code, &glyph_width, &glyph_height, &offset_x, &offset_y);
        if (bitmap != NULL) {
            for (int row = 0; row < glyph_height; row++) {
                int canvas_row = baseline + offset_y + row;
                if (canvas_row < 0 || canvas_row >= height) continue;
                for (int column = 0; column < glyph_width; column++) {
                    int canvas_column = (int) (pen + 0.5f) + offset_x + column;
                    if (canvas_column < 0 || canvas_column >= width) continue;
                    unsigned coverage = bitmap[(size_t) row * glyph_width + column];
                    if (coverage == 0) continue;
                    uint32_t *pixel = &canvas[(size_t) canvas_row * width + canvas_column];
                    unsigned inverse = 255 - coverage;
                    unsigned out_red = (red * coverage + ((*pixel >> 16) & 0xff) * inverse) / 255;
                    unsigned out_green = (green * coverage + ((*pixel >> 8) & 0xff) * inverse) / 255;
                    unsigned out_blue = (blue * coverage + (*pixel & 0xff) * inverse) / 255;
                    *pixel = 0xff000000u | (out_red << 16) | (out_green << 8) | out_blue;
                }
            }
            stbtt_FreeBitmap(bitmap, NULL);
        }
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(&font->info, (int) code, &advance, &bearing);
        pen += font->scale * (float) advance;
        previous = code;
    }
    return line_height;
}
