#include "cs2.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static char error_message[512] = "";
static int quiet;

void cs2_bytes_free(cs2_bytes *bytes) {
    if (bytes == NULL) return;
    free(bytes->data);
    bytes->data = NULL;
    bytes->size = 0;
}

const char *cs2_error(void) {
    return error_message[0] == '\0' ? "no error" : error_message;
}

void cs2_set_error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error_message, sizeof error_message, format, arguments);
    va_end(arguments);
}

void cs2_log_quiet(int value) {
    quiet = value;
}

void cs2_log(const char *format, ...) {
    if (quiet) return;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

uint32_t cs2_u32(const uint8_t *data, size_t size, size_t at) {
    if (data == NULL || at + 4 > size) return 0;
    return (uint32_t) data[at] | ((uint32_t) data[at + 1] << 8)
         | ((uint32_t) data[at + 2] << 16) | ((uint32_t) data[at + 3] << 24);
}

uint16_t cs2_u16(const uint8_t *data, size_t size, size_t at) {
    if (data == NULL || at + 2 > size) return 0;
    return (uint16_t) ((uint16_t) data[at] | ((uint16_t) data[at + 1] << 8));
}

int cs2_ieq(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        unsigned char a = (unsigned char) *left;
        unsigned char b = (unsigned char) *right;
        if (a >= 'A' && a <= 'Z') a = (unsigned char) (a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char) (b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

int cs2_inflate(const uint8_t *packed, size_t packed_size, uint8_t *plain, size_t plain_size) {
    z_stream stream;
    memset(&stream, 0, sizeof stream);
    if (inflateInit(&stream) != Z_OK) {
        cs2_set_error("could not start decompression");
        return -1;
    }
    stream.next_in = (Bytef *) packed;
    stream.avail_in = (uInt) packed_size;
    stream.next_out = plain;
    stream.avail_out = (uInt) plain_size;
    int result = inflate(&stream, Z_FINISH);
    size_t produced = stream.total_out;
    inflateEnd(&stream);
    if ((result != Z_STREAM_END && result != Z_OK) || produced != plain_size) {
        cs2_set_error("compressed stream does not hold the %zu bytes it declares", plain_size);
        return -1;
    }
    return 0;
}

int cs2_read_file(const char *path, size_t limit, cs2_bytes *out) {
    out->data = NULL;
    out->size = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        cs2_set_error("cannot open %s", path);
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        cs2_set_error("cannot measure %s", path);
        return -1;
    }
    long length = ftell(file);
    rewind(file);
    if (length < 0 || (size_t) length > limit) {
        fclose(file);
        cs2_set_error("%s is larger than %zu bytes", path, limit);
        return -1;
    }
    uint8_t *data = length == 0 ? NULL : malloc((size_t) length);
    if (length > 0 && data == NULL) {
        fclose(file);
        cs2_set_error("out of memory reading %s", path);
        return -1;
    }
    if (length > 0 && fread(data, 1, (size_t) length, file) != (size_t) length) {
        free(data);
        fclose(file);
        cs2_set_error("%s ended early", path);
        return -1;
    }
    fclose(file);
    out->data = data;
    out->size = (size_t) length;
    return 0;
}
