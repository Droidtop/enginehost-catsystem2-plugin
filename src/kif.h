/*
 * CatSystem2 archives, and the per-game key they are locked with.
 *
 * A ".int" (KIF) archive is a header and then one 0x48-byte entry per file: a
 * name padded to 0x40 bytes, a 32-bit offset and a 32-bit size. When the first
 * entry is named "__key__.dat" the archive is encrypted, and that entry is not
 * a file at all: its size field seeds the engine's Mersenne Twister, whose first
 * number is the Blowfish key over every offset and size and over every file's
 * bytes. Entry names are scrambled separately, with a number drawn from the same
 * generator re-seeded from the game's own key.
 *
 * That game key is a fold of a short passphrase every release keeps inside its
 * own executable: resource DATA/V_CODE2, Blowfish-decrypted under KEY/KEY_CODE
 * with each byte exclusive-ored by 0xCD, or under the constant "windmill" on
 * releases with no such resource. The executable is usually not the .exe the
 * player clicks - that is a launcher - so every binary in the folder is looked
 * at and the first carrying V_CODE2 wins.
 */
#ifndef CS2_KIF_H
#define CS2_KIF_H

#include "cs2.h"

typedef struct {
    int found;
    char source[1024];      /* the file the passphrase came from, for the log */
    char passphrase[64];
    uint32_t key;
} cs2_game_key;

/* Looks through a game folder for the binary that carries the key. */
void cs2_game_key_read(const char *game_root, cs2_game_key *out);

/* The engine's passphrase fold: a normal, MSB-first CRC-32, inverted per byte. */
uint32_t cs2_game_key_encode(const uint8_t *passphrase, size_t size);

typedef struct cs2_kif cs2_kif;

/*
 * Opens an archive. The key is consulted only for encrypted archives; without
 * one an encrypted archive fails and says so.
 */
cs2_kif *cs2_kif_open(const char *path, const cs2_game_key *key);

size_t cs2_kif_count(const cs2_kif *archive);
const char *cs2_kif_name(const cs2_kif *archive, size_t index);
int cs2_kif_contains(const cs2_kif *archive, const char *name);

/* Reads and decrypts one entry. Returns 0, or -1 when there is no such name. */
int cs2_kif_read(cs2_kif *archive, const char *name, cs2_bytes *out);

void cs2_kif_close(cs2_kif *archive);

#endif
