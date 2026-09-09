#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cs2.h"

/*
 * The file. Eight bytes that say what it is, then a header, then chunks: a
 * four-byte tag and a size, so a save written by an older build is read by a
 * newer one without a version to interpret, and a chunk this build does not
 * know is walked past rather than refused.
 */
#define MAGIC "CS2SAVE\1"
#define MAGIC_BYTES 8

#define TAG_SCENE 0x4e454353u   /* "SCEN" the scenario and how far into it */
#define TAG_FLAG  0x47414c46u   /* "FLAG" the numbered flags */
#define TAG_STRS  0x53525453u   /* "STRS" the numbered strings */
#define TAG_READ  0x44414552u   /* "READ" the read-text high-water marks */

#define SLOT_LIMIT 4096         /* pages 0..21 of eight is 176; this is generous */
#define SAVE_BYTES (4u * 1024u * 1024u)

struct cs2_saves {
    char folder[512];
};

/* ------------------------------------------------------------- one save */

void cs2_save_clear(cs2_save *save) {
    if (save == NULL) return;
    free(save->flags);
    for (size_t i = 0; i < save->string_count; i++) free(save->strings[i].text);
    free(save->strings);
    free(save->marks);
    memset(save, 0, sizeof *save);
}

void cs2_save_set_flag(cs2_save *save, uint32_t key, int32_t value) {
    for (size_t i = 0; i < save->flag_count; i++) {
        if (save->flags[i].key == key) {
            save->flags[i].value = value;
            return;
        }
    }
    cs2_save_flag *grown = realloc(save->flags, (save->flag_count + 1) * sizeof *grown);
    if (grown == NULL) return;
    save->flags = grown;
    save->flags[save->flag_count].key = key;
    save->flags[save->flag_count].value = value;
    save->flag_count++;
}

int cs2_save_flag_value(const cs2_save *save, uint32_t key, int32_t *value) {
    for (size_t i = 0; i < save->flag_count; i++) {
        if (save->flags[i].key == key) {
            if (value != NULL) *value = save->flags[i].value;
            return 1;
        }
    }
    return 0;
}

void cs2_save_set_string(cs2_save *save, uint32_t key, const char *text) {
    if (text == NULL) text = "";
    size_t length = strlen(text) + 1;
    for (size_t i = 0; i < save->string_count; i++) {
        if (save->strings[i].key != key) continue;
        char *kept = realloc(save->strings[i].text, length);
        if (kept == NULL) return;
        memcpy(kept, text, length);
        save->strings[i].text = kept;
        return;
    }
    cs2_save_string *grown = realloc(save->strings, (save->string_count + 1) * sizeof *grown);
    if (grown == NULL) return;
    save->strings = grown;
    char *kept = malloc(length);
    if (kept == NULL) return;
    memcpy(kept, text, length);
    save->strings[save->string_count].key = key;
    save->strings[save->string_count].text = kept;
    save->string_count++;
}

const char *cs2_save_string_value(const cs2_save *save, uint32_t key) {
    for (size_t i = 0; i < save->string_count; i++) {
        if (save->strings[i].key == key) return save->strings[i].text;
    }
    return NULL;
}

/* Raised and never lowered: that is what the read-text mark is (`cmovae`). */
void cs2_save_set_mark(cs2_save *save, const char *name, int32_t mark) {
    if (name == NULL || name[0] == 0) return;
    for (size_t i = 0; i < save->mark_count; i++) {
        if (strcmp(save->marks[i].name, name) != 0) continue;
        if (mark > save->marks[i].mark) save->marks[i].mark = mark;
        return;
    }
    cs2_save_mark *grown = realloc(save->marks, (save->mark_count + 1) * sizeof *grown);
    if (grown == NULL) return;
    save->marks = grown;
    snprintf(save->marks[save->mark_count].name,
             sizeof save->marks[save->mark_count].name, "%s", name);
    save->marks[save->mark_count].mark = mark;
    save->mark_count++;
}

