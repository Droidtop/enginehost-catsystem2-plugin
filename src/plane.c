/*
 * The plane table and how it is drawn. plane.h says what a plane is.
 */
#include "plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs2.h"

#define PLANE_LIMIT 512

struct cs2_planes {
    cs2_plane_state entries[PLANE_LIMIT];
    size_t count;
    cs2_plane next_handle;
    cs2_plane root;
};

static cs2_plane_state *find(cs2_planes *planes, cs2_plane handle) {
    if (planes == NULL || handle == 0) return NULL;
    for (size_t i = 0; i < planes->count; i++) {
        if (planes->entries[i].handle == handle) return &planes->entries[i];
    }
    return NULL;
}

static const cs2_plane_state *find_const(const cs2_planes *planes, cs2_plane handle) {
    if (planes == NULL || handle == 0) return NULL;
    for (size_t i = 0; i < planes->count; i++) {
        if (planes->entries[i].handle == handle) return &planes->entries[i];
    }
    return NULL;
}

cs2_planes *cs2_planes_new(int width, int height) {
    cs2_planes *planes = calloc(1, sizeof *planes);
    if (planes == NULL) {
        cs2_set_error("out of memory for the screen's planes");
        return NULL;
    }
    planes->next_handle = 1;
    planes->root = cs2_plane_create(planes, 0, 0, "screen");
    cs2_plane_state *root = find(planes, planes->root);
    if (root != NULL) {
        root->width = (float) width;
        root->height = (float) height;
        root->visible = 1;
    }
    return planes;
}

void cs2_planes_free(cs2_planes *planes) {
    free(planes);
}

cs2_plane cs2_planes_root(const cs2_planes *planes) {
    return planes == NULL ? 0 : planes->root;
}

cs2_plane cs2_plane_create(cs2_planes *planes, int type, cs2_plane parent, const char *name) {
    if (planes == NULL) return 0;
    if (planes->count >= PLANE_LIMIT) {
        cs2_log("the script has asked for more than %d planes; refusing", PLANE_LIMIT);
        return 0;
    }
    cs2_plane_state *plane = &planes->entries[planes->count++];
    memset(plane, 0, sizeof *plane);
    plane->handle = planes->next_handle++;
    plane->parent = parent;
    plane->type = type;
    plane->result = -1;
    if (name != NULL) snprintf(plane->name, sizeof plane->name, "%s", name);
    return plane->handle;
}

void cs2_plane_destroy(cs2_planes *planes, cs2_plane handle) {
    cs2_plane_state *plane = find(planes, handle);
    if (plane == NULL || handle == planes->root) return;
    size_t index = (size_t) (plane - planes->entries);
    memmove(&planes->entries[index], &planes->entries[index + 1],
            (planes->count - index - 1) * sizeof *planes->entries);
    planes->count--;
}

cs2_plane_state *cs2_plane_get(cs2_planes *planes, cs2_plane handle) {
    return find(planes, handle);
}

size_t cs2_planes_count(const cs2_planes *planes) {
    return planes == NULL ? 0 : planes->count;
}

const cs2_plane_state *cs2_planes_at(const cs2_planes *planes, size_t index) {
    if (planes == NULL || index >= planes->count) return NULL;
    return &planes->entries[index];
}

/* Where a plane sits on the screen: its own place plus every parent's. */
static void absolute(const cs2_planes *planes, const cs2_plane_state *plane, float *x, float *y) {
    *x = plane->x;
    *y = plane->y;
    cs2_plane parent = plane->parent;
    for (size_t guard = 0; parent != 0 && guard < planes->count; guard++) {
        const cs2_plane_state *above = find_const(planes, parent);
        if (above == NULL) break;
        *x += above->x;
        *y += above->y;
        parent = above->parent;
    }
}

/* Whether a plane and everything it hangs from is shown. */
static int shown(const cs2_planes *planes, const cs2_plane_state *plane) {
    if (!plane->visible) return 0;
    cs2_plane parent = plane->parent;
    for (size_t guard = 0; parent != 0 && guard < planes->count; guard++) {
        const cs2_plane_state *above = find_const(planes, parent);
        if (above == NULL) break;
        if (!above->visible) return 0;
        parent = above->parent;
    }
    return 1;
}

static void blend(uint32_t *pixel, uint32_t colour) {
    uint32_t alpha = colour >> 24;
    if (alpha == 0) return;
    if (alpha == 255) {
        *pixel = colour;
        return;
    }
    uint32_t under = *pixel, out = 0xff000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t a = (under >> shift) & 0xffu, b = (colour >> shift) & 0xffu;
        out |= ((b * alpha + a * (255u - alpha)) / 255u) << shift;
    }
    *pixel = out;
}

void cs2_planes_draw(const cs2_planes *planes, uint32_t *canvas, int width, int height) {
    if (planes == NULL || canvas == NULL) return;
    /*
     * Priority order, lowest first, equal priorities keeping the order they
     * were made in. A selection sort over a few hundred planes once a frame
     * costs nothing beside the drawing, and it leaves the table itself in the
     * order the script built it, which is what the logs read from.
     */
    size_t order[PLANE_LIMIT];
    for (size_t i = 0; i < planes->count; i++) order[i] = i;
    for (size_t i = 0; i + 1 < planes->count; i++) {
        size_t least = i;
        for (size_t j = i + 1; j < planes->count; j++) {
            if (planes->entries[order[j]].priority < planes->entries[order[least]].priority) least = j;
        }
        size_t hold = order[i];
        order[i] = order[least];
        order[least] = hold;
    }

    for (size_t k = 0; k < planes->count; k++) {
        const cs2_plane_state *plane = &planes->entries[order[k]];
        if (!plane->filled || !shown(planes, plane)) continue;
        float fx = 0, fy = 0;
        absolute(planes, plane, &fx, &fy);
        int left = (int) fx, top = (int) fy;
        /*
         * A plane covers its own rectangle and no more. A plane with no size
         * used to be taken as the whole screen, and once a scene starts the
         * game has thirty-odd of those - the layers it has made and not yet
         * put anything on - so the reader was shown a screen of flat colour
         * with the game underneath it.
         */
        if (plane->width == 0 || plane->height == 0) continue;
        int wide = (int) plane->width;
        int tall = (int) plane->height;
        for (int y = top; y < top + tall; y++) {
            if (y < 0 || y >= height) continue;
            for (int x = left; x < left + wide; x++) {
                if (x < 0 || x >= width) continue;
                blend(&canvas[(size_t) y * width + x], plane->colour);
            }
        }
    }
}
