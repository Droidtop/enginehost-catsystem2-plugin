#include "pe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs2.h"

#define MAX_RESOURCES 512
#define MAX_SECTIONS 96
#define MAX_RESOURCE_SIZE (1u << 20)

typedef struct {
    char key[80];
    const uint8_t *data;
    size_t size;
} resource;

struct cs2_pe {
    resource *items;
    size_t count;
};

typedef struct {
    uint32_t virtual_address;
    uint32_t span;
    uint32_t raw;
} section;

typedef struct {
    const uint8_t *image;
    size_t size;
    section sections[MAX_SECTIONS];
    int section_count;
    size_t root;
    cs2_pe *pe;
} walk_state;

/* Where an address in the loaded image lives in the file, or the size on miss. */
static size_t file_offset(const walk_state *state, uint32_t rva) {
    for (int i = 0; i < state->section_count; i++) {
        const section *s = &state->sections[i];
        if (rva >= s->virtual_address && rva - s->virtual_address < s->span) {
            size_t at = (size_t) s->raw + (rva - s->virtual_address);
            if (at < state->size) return at;
            return state->size;
        }
    }
    return state->size;
}

/* A resource-tree name: a two-byte length then that many UTF-16 units. */
static int read_name(const walk_state *state, uint32_t offset, char *out, size_t out_size) {
    size_t at = state->root + offset;
    if (at + 2 > state->size) return -1;
    unsigned length = cs2_u16(state->image, state->size, at);
    if (length == 0 || length > 64 || at + 2 + (size_t) length * 2 > state->size) return -1;
    size_t written = 0;
    for (unsigned i = 0; i < length && written + 1 < out_size; i++) {
        unsigned unit = cs2_u16(state->image, state->size, at + 2 + (size_t) i * 2);
        out[written++] = unit < 0x80 ? (char) unit : '?';
    }
    out[written] = '\0';
    return 0;
}

static void add(walk_state *state, const char *type, const char *name,
                const uint8_t *data, size_t size) {
    cs2_pe *pe = state->pe;
    if (pe->count >= MAX_RESOURCES) return;
    resource *item = &pe->items[pe->count];
    snprintf(item->key, sizeof item->key, "%s/%s", type, name);
    item->data = data;
    item->size = size;
    pe->count++;
}

static void walk(walk_state *state, size_t directory, const char *type, const char *name, int depth) {
    if (depth > 2 || directory + 16 > state->size || state->pe->count >= MAX_RESOURCES) return;
    unsigned named = cs2_u16(state->image, state->size, directory + 12);
    unsigned numbered = cs2_u16(state->image, state->size, directory + 14);
    unsigned total = named + numbered;
    for (unsigned i = 0; i < total; i++) {
        size_t entry = directory + 16 + (size_t) i * 8;
        if (entry + 8 > state->size) return;
        uint32_t name_field = cs2_u32(state->image, state->size, entry);
        uint32_t data_field = cs2_u32(state->image, state->size, entry + 4);
        char label[80];
        if (name_field & 0x80000000u) {
            if (read_name(state, name_field & 0x7fffffffu, label, sizeof label) != 0) continue;
        } else if (depth < 2) {
            /* Numbered types and names are icons, dialogs and the rest of the
               standard resource tree; only string-named ones matter here. */
            continue;
        } else {
            label[0] = '\0';
        }
        if (data_field & 0x80000000u) {
            walk(state, state->root + (data_field & 0x7fffffffu),
                 depth == 0 ? label : type, depth == 1 ? label : name, depth + 1);
        } else if (depth == 2 && type != NULL && name != NULL) {
            size_t data = state->root + data_field;
            if (data + 8 > state->size) continue;
            size_t at = file_offset(state, cs2_u32(state->image, state->size, data));
            uint32_t size = cs2_u32(state->image, state->size, data + 4);
            if (at >= state->size || size == 0 || size > MAX_RESOURCE_SIZE
                || at + size > state->size) {
                continue;
            }
            add(state, type, name, state->image + at, size);
        }
    }
}

cs2_pe *cs2_pe_open(const uint8_t *image, size_t size) {
    if (image == NULL || size < 0x40 || image[0] != 'M' || image[1] != 'Z') return NULL;
    size_t pe_offset = cs2_u32(image, size, 0x3c);
    if (pe_offset + 24 > size || cs2_u32(image, size, pe_offset) != 0x00004550u) return NULL;

    size_t coff = pe_offset + 4;
    unsigned section_count = cs2_u16(image, size, coff + 2);
    unsigned optional_size = cs2_u16(image, size, coff + 16);
    size_t optional = coff + 20;
    unsigned magic = cs2_u16(image, size, optional);
    size_t directories = optional + (magic == 0x20b ? 108 : 92);
    if (directories + 4 + 8 * 3 > size) return NULL;
    uint32_t resource_rva = cs2_u32(image, size, directories + 4 + 8 * 2);
    if (resource_rva == 0) return NULL;
    if (section_count == 0 || section_count > MAX_SECTIONS) return NULL;

    walk_state state;
    memset(&state, 0, sizeof state);
    state.image = image;
    state.size = size;
    state.section_count = (int) section_count;
    size_t table = optional + optional_size;
    for (unsigned i = 0; i < section_count; i++) {
        size_t base = table + (size_t) i * 40;
        if (base + 40 > size) return NULL;
        uint32_t raw_size = cs2_u32(image, size, base + 16);
        uint32_t virtual_size = cs2_u32(image, size, base + 8);
        state.sections[i].virtual_address = cs2_u32(image, size, base + 12);
        state.sections[i].span = raw_size > virtual_size ? raw_size : virtual_size;
        state.sections[i].raw = cs2_u32(image, size, base + 20);
    }

    state.root = file_offset(&state, resource_rva);
    if (state.root >= size) return NULL;

    cs2_pe *pe = calloc(1, sizeof *pe);
    if (pe == NULL) return NULL;
    pe->items = calloc(MAX_RESOURCES, sizeof *pe->items);
    if (pe->items == NULL) {
        free(pe);
        return NULL;
    }
    state.pe = pe;
    walk(&state, state.root, NULL, NULL, 0);
    if (pe->count == 0) {
        cs2_pe_free(pe);
        return NULL;
    }
    return pe;
}

const uint8_t *cs2_pe_resource(const cs2_pe *pe, const char *type, const char *name, size_t *size) {
    if (pe == NULL) return NULL;
    char forward[80];
    char backward[80];
    snprintf(forward, sizeof forward, "%s/%s", type, name);
    snprintf(backward, sizeof backward, "%s/%s", name, type);
    for (size_t i = 0; i < pe->count; i++) {
        if (cs2_ieq(pe->items[i].key, forward) || cs2_ieq(pe->items[i].key, backward)) {
            if (size != NULL) *size = pe->items[i].size;
            return pe->items[i].data;
        }
    }
    return NULL;
}

void cs2_pe_free(cs2_pe *pe) {
    if (pe == NULL) return;
    free(pe->items);
    free(pe);
}
