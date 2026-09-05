#include "crypto.h"

#include <string.h>

#include "blowfish_tables.h"

#define ROUNDS 16

static uint32_t f(const cs2_blowfish *cipher, uint32_t x) {
    uint32_t a = (x >> 24) & 0xff;
    uint32_t b = (x >> 16) & 0xff;
    uint32_t c = (x >> 8) & 0xff;
    uint32_t d = x & 0xff;
    return ((cipher->s[0][a] + cipher->s[1][b]) ^ cipher->s[2][c]) + cipher->s[3][d];
}

static void encipher(const cs2_blowfish *cipher, uint32_t block[2]) {
    uint32_t left = block[0];
    uint32_t right = block[1];
    for (int i = 0; i < ROUNDS; i++) {
        left ^= cipher->p[i];
        right ^= f(cipher, left);
        uint32_t swap = left;
        left = right;
        right = swap;
    }
    uint32_t swap = left;
    left = right;
    right = swap;
    right ^= cipher->p[ROUNDS];
    left ^= cipher->p[ROUNDS + 1];
    block[0] = left;
    block[1] = right;
}

void cs2_blowfish_decipher(const cs2_blowfish *cipher, uint32_t block[2]) {
    uint32_t left = block[0];
    uint32_t right = block[1];
    for (int i = ROUNDS + 1; i > 1; i--) {
        left ^= cipher->p[i];
        right ^= f(cipher, left);
        uint32_t swap = left;
        left = right;
        right = swap;
    }
    uint32_t swap = left;
    left = right;
    right = swap;
    right ^= cipher->p[1];
    left ^= cipher->p[0];
    block[0] = left;
    block[1] = right;
}

void cs2_blowfish_init(cs2_blowfish *cipher, const uint8_t *key, size_t key_size) {
    memcpy(cipher->p, blowfish_p, sizeof cipher->p);
    memcpy(cipher->s, blowfish_s, sizeof cipher->s);
    if (key == NULL || key_size == 0) return;

    size_t j = 0;
    for (size_t i = 0; i < 18; i++) {
        uint32_t data = 0;
        for (int k = 0; k < 4; k++) {
            data = (data << 8) | key[j];
            if (++j >= key_size) j = 0;
        }
        cipher->p[i] ^= data;
    }
    uint32_t block[2] = { 0, 0 };
    for (size_t i = 0; i < 18; i += 2) {
        encipher(cipher, block);
        cipher->p[i] = block[0];
        cipher->p[i + 1] = block[1];
    }
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 256; k += 2) {
            encipher(cipher, block);
            cipher->s[i][k] = block[0];
            cipher->s[i][k + 1] = block[1];
        }
    }
}

void cs2_blowfish_decipher_le(const cs2_blowfish *cipher, uint8_t *data, size_t size) {
    for (size_t at = 0; at + 8 <= size; at += 8) {
        uint32_t block[2];
        block[0] = (uint32_t) data[at] | ((uint32_t) data[at + 1] << 8)
                 | ((uint32_t) data[at + 2] << 16) | ((uint32_t) data[at + 3] << 24);
        block[1] = (uint32_t) data[at + 4] | ((uint32_t) data[at + 5] << 8)
                 | ((uint32_t) data[at + 6] << 16) | ((uint32_t) data[at + 7] << 24);
        cs2_blowfish_decipher(cipher, block);
        for (int i = 0; i < 4; i++) {
            data[at + i] = (uint8_t) (block[0] >> (8 * i));
            data[at + 4 + i] = (uint8_t) (block[1] >> (8 * i));
        }
    }
}

#define N 624
#define M 397
#define MATRIX_A 0x9908b0dfu
#define SIGN_MASK 0x80000000u
#define LOWER_MASK 0x7fffffffu

void cs2_mt_seed(cs2_mt *generator, uint32_t seed) {
    for (int i = 0; i < N; i++) {
        uint32_t upper = seed & 0xffff0000u;
        seed = 69069u * seed + 1u;
        generator->state[i] = upper | ((seed & 0xffff0000u) >> 16);
        seed = 69069u * seed + 1u;
    }
    generator->index = N;
}

uint32_t cs2_mt_next(cs2_mt *generator) {
    if (generator->index >= N) {
        int k = 0;
        for (; k < N - M; k++) {
            uint32_t y = (generator->state[k] & SIGN_MASK) | (generator->state[k + 1] & LOWER_MASK);
            generator->state[k] = generator->state[k + M] ^ (y >> 1) ^ ((y & 1) ? MATRIX_A : 0);
        }
        for (; k < N - 1; k++) {
            uint32_t y = (generator->state[k] & SIGN_MASK) | (generator->state[k + 1] & LOWER_MASK);
            generator->state[k] = generator->state[k + M - N] ^ (y >> 1) ^ ((y & 1) ? MATRIX_A : 0);
        }
        uint32_t y = (generator->state[N - 1] & SIGN_MASK) | (generator->state[0] & LOWER_MASK);
        generator->state[N - 1] = generator->state[M - 1] ^ (y >> 1) ^ ((y & 1) ? MATRIX_A : 0);
        generator->index = 0;
    }
    uint32_t y = generator->state[generator->index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}
