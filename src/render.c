#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "hg3.h"
#include "png.h"

struct cs2_render {
    cs2_files *files;
    cs2_pictures *pictures;
    int width;
    int height;
    uint32_t *canvas;
    cs2_font *font;
    cs2_font *small_font;
};

cs2_render *cs2_render_new(cs2_files *files, cs2_pictures *pictures, int width, int height) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        cs2_set_error("cannot draw a %dx%d screen", width, height);
        return NULL;
    }
    cs2_render *render = calloc(1, sizeof *render);
    if (render == NULL) {
        cs2_set_error("out of memory");
        return NULL;
    }
    render->files = files;
    render->pictures = pictures;
    render->width = width;
    render->height = height;
    render->canvas = calloc((size_t) width * height, sizeof *render->canvas);
    if (render->canvas == NULL) {
        free(render);
        cs2_set_error("out of memory");
        return NULL;
    }
    const char *root = cs2_files_root(files);
    render->font = cs2_font_open_beside(root, 30);
    render->small_font = cs2_font_open_beside(root, 19);
    if (render->font == NULL) {
        cs2_log("the game ships no font beside its archives; no text will be drawn");
    }
    return render;
}

void cs2_render_free(cs2_render *render) {
    if (render == NULL) return;
    cs2_font_free(render->font);
    cs2_font_free(render->small_font);
    free(render->canvas);
    free(render);
}

int cs2_render_width(const cs2_render *render) { return render->width; }
int cs2_render_height(const cs2_render *render) { return render->height; }

/* One source pixel over one destination pixel, both 0xAARRGGBB. */
static void blend(uint32_t *destination, uint32_t source, int alpha) {
    unsigned source_alpha = ((source >> 24) * (unsigned) alpha) / 255;
    if (source_alpha == 0) return;
    if (source_alpha == 255) {
        *destination = 0xff000000u | (source & 0x00ffffffu);
        return;
    }
    unsigned inverse = 255 - source_alpha;
    unsigned red = (((source >> 16) & 0xff) * source_alpha + ((*destination >> 16) & 0xff) * inverse) / 255;
    unsigned green = (((source >> 8) & 0xff) * source_alpha + ((*destination >> 8) & 0xff) * inverse) / 255;
    unsigned blue = ((source & 0xff) * source_alpha + (*destination & 0xff) * inverse) / 255;
    *destination = 0xff000000u | (red << 16) | (green << 8) | blue;
}

static void fill(cs2_render *render, int x, int y, int width, int height, uint32_t colour) {
    int alpha = (int) (colour >> 24);
    for (int row = y; row < y + height; row++) {
        if (row < 0 || row >= render->height) continue;
        for (int column = x; column < x + width; column++) {
            if (column < 0 || column >= render->width) continue;
            blend(&render->canvas[(size_t) row * render->width + column],
                  colour | 0xff000000u, alpha);
        }
    }
}

static void draw_image(cs2_render *render, const cs2_hg3_frame *frame, int x, int y, int alpha) {
    for (int row = 0; row < frame->height; row++) {
        int destination_row = y + frame->offset_y + row;
        if (destination_row < 0 || destination_row >= render->height) continue;
        for (int column = 0; column < frame->width; column++) {
            int destination_column = x + frame->offset_x + column;
            if (destination_column < 0 || destination_column >= render->width) continue;
            blend(&render->canvas[(size_t) destination_row * render->width + destination_column],
                  frame->pixels[(size_t) row * frame->width + column], alpha);
        }
    }
}

static int draw_line(cs2_render *render, const cs2_font *font, const char *line,
                     int x, int y, uint32_t colour) {
    return cs2_font_draw(font, render->canvas, render->width, render->height, x, y, line, colour);
}

