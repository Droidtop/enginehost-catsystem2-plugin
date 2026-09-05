#include "render.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "hg3.h"

#define CACHE_SIZE 12

typedef struct {
    char name[128];
    cs2_hg3_frame frame;
    int used;
    unsigned long age;
} cached;

struct cs2_render {
    cs2_files *files;
    int width;
    int height;
    uint32_t *canvas;
    cached cache[CACHE_SIZE];
    unsigned long clock;
    cs2_font *font;
    cs2_font *small_font;
};

/* The game's own font, whichever face it ships beside its archives. */
static char *find_font(const char *root) {
    DIR *directory = opendir(root);
    if (directory == NULL) return NULL;
    char best[256] = "";
    for (struct dirent *item; (item = readdir(directory)) != NULL; ) {
        size_t length = strlen(item->d_name);
        if (length < 5 || length >= sizeof best) continue;
        const char *suffix = item->d_name + length - 4;
        if (!cs2_ieq(suffix, ".ttf") && !cs2_ieq(suffix, ".otf") && !cs2_ieq(suffix, ".ttc")) continue;
        if (best[0] == '\0' || strcmp(item->d_name, best) < 0) {
            memcpy(best, item->d_name, length + 1);
        }
    }
    closedir(directory);
    if (best[0] == '\0') return NULL;
    size_t size = strlen(root) + strlen(best) + 2;
    char *path = malloc(size);
    if (path == NULL) return NULL;
    snprintf(path, size, "%s/%s", root, best);
    return path;
}

cs2_render *cs2_render_new(cs2_files *files, int width, int height) {
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
    render->width = width;
    render->height = height;
    render->canvas = calloc((size_t) width * height, sizeof *render->canvas);
    if (render->canvas == NULL) {
        free(render);
        cs2_set_error("out of memory");
        return NULL;
    }
    char *path = find_font(cs2_files_root(files));
    if (path == NULL) {
        cs2_log("the game ships no font beside its archives; no text will be drawn");
    } else {
        render->font = cs2_font_open(path, 30);
        render->small_font = cs2_font_open(path, 19);
        if (render->font == NULL) cs2_log("could not read the game's font %s", path);
        free(path);
    }
    return render;
}

void cs2_render_free(cs2_render *render) {
    if (render == NULL) return;
    for (int i = 0; i < CACHE_SIZE; i++) cs2_hg3_frame_free(&render->cache[i].frame);
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

static const cs2_hg3_frame *image(cs2_render *render, const char *name) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (render->cache[i].used && strcmp(render->cache[i].name, name) == 0) {
            render->cache[i].age = ++render->clock;
            return &render->cache[i].frame;
        }
    }
    char path[256];
    snprintf(path, sizeof path, "image.int/%s.hg3", name);
    cs2_bytes data;
    if (cs2_files_read(render->files, path, &data) != 0) {
        snprintf(path, sizeof path, "%s.hg3", name);
        if (cs2_files_read(render->files, path, &data) != 0) {
            cs2_log("no image named %s", name);
            return NULL;
        }
    }
    cs2_hg3_frame frame;
    int decoded = cs2_hg3_decode(data.data, data.size, 0, &frame);
    cs2_bytes_free(&data);
    if (decoded != 0) {
        cs2_log("could not decode %s: %s", name, cs2_error());
        return NULL;
    }
    int oldest = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!render->cache[i].used) {
            oldest = i;
            break;
        }
        if (render->cache[i].age < render->cache[oldest].age) oldest = i;
    }
    cs2_hg3_frame_free(&render->cache[oldest].frame);
    snprintf(render->cache[oldest].name, sizeof render->cache[oldest].name, "%s", name);
    render->cache[oldest].frame = frame;
    render->cache[oldest].used = 1;
    render->cache[oldest].age = ++render->clock;
    return &render->cache[oldest].frame;
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
        const cs2_hg3_frame *frame = image(render, layer->image);
        if (frame != NULL) draw_image(render, frame, layer->x, layer->y, layer->alpha);
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
