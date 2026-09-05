#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t) (value >> 24);
    out[1] = (uint8_t) (value >> 16);
    out[2] = (uint8_t) (value >> 8);
    out[3] = (uint8_t) value;
}

static int chunk(FILE *file, const char *type, const uint8_t *data, size_t size) {
    uint8_t header[8];
    put32(header, (uint32_t) size);
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, file) != 8) return -1;
    if (size > 0 && fwrite(data, 1, size, file) != size) return -1;
    uLong crc = crc32(0, (const Bytef *) type, 4);
    if (size > 0) crc = crc32(crc, data, (uInt) size);
    uint8_t tail[4];
    put32(tail, (uint32_t) crc);
    return fwrite(tail, 1, 4, file) == 4 ? 0 : -1;
}

int cs2_png_write(const char *path, const uint32_t *pixels, int width, int height) {
    if (width <= 0 || height <= 0) {
        cs2_set_error("cannot write a %dx%d image", width, height);
        return -1;
    }
    /* One filter byte per row, then the row as red, green, blue, alpha. */
    size_t raw_size = (size_t) height * (1 + (size_t) width * 4);
    uint8_t *raw = malloc(raw_size);
    if (raw == NULL) {
        cs2_set_error("out of memory writing %s", path);
        return -1;
    }
    size_t at = 0;
    for (int y = 0; y < height; y++) {
        raw[at++] = 0;
        for (int x = 0; x < width; x++) {
            uint32_t pixel = pixels[(size_t) y * width + x];
            raw[at++] = (uint8_t) (pixel >> 16);
            raw[at++] = (uint8_t) (pixel >> 8);
            raw[at++] = (uint8_t) pixel;
            raw[at++] = (uint8_t) (pixel >> 24);
        }
    }
    uLongf packed_size = compressBound((uLong) raw_size);
    uint8_t *packed = malloc(packed_size);
    if (packed == NULL || compress2(packed, &packed_size, raw, (uLong) raw_size, 6) != Z_OK) {
        free(raw);
        free(packed);
        cs2_set_error("could not compress %s", path);
        return -1;
    }
    free(raw);

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        free(packed);
        cs2_set_error("cannot write %s", path);
        return -1;
    }
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    uint8_t header[13];
    put32(header, (uint32_t) width);
    put32(header + 4, (uint32_t) height);
    header[8] = 8;    /* bits per channel */
    header[9] = 6;    /* colour type: truecolour with alpha */
    header[10] = 0;
    header[11] = 0;
    header[12] = 0;
    int failed = fwrite(signature, 1, 8, file) != 8
              || chunk(file, "IHDR", header, sizeof header) != 0
              || chunk(file, "IDAT", packed, packed_size) != 0
              || chunk(file, "IEND", NULL, 0) != 0;
    free(packed);
    fclose(file);
    if (failed) {
        cs2_set_error("could not write all of %s", path);
        return -1;
    }
    return 0;
}