int32_t cs2_save_mark_value(const cs2_save *save, const char *name) {
    if (name == NULL) return -1;
    for (size_t i = 0; i < save->mark_count; i++) {
        if (strcmp(save->marks[i].name, name) == 0) return save->marks[i].mark;
    }
    return -1;
}

/* ------------------------------------------------------------- the bytes */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} builder;

static void put(builder *out, const void *bytes, size_t count) {
    if (out->failed) return;
    if (out->size + count > out->capacity) {
        size_t wanted = (out->capacity == 0 ? 4096 : out->capacity * 2);
        while (wanted < out->size + count) wanted *= 2;
        uint8_t *grown = realloc(out->data, wanted);
        if (grown == NULL) {
            out->failed = 1;
            return;
        }
        out->data = grown;
        out->capacity = wanted;
    }
    memcpy(out->data + out->size, bytes, count);
    out->size += count;
}

static void put32(builder *out, uint32_t value) {
    uint8_t bytes[4] = { (uint8_t) value, (uint8_t) (value >> 8),
                         (uint8_t) (value >> 16), (uint8_t) (value >> 24) };
    put(out, bytes, 4);
}

static void put64(builder *out, uint64_t value) {
    put32(out, (uint32_t) value);
    put32(out, (uint32_t) (value >> 32));
}

static void put_text(builder *out, const char *text) {
    if (text == NULL) text = "";
    uint32_t length = (uint32_t) strlen(text);
    put32(out, length);
    put(out, text, length);
}

/* A chunk is written into its own builder so its size is known before its tag. */
static void put_chunk(builder *out, uint32_t tag, const builder *body) {
    put32(out, tag);
    put32(out, (uint32_t) body->size);
    put(out, body->data, body->size);
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t at;
    int failed;
} scanner;

static uint32_t take32(scanner *in) {
    if (in->failed || in->at + 4 > in->size) {
        in->failed = 1;
        return 0;
    }
    uint32_t value = cs2_u32(in->data, in->size, in->at);
    in->at += 4;
    return value;
}

static uint64_t take64(scanner *in) {
    uint32_t low = take32(in);
    uint32_t high = take32(in);
    return (uint64_t) low | ((uint64_t) high << 32);
}

/* Copies a counted string into a buffer of its own, always terminated. */
static void take_text(scanner *in, char *into, size_t into_size) {
    uint32_t length = take32(in);
    if (in->failed || length > in->size - in->at) {
        in->failed = 1;
        if (into_size > 0) into[0] = 0;
        return;
    }
    size_t keep = length;
    if (keep + 1 > into_size) keep = into_size - 1;
    memcpy(into, in->data + in->at, keep);
    into[keep] = 0;
    in->at += length;
}

/* ------------------------------------------------------------- the store */

/* The folder and everything above it: the host's may not be there yet either. */
static int make_folder(const char *path) {
    struct stat info;
    if (stat(path, &info) == 0) return 0;
    char built[512];
    size_t length = snprintf(built, sizeof built, "%s", path);
    if (length >= sizeof built) return -1;
    for (size_t i = 1; i < length; i++) {
        if (built[i] != '/') continue;
        built[i] = 0;
        if (stat(built, &info) != 0) mkdir(built, 0777);
        built[i] = '/';
    }
    return mkdir(path, 0777) == 0 || stat(path, &info) == 0 ? 0 : -1;
}

cs2_saves *cs2_saves_open(const char *folder) {
    if (folder == NULL || folder[0] == 0) {
        cs2_set_error("a save folder is needed and none was given");
        return NULL;
    }
    if (make_folder(folder) != 0) {
        cs2_set_error("the save folder %s cannot be made", folder);
        return NULL;
    }
    cs2_saves *saves = calloc(1, sizeof *saves);
    if (saves == NULL) {
        cs2_set_error("out of memory for the save store");
        return NULL;
    }
    snprintf(saves->folder, sizeof saves->folder, "%s", folder);
    return saves;
}

void cs2_saves_free(cs2_saves *saves) {
    free(saves);
}

const char *cs2_saves_folder(const cs2_saves *saves) {
    return saves == NULL ? NULL : saves->folder;
}