/* Breaks a line at spaces so it fits the width the message box allows. */
static void draw_wrapped(cs2_render *render, const char *text, int x, int *y, int width) {
    if (render->font == NULL) return;
    char line[1024];
    size_t at = 0;
    while (text[at] != '\0') {
        size_t end = at;
        size_t last_space = 0;
        size_t length = 0;
        while (text[end] != '\0' && text[end] != '\n' && length + 1 < sizeof line) {
            line[length++] = text[end];
            line[length] = '\0';
            if (cs2_font_measure(render->font, line) > width && last_space > 0) {
                length = last_space;
                line[length] = '\0';
                end = at + last_space;
                break;
            }
            if (text[end] == ' ') last_space = length;
            end++;
        }
        *y += draw_line(render, render->font, line, x, *y, 0xffffffffu) + 4;
        at = end;
        while (text[at] == ' ' || text[at] == '\n') at++;
    }
}

const uint32_t *cs2_render_frame(cs2_render *render, const cs2_scene *scene, const char *status) {
    for (size_t i = 0; i < (size_t) render->width * render->height; i++) {
        render->canvas[i] = 0xff000000u;
    }
    for (size_t i = 0; i < cs2_scene_layer_count(scene); i++) {
        const cs2_layer *layer = cs2_scene_layer_at(scene, i);
        if (layer->is_colour) {
            fill(render, layer->x, layer->y,
                 layer->width > 0 ? layer->width : render->width,
                 layer->height > 0 ? layer->height : render->height, layer->colour);
            continue;
        }
        const cs2_hg3_frame *frame = cs2_pictures_index(render->pictures, layer->image, 0);
        if (frame != NULL) draw_image(render, frame, layer->x, layer->y, layer->alpha);
        for (int part = 0; part < layer->part_count; part++) {
            const cs2_hg3_frame *over = cs2_pictures_index(render->pictures, layer->parts[part], 0);
            if (over != NULL) draw_image(render, over, layer->x, layer->y, layer->alpha);
        }
    }

    const char *speaker = cs2_scene_speaker(scene);
    const char *text = cs2_scene_text(scene);
    if (render->font != NULL && (speaker[0] != '\0' || text[0] != '\0')) {
        int margin = render->width / 20;
        int box_top = render->height * 3 / 5;
        fill(render, margin / 2, box_top, render->width - margin,
             render->height - box_top - margin / 4, 0xcc000c10u);
        int y = box_top + margin / 2;
        if (speaker[0] != '\0') {
            y += draw_line(render, render->font, speaker, margin, y, 0xff80cbc4u) + 4;
        }
        draw_wrapped(render, text, margin, &y, render->width - margin * 2);
    }
    if (status != NULL && render->small_font != NULL) {
        draw_line(render, render->small_font, status, 12, 6, 0xff6d7f8au);
    }
    return render->canvas;
}

/*
 * Draws named images over one another into a PNG. This exists to answer a
 * question the scripts do not: a character is a body and a set of parts, named
 * with suffixes the scripts index by, and the only way to learn which suffix is
 * which is to lay them over one another and look.
 */
int cs2_draw_images(cs2_files *files, const char *names, const char *path) {
    cs2_pictures *pictures = cs2_pictures_new(files, 0);
    cs2_render *render = pictures == NULL ? NULL
                       : cs2_render_new(files, pictures, 1024, 768);
    if (render == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        cs2_pictures_free(pictures);
        return 1;
    }
    for (size_t i = 0; i < (size_t) render->width * render->height; i++) {
        render->canvas[i] = 0xff202020u;
    }
    char list[1024];
    snprintf(list, sizeof list, "%s", names);
    int drawn = 0;
    for (char *name = strtok(list, ","); name != NULL; name = strtok(NULL, ",")) {
        const cs2_hg3_frame *frame = cs2_pictures_index(pictures, name, 0);
        if (frame == NULL) continue;
        cs2_log("%s: %dx%d at %d,%d of %dx%d, base %d,%d", name, frame->width, frame->height,
                frame->offset_x, frame->offset_y, frame->total_width, frame->total_height,
                frame->base_x, frame->base_y);
        draw_image(render, frame, 0, 0, 255);
        drawn++;
    }
    int result = drawn == 0 ? 1 : cs2_png_write(path, render->canvas, render->width, render->height);
    if (result != 0) fprintf(stderr, "%s\n", cs2_error());
    cs2_render_free(render);
    cs2_pictures_free(pictures);
    return result;
}
