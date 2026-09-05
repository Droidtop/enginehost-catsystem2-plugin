#include "hg3.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PIXELS (64u << 20)

/* The command stream, read as bits from the low end of each byte. */
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t at;
    int bit;
    int failed;
} bits;

static int next_bit(bits *stream) {
    if (stream->bit > 7) {
        stream->at++;
        stream->bit = 0;
    }
    if (stream->at >= stream->size) {
        stream->failed = 1;
        return 0;
    }
    return (stream->data[stream->at] >> stream->bit++) & 1;
}

/*
 * An Elias gamma value: as many zero bits as the number has bits below its
 * leading one, then those bits.
 */
static uint32_t gamma_value(bits *stream) {
    int digits = 0;
    while (!next_bit(stream)) {
        if (stream->failed || ++digits > 31) {
            stream->failed = 1;
            return 0;
        }
    }
    uint32_t value = 1u << digits;
    while (digits-- != 0) {
        if (next_bit(stream)) value |= 1u << digits;
    }
    return stream->failed ? 0 : value;
}

static int check(size_t position, size_t size, const char *what) {
    if (position > size) {
        cs2_set_error("truncated %s", what);
        return -1;
    }
    return 0;
}

static int is_numbered_image(const char *signature) {
    size_t length = strlen(signature);
    if (length < 7 || strncmp(signature, "img", 3) != 0) return 0;
    for (size_t i = 3; i < length; i++) {
        if (signature[i] < '0' || signature[i] > '9') return 0;
    }
    return 1;
}

static void signature_at(const uint8_t *file, size_t at, char out[9]) {
    size_t length = 0;
    while (length < 8 && file[at + length] != '\0') {
        out[length] = (char) file[at + length];
        length++;
    }
    out[length] = '\0';
}

