#include "files.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ARCHIVES 64
#define MAX_LOOSE (256u * 1024 * 1024)

typedef struct {
    char name[256];
    char path[1024];
    cs2_kif *archive;
    int tried;
} slot;

struct cs2_files {
    char root[1024];
    cs2_game_key key;
    slot archives[MAX_ARCHIVES];
    size_t count;
};

static void lower(char *text) {
    for (; *text != '\0'; text++) {
        if (*text >= 'A' && *text <= 'Z') *text = (char) (*text - 'A' + 'a');
    }
}

static int has_int_suffix(const char *name) {
    size_t length = strlen(name);
    return length > 4 && cs2_ieq(name + length - 4, ".int");
}

cs2_files *cs2_files_open(const char *game_root) {
    DIR *directory = opendir(game_root);
    if (directory == NULL) {
        cs2_set_error("the game folder %s cannot be read", game_root);
        return NULL;
    }
    cs2_files *files = calloc(1, sizeof *files);
    if (files == NULL) {
        closedir(directory);
        cs2_set_error("out of memory");
        return NULL;
    }
    snprintf(files->root, sizeof files->root, "%s", game_root);
    for (struct dirent *item; (item = readdir(directory)) != NULL && files->count < MAX_ARCHIVES; ) {
        if (!has_int_suffix(item->d_name)) continue;
        slot *entry = &files->archives[files->count];
        snprintf(entry->name, sizeof entry->name, "%s", item->d_name);
        lower(entry->name);
        snprintf(entry->path, sizeof entry->path, "%s/%s", game_root, item->d_name);
        files->count++;
    }
    closedir(directory);
    for (size_t i = 0; i + 1 < files->count; i++) {
        for (size_t j = i + 1; j < files->count; j++) {
            if (strcmp(files->archives[j].name, files->archives[i].name) < 0) {
                slot swap = files->archives[i];
                files->archives[i] = files->archives[j];
                files->archives[j] = swap;
            }
        }
    }
    cs2_game_key_read(game_root, &files->key);
    return files;
}

void cs2_files_close(cs2_files *files) {
    if (files == NULL) return;
    for (size_t i = 0; i < files->count; i++) cs2_kif_close(files->archives[i].archive);
    free(files);
}

const char *cs2_files_root(const cs2_files *files) {
    return files->root;
}

const cs2_game_key *cs2_files_key(const cs2_files *files) {
    return &files->key;
}

size_t cs2_files_archive_count(const cs2_files *files) {
    return files->count;
}

const char *cs2_files_archive_name(const cs2_files *files, size_t index) {
    return index >= files->count ? NULL : files->archives[index].name;
}

cs2_kif *cs2_files_archive(cs2_files *files, const char *archive_name) {
    char wanted[256];
    snprintf(wanted, sizeof wanted, "%s", archive_name);
    lower(wanted);
    if (!has_int_suffix(wanted)) {
        size_t length = strlen(wanted);
        if (length + 4 < sizeof wanted) memcpy(wanted + length, ".int", 5);
    }
    for (size_t i = 0; i < files->count; i++) {
        slot *entry = &files->archives[i];
        if (strcmp(entry->name, wanted) != 0) continue;
        if (entry->archive == NULL && !entry->tried) {
            entry->tried = 1;
            entry->archive = cs2_kif_open(entry->path, &files->key);
            if (entry->archive == NULL) cs2_log("%s", cs2_error());
        }
        return entry->archive;
    }
    return NULL;
}

/* A loose file under the game folder, refusing any path that leaves it. */
static int read_loose(const cs2_files *files, const char *relative, cs2_bytes *out) {
    if (strstr(relative, "..") != NULL || relative[0] == '/') return -1;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", files->root, relative);
    return cs2_read_file(path, MAX_LOOSE, out);
}

static void strip_extension(char *name) {
    char *dot = strrchr(name, '.');
    if (dot != NULL && dot != name) *dot = '\0';
}

int cs2_files_read(cs2_files *files, const char *path, cs2_bytes *out) {
    out->data = NULL;
    out->size = 0;

    char normalised[1024];
    snprintf(normalised, sizeof normalised, "%s", path);
    for (char *c = normalised; *c != '\0'; c++) {
        if (*c == '\\') *c = '/';
    }
    char *slash = strrchr(normalised, '/');
    if (slash != NULL) {
        *slash = '\0';
        const char *name = slash + 1;
        char folder[1024];
        snprintf(folder, sizeof folder, "%s", normalised);
        strip_extension(folder);
        char loose[2048];
        snprintf(loose, sizeof loose, "%s/%s", folder, name);
        if (read_loose(files, loose, out) == 0) return 0;
        cs2_kif *archive = cs2_files_archive(files, normalised);
        if (archive == NULL) {
            cs2_set_error("this game has no %s", path);
            return -1;
        }
        if (cs2_kif_read(archive, name, out) != 0) {
            cs2_set_error("%s holds no %s", normalised, name);
            return -1;
        }
        return 0;
    }

    if (read_loose(files, normalised, out) == 0) return 0;
    for (size_t i = 0; i < files->count; i++) {
        cs2_kif *archive = cs2_files_archive(files, files->archives[i].name);
        if (archive != NULL && cs2_kif_contains(archive, normalised)) {
            return cs2_kif_read(archive, normalised, out);
        }
    }
    cs2_set_error("this game has no %s", path);
    return -1;
}
