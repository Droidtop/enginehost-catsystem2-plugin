#include "kif.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "broker.h"
#include "crypto.h"
#include "pe.h"

#define MAX_ENTRIES 200000
#define ENTRY_SIZE 0x48
#define NAME_SIZE 0x40
#define MAX_BINARY (64u * 1024 * 1024)

static const char ALPHABET[] = "zyxwvutsrqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA";

typedef struct {
    char name[NAME_SIZE + 1];
    uint32_t offset;
    uint32_t size;
} entry;

struct cs2_kif {
    FILE *file;
    long length;
    entry *entries;
    size_t count;
    cs2_blowfish cipher;
    int encrypted;
};

uint32_t cs2_game_key_encode(const uint8_t *passphrase, size_t size) {
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (int i = 0; i < 256; i++) {
            uint32_t value = (uint32_t) i << 24;
            for (int bit = 0; bit < 8; bit++) {
                value = (value & 0x80000000u) ? (value << 1) ^ 0x04C11DB7u : value << 1;
            }
            table[i] = value;
        }
        ready = 1;
    }
    uint32_t key = 0xffffffffu;
    for (size_t i = 0; i < size; i++) {
        key = ~table[((key >> 24) ^ passphrase[i]) & 0xff] ^ (key << 8);
    }
    return key;
}

/* Pulls the passphrase out of one binary, if it carries one. */
static int key_from_bytes(cs2_bytes image, const char *label, cs2_game_key *out) {
    int found = -1;
    cs2_pe *pe = cs2_pe_open(image.data, image.size);
    if (pe != NULL) {
        size_t code_size = 0;
        const uint8_t *code = cs2_pe_resource(pe, "DATA", "V_CODE2", &code_size);
        if (code != NULL && code_size >= 8 && code_size < 4096) {
            size_t key_size = 0;
            const uint8_t *key_code = cs2_pe_resource(pe, "KEY", "KEY_CODE", &key_size);
            uint8_t blowfish_key[256];
            size_t blowfish_key_size;
            if (key_code != NULL && key_size > 0 && key_size <= sizeof blowfish_key) {
                for (size_t i = 0; i < key_size; i++) blowfish_key[i] = (uint8_t) (key_code[i] ^ 0xCD);
                blowfish_key_size = key_size;
            } else {
                memcpy(blowfish_key, "windmill", 8);
                blowfish_key_size = 8;
            }
            uint8_t plain[4096];
            memcpy(plain, code, code_size);
            cs2_blowfish cipher;
            cs2_blowfish_init(&cipher, blowfish_key, blowfish_key_size);
            cs2_blowfish_decipher_le(&cipher, plain, code_size / 8 * 8);
            size_t end = 0;
            while (end < code_size && plain[end] != '\0') end++;
            if (end > 0 && end < sizeof out->passphrase) {
                memcpy(out->passphrase, plain, end);
                out->passphrase[end] = '\0';
                out->key = cs2_game_key_encode(plain, end);
                size_t path_length = strlen(label);
                if (path_length >= sizeof out->source) path_length = sizeof out->source - 1;
                memcpy(out->source, label, path_length);
                out->source[path_length] = '\0';
                out->found = 1;
                found = 0;
            }
        }
        cs2_pe_free(pe);
    }
    cs2_bytes_free(&image);
    return found;
}

static int key_from_binary(const char *path, cs2_game_key *out) {
    cs2_bytes image;
    if (cs2_read_file(path, MAX_BINARY, &image) != 0) return -1;
    return key_from_bytes(image, path, out);
}

static int key_from_binary_fd(int fd, const char *label, cs2_game_key *out) {
    cs2_bytes image;
    if (cs2_read_file_fd(fd, MAX_BINARY, &image) != 0) return -1;
    return key_from_bytes(image, label, out);
}

static int has_suffix(const char *name, const char *suffix) {
    size_t name_length = strlen(name);
    size_t suffix_length = strlen(suffix);
    return name_length > suffix_length && cs2_ieq(name + name_length - suffix_length, suffix);
}

void cs2_game_key_read(const char *game_root, cs2_game_key *out) {
    memset(out, 0, sizeof *out);
    static const char *suffixes[] = { ".bin", ".exe", ".dll" };
    for (size_t s = 0; s < sizeof suffixes / sizeof suffixes[0]; s++) {
        /* Sorted by name inside each suffix, so the choice does not depend on
           the order the file system happens to hand the folder over in. */
        char names[512][512];
        size_t count = 0;
        DIR *directory = opendir(game_root);
        if (directory == NULL) return;
        for (struct dirent *item; (item = readdir(directory)) != NULL && count < 512; ) {
            if (has_suffix(item->d_name, suffixes[s])) {
                snprintf(names[count], sizeof names[count], "%s", item->d_name);
                count++;
            }
        }
        closedir(directory);
        for (size_t i = 0; i + 1 < count; i++) {
            for (size_t j = i + 1; j < count; j++) {
                if (strcmp(names[j], names[i]) < 0) {
                    char swap[512];
                    memcpy(swap, names[i], sizeof swap);
                    memcpy(names[i], names[j], sizeof swap);
                    memcpy(names[j], swap, sizeof swap);
                }
            }
        }
        for (size_t i = 0; i < count; i++) {
            char path[2048];
            if (snprintf(path, sizeof path, "%s/%s", game_root, names[i]) >= (int) sizeof path) continue;
            if (key_from_binary(path, out) == 0) return;
        }
    }
}