static uint8_t *inflate_stream(const uint8_t *file, size_t size, size_t at,
                               uint32_t packed, uint32_t plain) {
    if (packed == 0 || plain == 0 || plain > MAX_PIXELS * 4) {
        cs2_set_error("an HG-3 stream declares an impossible size");
        return NULL;
    }
    if (check(at + packed, size, "HG-3 stream") != 0) return NULL;
    uint8_t *out = malloc(plain);
    if (out == NULL) {
        cs2_set_error("out of memory decoding an image");
        return NULL;
    }
    if (cs2_inflate(file + at, packed, out, plain) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

/*
 * Undoes the run-length pass. The command stream's first bit says whether the
 * first run is copied from the data stream or is zero, and runs alternate.
 */
static uint8_t *unrle(const uint8_t *data, size_t data_size,
                      const uint8_t *commands, size_t commands_size, size_t *out_size) {
    bits stream = { commands, commands_size, 0, 0, 0 };
    int copy = next_bit(&stream);
    uint32_t length = gamma_value(&stream);
    if (stream.failed || length == 0 || length > MAX_PIXELS * 4) {
        cs2_set_error("an HG-3 image declares an impossible unpacked size");
        return NULL;
    }
    uint8_t *out = calloc(length, 1);
    if (out == NULL) {
        cs2_set_error("out of memory decoding an image");
        return NULL;
    }
    size_t position = 0;
    size_t source = 0;
    while (position < length) {
        uint32_t run = gamma_value(&stream);
        if (stream.failed || position + run > length) {
            free(out);
            cs2_set_error("an HG-3 run runs past the end of the image");
            return NULL;
        }
        if (copy) {
            if (source + run > data_size) {
                free(out);
                cs2_set_error("an HG-3 run runs past the end of the data");
                return NULL;
            }
            memcpy(out + position, data + source, run);
            source += run;
        }
        position += run;
        copy = !copy;
    }
    *out_size = length;
    return out;
}

static uint8_t unpack_byte(uint32_t value) {
    uint32_t single = value & 0xff;
    return (uint8_t) ((single & 1) ? ((single >> 1) ^ 0xff) : (single >> 1));
}

/*
 * Undoes the transposition and the delta filter. Every pixel's four bytes live
 * in four separate quarters of the buffer, and each byte's four bit pairs are
 * spread one per quarter, so a byte is rebuilt from four lookups; the low bit of
 * the result then says whether the other seven are inverted. What that leaves is
 * a difference image: every byte is a delta from the pixel to its left, and
 * every row a delta from the row above.
 */
static uint8_t *undelta(uint8_t *packed, size_t packed_size, int height, int depth_bytes, int stride) {
    if (packed_size < (size_t) stride * (size_t) height) {
        cs2_set_error("an HG-3 image is shorter than the size it declares");
        return NULL;
    }
    uint32_t table[256];
    for (int i = 0; i < 256; i++) {
        uint32_t value = (uint32_t) (i & 0xC0);
        value = (value << 6) | (uint32_t) (i & 0x30);
        value = (value << 6) | (uint32_t) (i & 0x0C);
        value = (value << 6) | (uint32_t) (i & 0x03);
        table[i] = value;
    }
    uint8_t *out = malloc(packed_size);
    if (out == NULL) {
        cs2_set_error("out of memory decoding an image");
        return NULL;
    }
    size_t section = packed_size / 4;
    size_t a = 0, b = section, c = section * 2, d = section * 3;
    for (size_t at = 0; at + 4 <= packed_size && d < packed_size; at += 4) {
        uint32_t value = (table[packed[a++]] << 6) | (table[packed[b++]] << 4)
                       | (table[packed[c++]] << 2) | table[packed[d++]];
        out[at] = unpack_byte(value);
        out[at + 1] = unpack_byte(value >> 8);
        out[at + 2] = unpack_byte(value >> 16);
        out[at + 3] = unpack_byte(value >> 24);
    }
    for (int x = depth_bytes; x < stride; x++) out[x] = (uint8_t) (out[x] + out[x - depth_bytes]);
    for (int y = 1; y < height; y++) {
        size_t line = (size_t) y * stride;
        for (int x = 0; x < stride; x++) {
            out[line + x] = (uint8_t) (out[line + x] + out[line + x - stride]);
        }
    }
    return out;
}

/* Rows are stored bottom-up, and each pixel as blue, green, red, alpha. */
static uint32_t *to_argb(const uint8_t *pixels, int width, int height, int depth_bytes, int stride) {
    uint32_t *argb = malloc((size_t) width * height * sizeof *argb);
    if (argb == NULL) {
        cs2_set_error("out of memory decoding an image");
        return NULL;
    }
    for (int y = 0; y < height; y++) {
        size_t row = (size_t) (height - 1 - y) * stride;
        size_t out = (size_t) y * width;
        for (int x = 0; x < width; x++) {
            size_t at = row + (size_t) x * depth_bytes;
            uint32_t blue = pixels[at];
            uint32_t green = pixels[at + 1];
            uint32_t red = pixels[at + 2];
            uint32_t alpha = depth_bytes == 4 ? pixels[at + 3] : 0xff;
            argb[out + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
    }
    return argb;
}

static int first_frame(const uint8_t *file, size_t size, size_t *at) {
    if (size < 12 || memcmp(file, "HG-3", 4) != 0) {
        cs2_set_error("not an HG-3 image");
        return -1;
    }
    *at = cs2_u32(file, size, 4);
    return check(*at, size, "HG-3 header");
}

int cs2_hg3_frame_count(const uint8_t *file, size_t size) {
    size_t frame;
    if (first_frame(file, size, &frame) != 0) return -1;
    for (int count = 1; ; count++) {
        if (check(frame + 8, size, "HG-3 frame") != 0) return -1;
        uint32_t next = cs2_u32(file, size, frame);
        if (next == 0) return count;
        frame += next;
        if (check(frame, size, "HG-3 frame chain") != 0) return -1;
    }
}

static int decode_frame(const uint8_t *file, size_t size, size_t first_tag, cs2_hg3_frame *out) {
    int32_t std[10];
    int have_std = 0;
    size_t image_tag = 0;
    int have_image = 0;
    char kind[9] = "";

    for (size_t tag = first_tag; ; ) {
        if (check(tag + 16, size, "HG-3 tag") != 0) return -1;
        char signature[9];
        signature_at(file, tag, signature);
        uint32_t next = cs2_u32(file, size, tag + 8);
        size_t data = tag + 16;
        if (strncmp(signature, "stdinfo", 7) == 0) {
            if (check(data + 40, size, "HG-3 stdinfo") != 0) return -1;
            for (int i = 0; i < 10; i++) std[i] = (int32_t) cs2_u32(file, size, data + (size_t) i * 4);
            have_std = 1;
        } else if (!have_image && strncmp(signature, "img", 3) == 0
                   && strncmp(signature, "imgmode", 7) != 0) {
            image_tag = data;
            have_image = 1;
            memcpy(kind, signature, sizeof kind);
        }
        if (next == 0) break;
        tag += next;
        if (check(tag, size, "HG-3 tag chain") != 0) return -1;
    }
    if (!have_std) {
        cs2_set_error("an HG-3 frame has no stdinfo tag");
        return -1;
    }
    if (!have_image) {
        cs2_set_error("an HG-3 frame has no image tag");
        return -1;
    }
    if (!is_numbered_image(kind)) {
        cs2_set_error("HG-3 image tag \"%s\" is not supported yet", kind);
        return -1;
    }

    int width = std[0];
    int height = std[1];
    int depth_bits = std[2];
    if (width <= 0 || height <= 0 || (uint64_t) width * height > MAX_PIXELS) {
        cs2_set_error("an HG-3 frame claims to be %dx%d", width, height);
        return -1;
    }
    if (depth_bits != 24 && depth_bits != 32) {
        cs2_set_error("HG-3 depth %d is not supported", depth_bits);
        return -1;
    }
    int depth_bytes = (depth_bits + 7) / 8;
    int stride = (width * depth_bytes + 3) & ~3;

    /* The image tag: an unused word, the height again, then both streams'
       packed and unpacked lengths, and then the packed bytes themselves. */
    if (check(image_tag + 24, size, "HG-3 image header") != 0) return -1;
    uint32_t packed_data = cs2_u32(file, size, image_tag + 8);
    uint32_t plain_data = cs2_u32(file, size, image_tag + 12);
    uint32_t packed_commands = cs2_u32(file, size, image_tag + 16);
    uint32_t plain_commands = cs2_u32(file, size, image_tag + 20);
    size_t at = image_tag + 24;

    uint8_t *data = inflate_stream(file, size, at, packed_data, plain_data);
    if (data == NULL) return -1;
    uint8_t *commands = inflate_stream(file, size, at + packed_data, packed_commands, plain_commands);
    if (commands == NULL) {
        free(data);
        return -1;
    }
    size_t unpacked_size = 0;
    uint8_t *unpacked = unrle(data, plain_data, commands, plain_commands, &unpacked_size);
    free(data);
    free(commands);
    if (unpacked == NULL) return -1;
    uint8_t *plain = undelta(unpacked, unpacked_size, height, depth_bytes, stride);
    free(unpacked);
    if (plain == NULL) return -1;
    uint32_t *pixels = to_argb(plain, width, height, depth_bytes, stride);
    free(plain);
    if (pixels == NULL) return -1;

    out->width = width;
    out->height = height;
    out->offset_x = std[3];
    out->offset_y = std[4];
    out->total_width = std[5];
    out->total_height = std[6];
    out->base_x = std[8];
    out->base_y = std[9];
    out->pixels = pixels;
    return 0;
}

int cs2_hg3_decode(const uint8_t *file, size_t size, int frame_index, cs2_hg3_frame *out) {
    memset(out, 0, sizeof *out);
    size_t frame;
    if (first_frame(file, size, &frame) != 0) return -1;
    for (int index = 0; ; index++) {
        if (check(frame + 8, size, "HG-3 frame") != 0) return -1;
        uint32_t next = cs2_u32(file, size, frame);
        if (index == frame_index) return decode_frame(file, size, frame + 8, out);
        if (next == 0) {
            cs2_set_error("this HG-3 image has no frame %d", frame_index);
            return -1;
        }
        frame += next;
        if (check(frame, size, "HG-3 frame chain") != 0) return -1;
    }
}

void cs2_hg3_frame_free(cs2_hg3_frame *frame) {
    if (frame == NULL) return;
    free(frame->pixels);
    frame->pixels = NULL;
}