/* The original's own name for a slot's file, so a folder reads the same way. */
static int path_of(const cs2_saves *saves, int slot, char *into, size_t into_size) {
    if (saves == NULL || slot < 0 || slot >= SLOT_LIMIT) return -1;
    snprintf(into, into_size, "%s/save%04d.dat", saves->folder, slot);
    return 0;
}

int cs2_saves_exists(const cs2_saves *saves, int slot) {
    char path[600];
    struct stat info;
    if (path_of(saves, slot, path, sizeof path) != 0) return 0;
    return stat(path, &info) == 0 && info.st_size > MAGIC_BYTES;
}

/*
 * When a slot was written, out of its own header rather than the file system:
 * a folder copied between devices keeps the game's own order that way.
 */
static int64_t written_at(const cs2_saves *saves, int slot) {
    char path[600];
    if (path_of(saves, slot, path, sizeof path) != 0) return -1;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    uint8_t head[MAGIC_BYTES + 8];
    size_t got = fread(head, 1, sizeof head, file);
    fclose(file);
    if (got != sizeof head || memcmp(head, MAGIC, MAGIC_BYTES) != 0) return -1;
    uint64_t when = (uint64_t) cs2_u32(head, sizeof head, MAGIC_BYTES)
                  | ((uint64_t) cs2_u32(head, sizeof head, MAGIC_BYTES + 4) << 32);
    return (int64_t) when;
}

int cs2_saves_newest(const cs2_saves *saves, int from, int to) {
    int newest = -1;
    int64_t best = -1;
    if (from < 0) from = 0;
    if (to >= SLOT_LIMIT) to = SLOT_LIMIT - 1;
    for (int slot = from; slot <= to; slot++) {
        int64_t when = written_at(saves, slot);
        if (when < 0) continue;
        if (newest < 0 || when >= best) {
            newest = slot;
            best = when;
        }
    }
    return newest;
}

int cs2_saves_write(const cs2_saves *saves, int slot, const cs2_save *from) {
    char path[600];
    if (path_of(saves, slot, path, sizeof path) != 0 || from == NULL) {
        cs2_set_error("slot %d is not a save this game has", slot);
        return -1;
    }

    builder out = { 0 }, body = { 0 };
    put(&out, MAGIC, MAGIC_BYTES);
    put64(&out, (uint64_t) (from->when == 0 ? (int64_t) time(NULL) : from->when));

    put_text(&body, from->scenario);
    put32(&body, from->cursor);
    put_chunk(&out, TAG_SCENE, &body);
    body.size = 0;

    put32(&body, (uint32_t) from->flag_count);
    for (size_t i = 0; i < from->flag_count; i++) {
        put32(&body, from->flags[i].key);
        put32(&body, (uint32_t) from->flags[i].value);
    }
    put_chunk(&out, TAG_FLAG, &body);
    body.size = 0;

    put32(&body, (uint32_t) from->string_count);
    for (size_t i = 0; i < from->string_count; i++) {
        put32(&body, from->strings[i].key);
        put_text(&body, from->strings[i].text);
    }
    put_chunk(&out, TAG_STRS, &body);
    body.size = 0;

    put32(&body, (uint32_t) from->mark_count);
    for (size_t i = 0; i < from->mark_count; i++) {
        put_text(&body, from->marks[i].name);
        put32(&body, (uint32_t) from->marks[i].mark);
    }
    put_chunk(&out, TAG_READ, &body);

    int failed = out.failed || body.failed;
    free(body.data);
    if (failed) {
        free(out.data);
        cs2_set_error("out of memory writing save %d", slot);
        return -1;
    }

    /*
     * Written beside itself and moved into place: a console loses power in the
     * middle of a write like any other machine, and a half-written save that
     * replaced a good one is the one bug a save system must not have.
     */
    char temporary[620];
    snprintf(temporary, sizeof temporary, "%s.new", path);
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        free(out.data);
        cs2_set_error("save %d cannot be written to %s", slot, saves->folder);
        return -1;
    }
    size_t written = fwrite(out.data, 1, out.size, file);
    int closed = fclose(file);
    free(out.data);
    if (written != out.size || closed != 0) {
        remove(temporary);
        cs2_set_error("save %d was not written whole", slot);
        return -1;
    }
    remove(path);
    if (rename(temporary, path) != 0) {
        remove(temporary);
        cs2_set_error("save %d cannot be put in place", slot);
        return -1;
    }
    return 0;
}