/* Same search, over a host broker's listing instead of opendir/fopen (docs/engine-sandbox.md). */
void cs2_game_key_read_via_broker(const cs2_broker *broker, cs2_game_key *out) {
    memset(out, 0, sizeof *out);
    static const char *suffixes[] = { ".bin", ".exe", ".dll" };
    char (*all)[256] = malloc(CS2_BROKER_LIST_MAX * sizeof *all);
    if (all == NULL) return;
    int total = broker->list(broker->ctx, "", all, CS2_BROKER_LIST_MAX);
    if (total < 0) {
        free(all);
        return;
    }
    for (size_t s = 0; s < sizeof suffixes / sizeof suffixes[0]; s++) {
        /* Sorted by name inside each suffix, so the choice does not depend
           on the order the broker's own listing happens to arrive in. */
        char names[512][256];
        size_t count = 0;
        for (int i = 0; i < total && count < 512; i++) {
            if (has_suffix(all[i], suffixes[s])) {
                snprintf(names[count], sizeof names[count], "%s", all[i]);
                count++;
            }
        }
        for (size_t i = 0; i + 1 < count; i++) {
            for (size_t j = i + 1; j < count; j++) {
                if (strcmp(names[j], names[i]) < 0) {
                    char swap[256];
                    memcpy(swap, names[i], sizeof swap);
                    memcpy(names[i], names[j], sizeof swap);
                    memcpy(names[j], swap, sizeof swap);
                }
            }
        }
        for (size_t i = 0; i < count; i++) {
            int fd = broker->open_read(broker->ctx, names[i]);
            if (fd < 0) continue;
            if (key_from_binary_fd(fd, names[i], out) == 0) {
                free(all);
                return;
            }
        }
    }
    free(all);
}

/*
 * Unscrambles one entry name. Letters are rotated through a reversed alphabet
 * by a shift that advances one position per character; digits, punctuation and
 * the dot are left alone, which is why an encrypted archive's table still reads
 * as "xxxxx_1.xxx".
 */
static void decipher_name(char *name, uint32_t key) {
    uint32_t shift = ((key >> 24) + (key >> 16) + (key >> 8) + key) & 0xff;
    for (size_t i = 0; i < NAME_SIZE && name[i] != '\0'; i++) {
        const char *found = memchr(ALPHABET, name[i], sizeof ALPHABET - 1);
        if (found != NULL) {
            int index = (int) (found - ALPHABET) - (int) (shift % 0x34);
            if (index < 0) index += 0x34;
            name[i] = ALPHABET[0x33 - index];
        }
        shift++;
    }
}

static void put(cs2_kif *archive, const char *name, uint32_t offset, uint32_t size) {
    if (archive->count >= MAX_ENTRIES) return;
    entry *item = &archive->entries[archive->count];
    snprintf(item->name, sizeof item->name, "%s", name);
    item->offset = offset;
    item->size = size;
    archive->count++;
}

/*
 * Everything past the point of already having an open FILE*:
 * cs2_kif_open (a real path) and cs2_kif_open_fd (a host-brokered
 * descriptor, isolated runtime, docs/engine-sandbox.md) differ only in
 * how that FILE* was obtained. label is for cs2_set_error alone.
 */
