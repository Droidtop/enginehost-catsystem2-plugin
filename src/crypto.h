/*
 * The two pieces of arithmetic a CatSystem2 archive is locked with.
 *
 * Blowfish is the stock cipher; the engine loads a block big-endian when it
 * encrypts and little-endian when it decrypts, so both orders are here. The
 * Mersenne Twister is stock MT19937 except in its seeding: instead of Knuth's
 * initialiser the engine fills the state from the high halves of two steps of a
 * 69069 linear congruential generator per word, so a stock MT19937 gives
 * different numbers from the same seed.
 */
#ifndef CS2_CRYPTO_H
#define CS2_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t p[18];
    uint32_t s[4][256];
} cs2_blowfish;

void cs2_blowfish_init(cs2_blowfish *cipher, const uint8_t *key, size_t key_size);
void cs2_blowfish_decipher(const cs2_blowfish *cipher, uint32_t block[2]);
/* Deciphers whole eight-byte blocks in place, reading each one little-endian. */
void cs2_blowfish_decipher_le(const cs2_blowfish *cipher, uint8_t *data, size_t size);

typedef struct {
    uint32_t state[624];
    int index;
} cs2_mt;

void cs2_mt_seed(cs2_mt *generator, uint32_t seed);
uint32_t cs2_mt_next(cs2_mt *generator);

#endif
