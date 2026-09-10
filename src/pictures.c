#include "pictures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs2.h"

#define KEPT 512
#define DEFAULT_BUDGET (192u * 1024 * 1024)

typedef struct {
    int used;
    char name[128];
    int id;                  /* the id the layouts name, or the index */
    int by_index;
    int there;               /* 0 for a frame this picture does not carry */
    cs2_hg3_frame frame;
    size_t bytes;
    unsigned long age;
} kept;

struct cs2_pictures {
    cs2_files *files;
    size_t budget;
    size_t held;
    unsigned long clock;
    kept entries[KEPT];
    /*
     * The file the last frame came out of. Eight buttons of one picture are
     * eight frames of one archive entry, and deciphering and inflating that
     * entry once for all of them is the difference between a screen appearing
     * and a screen being waited for.
     */
    char open_name[128];
    cs2_bytes open_file;
    int open_missing;         /* the game has no picture of that name */
    char said[128];           /* the last missing one complained about */
};

cs2_pictures *cs2_pictures_new(cs2_files *files, size_t budget) {
    cs2_pictures *pictures = calloc(1, sizeof *pictures);
    if (pictures == NULL) {
        cs2_set_error("out of memory");
        return NULL;
    }
    pictures->files = files;
    pictures->budget = budget == 0 ? DEFAULT_BUDGET : budget;
    return pictures;
}

void cs2_pictures_free(cs2_pictures *pictures) {
    if (pictures == NULL) return;
    for (int i = 0; i < KEPT; i++) cs2_hg3_frame_free(&pictures->entries[i].frame);
    cs2_bytes_free(&pictures->open_file);
    free(pictures);
}

/* The picture itself, read out of the game once and kept until another is. */
static const cs2_bytes *file_of(cs2_pictures *pictures, const char *name) {
    if (strcmp(pictures->open_name, name) == 0) {
        return pictures->open_missing ? NULL : &pictures->open_file;
    }
    cs2_bytes_free(&pictures->open_file);
    snprintf(pictures->open_name, sizeof pictures->open_name, "%s", name);
    pictures->open_missing = 0;
    char path[256];
    snprintf(path, sizeof path, "image.int/%s.hg3", name);
    if (cs2_files_read(pictures->files, path, &pictures->open_file) != 0) {
        snprintf(path, sizeof path, "%s.hg3", name);
        if (cs2_files_read(pictures->files, path, &pictures->open_file) != 0) {
            pictures->open_missing = 1;
            if (strcmp(pictures->said, name) != 0) {
                snprintf(pictures->said, sizeof pictures->said, "%s", name);
                cs2_log("this copy of the game has no picture named %s", name);
            }
            return NULL;
        }
    }
    return &pictures->open_file;
}

/* Room for one more, by letting the least recently asked-for go. */
static void make_room(cs2_pictures *pictures, size_t wanted) {
    for (;;) {
        if (pictures->held + wanted <= pictures->budget) return;
        int oldest = -1;
        for (int i = 0; i < KEPT; i++) {
            if (!pictures->entries[i].used || pictures->entries[i].bytes == 0) continue;
            if (oldest < 0 || pictures->entries[i].age < pictures->entries[oldest].age) oldest = i;
        }
        if (oldest < 0) return;
        pictures->held -= pictures->entries[oldest].bytes;
        cs2_hg3_frame_free(&pictures->entries[oldest].frame);
        memset(&pictures->entries[oldest], 0, sizeof pictures->entries[oldest]);
    }
}

static kept *a_place(cs2_pictures *pictures) {
    int oldest = -1;
    for (int i = 0; i < KEPT; i++) {
        if (!pictures->entries[i].used) return &pictures->entries[i];
        if (oldest < 0 || pictures->entries[i].age < pictures->entries[oldest].age) oldest = i;
    }
    pictures->held -= pictures->entries[oldest].bytes;
    cs2_hg3_frame_free(&pictures->entries[oldest].frame);
    memset(&pictures->entries[oldest], 0, sizeof pictures->entries[oldest]);
    return &pictures->entries[oldest];
}

static const cs2_hg3_frame *decoded(cs2_pictures *pictures, const char *name,
                                    int which, int by_index) {
    if (pictures == NULL || name == NULL || name[0] == 0) return NULL;
    for (int i = 0; i < KEPT; i++) {
        kept *entry = &pictures->entries[i];
        if (!entry->used || entry->id != which || entry->by_index != by_index) continue;
        if (strcmp(entry->name, name) != 0) continue;
        entry->age = ++pictures->clock;
        return entry->there ? &entry->frame : NULL;
    }
    const cs2_bytes *file = file_of(pictures, name);
    cs2_hg3_frame frame;
    int decoded_it = 0;
    if (file != NULL) {
        decoded_it = by_index
            ? cs2_hg3_decode(file->data, file->size, which, &frame) == 0
            : cs2_hg3_decode_id(file->data, file->size, which, &frame) == 0;
    }
    /*
     * A picture that is in the game and a frame that is not in the picture are
     * both written down, the second one as a no. Only a picture the game does
     * not have at all is left unwritten, because a game whose numbered strings
     * have not been set yet names pictures that exist a moment later.
     */
    if (file == NULL) return NULL;
    size_t bytes = decoded_it
        ? (size_t) frame.width * (size_t) frame.height * sizeof(uint32_t) : 0;
    make_room(pictures, bytes);
    kept *entry = a_place(pictures);
    entry->used = 1;
    snprintf(entry->name, sizeof entry->name, "%s", name);
    entry->id = which;
    entry->by_index = by_index;
    entry->there = decoded_it;
    entry->bytes = bytes;
    entry->age = ++pictures->clock;
    if (decoded_it) {
        entry->frame = frame;
        pictures->held += bytes;
        return &entry->frame;
    }
    return NULL;
}

const cs2_hg3_frame *cs2_pictures_id(cs2_pictures *pictures, const char *name, int id) {
    return decoded(pictures, name, id, 0);
}

const cs2_hg3_frame *cs2_pictures_index(cs2_pictures *pictures, const char *name, int index) {
    return decoded(pictures, name, index, 1);
}

int cs2_pictures_has(cs2_pictures *pictures, const char *name) {
    if (pictures == NULL || name == NULL || name[0] == 0) return 0;
    return file_of(pictures, name) != NULL;
}