static cs2_kif *open_from_file(FILE *file, const char *label, const cs2_game_key *key) {
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        cs2_set_error("cannot measure %s", label);
        return NULL;
    }
    long length = ftell(file);
    uint8_t head[8];
    rewind(file);
    if (length < 8 || fread(head, 1, 8, file) != 8
        || head[0] != 'K' || head[1] != 'I' || head[2] != 'F' || head[3] != 0) {
        fclose(file);
        cs2_set_error("%s is not a KIF archive", label);
        return NULL;
    }
    uint32_t count = cs2_u32(head, sizeof head, 4);
    if (count == 0 || count > MAX_ENTRIES
        || (uint64_t) 8 + (uint64_t) count * ENTRY_SIZE > (uint64_t) length) {
        fclose(file);
        cs2_set_error("%s says it holds %u files, which cannot be", label, count);
        return NULL;
    }
    size_t table_size = (size_t) count * ENTRY_SIZE;
    uint8_t *table = malloc(table_size);
    if (table == NULL || fread(table, 1, table_size, file) != table_size) {
        free(table);
        fclose(file);
        cs2_set_error("%s ended inside its own file table", label);
        return NULL;
    }

    cs2_kif *archive = calloc(1, sizeof *archive);
    if (archive == NULL) {
        free(table);
        fclose(file);
        cs2_set_error("out of memory opening %s", label);
        return NULL;
    }
    archive->file = file;
    archive->length = length;
    archive->entries = calloc(count, sizeof *archive->entries);
    if (archive->entries == NULL) {
        free(table);
        cs2_kif_close(archive);
        cs2_set_error("out of memory opening %s", label);
        return NULL;
    }

    archive->encrypted = memcmp(table, "__key__.dat", 12) == 0;
    if (!archive->encrypted) {
        for (uint32_t i = 0; i < count; i++) {
            size_t base = (size_t) i * ENTRY_SIZE;
            char name[NAME_SIZE + 1];
            memcpy(name, table + base, NAME_SIZE);
            name[NAME_SIZE] = '\0';
            uint32_t offset = cs2_u32(table, table_size, base + 0x40);
            uint32_t size = cs2_u32(table, table_size, base + 0x44);
            if (name[0] != '\0' && (uint64_t) offset + size <= (uint64_t) length) {
                put(archive, name, offset, size);
            }
        }
    } else if (key == NULL || !key->found) {
        free(table);
        cs2_kif_close(archive);
        cs2_set_error("%s is encrypted and no key was found in the game's own binaries", label);
        return NULL;
    } else {
        cs2_mt generator;
        cs2_mt_seed(&generator, cs2_u32(table, table_size, 0x44));
        uint32_t seed = cs2_mt_next(&generator);
        uint8_t seed_bytes[4] = {
            (uint8_t) seed, (uint8_t) (seed >> 8), (uint8_t) (seed >> 16), (uint8_t) (seed >> 24)
        };
        cs2_blowfish_init(&archive->cipher, seed_bytes, sizeof seed_bytes);
        for (uint32_t i = 1; i < count; i++) {
            size_t base = (size_t) i * ENTRY_SIZE;
            char name[NAME_SIZE + 1];
            memcpy(name, table + base, NAME_SIZE);
            name[NAME_SIZE] = '\0';
            uint32_t block[2];
            block[0] = cs2_u32(table, table_size, base + 0x40) + i;
            block[1] = cs2_u32(table, table_size, base + 0x44);
            cs2_blowfish_decipher(&archive->cipher, block);
            cs2_mt_seed(&generator, key->key + i);
            decipher_name(name, cs2_mt_next(&generator));
            if (name[0] != '\0' && (uint64_t) block[0] + block[1] <= (uint64_t) length) {
                put(archive, name, block[0], block[1]);
            }
        }
    }
    free(table);
    return archive;
}

cs2_kif *cs2_kif_open(const char *path, const cs2_game_key *key) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        cs2_set_error("cannot open %s", path);
        return NULL;
    }
    return open_from_file(file, path, key);
}

/*
 * Same archive format, a descriptor the host already opened instead of a
 * path this process could resolve itself (docs/engine-sandbox.md "Host
 * file service design"). fd is consumed either way: fdopen takes it over
 * on success, and this closes it itself on failure.
 */
cs2_kif *cs2_kif_open_fd(int fd, const char *label, const cs2_game_key *key) {
    FILE *file = fdopen(fd, "rb");
    if (file == NULL) {
        close(fd);
        cs2_set_error("cannot open %s", label);
        return NULL;
    }
    return open_from_file(file, label, key);
}

size_t cs2_kif_count(const cs2_kif *archive) {
    return archive == NULL ? 0 : archive->count;
}

const char *cs2_kif_name(const cs2_kif *archive, size_t index) {
    return archive == NULL || index >= archive->count ? NULL : archive->entries[index].name;
}

static const entry *find(const cs2_kif *archive, const char *name) {
    if (archive == NULL) return NULL;
    for (size_t i = 0; i < archive->count; i++) {
        if (cs2_ieq(archive->entries[i].name, name)) return &archive->entries[i];
    }
    return NULL;
}

int cs2_kif_contains(const cs2_kif *archive, const char *name) {
    return find(archive, name) != NULL;
}

int cs2_kif_read(cs2_kif *archive, const char *name, cs2_bytes *out) {
    out->data = NULL;
    out->size = 0;
    const entry *item = find(archive, name);
    if (item == NULL) return -1;
    uint8_t *data = item->size == 0 ? NULL : malloc(item->size);
    if (item->size > 0 && data == NULL) {
        cs2_set_error("out of memory reading %s", name);
        return -1;
    }
    if (item->size > 0 && (fseek(archive->file, (long) item->offset, SEEK_SET) != 0
                           || fread(data, 1, item->size, archive->file) != item->size)) {
        free(data);
        cs2_set_error("%s ended inside the entry %s", "the archive", name);
        return -1;
    }
    if (archive->encrypted && data != NULL) {
        cs2_blowfish_decipher_le(&archive->cipher, data, item->size / 8 * 8);
    }
    out->data = data;
    out->size = item->size;
    return 0;
}

void cs2_kif_close(cs2_kif *archive) {
    if (archive == NULL) return;
    if (archive->file != NULL) fclose(archive->file);
    free(archive->entries);
    free(archive);
}