int cs2_saves_read(const cs2_saves *saves, int slot, cs2_save *into) {
    char path[600];
    if (path_of(saves, slot, path, sizeof path) != 0 || into == NULL) {
        cs2_set_error("slot %d is not a save this game has", slot);
        return -1;
    }
    cs2_bytes file = { 0 };
    if (cs2_read_file(path, SAVE_BYTES, &file) != 0) return -1;
    if (file.size < MAGIC_BYTES + 8 || memcmp(file.data, MAGIC, MAGIC_BYTES) != 0) {
        cs2_bytes_free(&file);
        cs2_set_error("%s is not a save this engine wrote", path);
        return -1;
    }

    memset(into, 0, sizeof *into);
    scanner in = { file.data, file.size, MAGIC_BYTES, 0 };
    into->when = (int64_t) take64(&in);
    while (!in.failed && in.at + 8 <= in.size) {
        uint32_t tag = take32(&in);
        uint32_t size = take32(&in);
        if (in.failed || size > in.size - in.at) break;
        size_t past = in.at + size;
        if (tag == TAG_SCENE) {
            take_text(&in, into->scenario, sizeof into->scenario);
            into->cursor = take32(&in);
        } else if (tag == TAG_FLAG) {
            uint32_t count = take32(&in);
            for (uint32_t i = 0; i < count && !in.failed; i++) {
                uint32_t key = take32(&in);
                int32_t value = (int32_t) take32(&in);
                if (!in.failed) cs2_save_set_flag(into, key, value);
            }
        } else if (tag == TAG_STRS) {
            uint32_t count = take32(&in);
            for (uint32_t i = 0; i < count && !in.failed; i++) {
                uint32_t key = take32(&in);
                char text[1024];
                take_text(&in, text, sizeof text);
                if (!in.failed) cs2_save_set_string(into, key, text);
            }
        } else if (tag == TAG_READ) {
            uint32_t count = take32(&in);
            for (uint32_t i = 0; i < count && !in.failed; i++) {
                char name[64];
                take_text(&in, name, sizeof name);
                int32_t mark = (int32_t) take32(&in);
                if (!in.failed) cs2_save_set_mark(into, name, mark);
            }
        }
        in.at = past;                    /* past a chunk this build skipped */
    }
    cs2_bytes_free(&file);
    if (in.failed) {
        cs2_save_clear(into);
        cs2_set_error("save %d stops in the middle of itself", slot);
        return -1;
    }
    return 0;
}

int cs2_saves_delete(const cs2_saves *saves, int slot) {
    char path[600];
    if (path_of(saves, slot, path, sizeof path) != 0) return -1;
    return remove(path) == 0 ? 0 : -1;
}

int cs2_saves_copy(const cs2_saves *saves, int from, int to) {
    cs2_save save = { 0 };
    if (cs2_saves_read(saves, from, &save) != 0) return -1;
    int written = cs2_saves_write(saves, to, &save);
    cs2_save_clear(&save);
    return written;
}

int cs2_saves_exchange(const cs2_saves *saves, int a, int b) {
    cs2_save left = { 0 }, right = { 0 };
    int have_left = cs2_saves_read(saves, a, &left) == 0;
    int have_right = cs2_saves_read(saves, b, &right) == 0;
    int failed = 0;
    if (have_left) failed |= cs2_saves_write(saves, b, &left) != 0;
    else cs2_saves_delete(saves, b);
    if (have_right) failed |= cs2_saves_write(saves, a, &right) != 0;
    else cs2_saves_delete(saves, a);
    cs2_save_clear(&left);
    cs2_save_clear(&right);
    return failed ? -1 : 0;
}
