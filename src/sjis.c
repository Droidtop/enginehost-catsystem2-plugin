#include "sjis.h"

#include <stdlib.h>

#include "cp932_table.h"

/* The row a lead byte occupies in the table, or -1 when it leads nothing. */
static int lead_row(unsigned byte) {
    if (byte >= 0x81 && byte <= 0x9f) return (int) (byte - 0x81);
    if (byte >= 0xe0 && byte <= 0xfc) return (int) (byte - 0xe0 + (0x9f - 0x81 + 1));
    return -1;
}

static size_t append_utf8(char *out, size_t at, unsigned code) {
    if (code < 0x80) {
        out[at++] = (char) code;
    } else if (code < 0x800) {
        out[at++] = (char) (0xc0 | (code >> 6));
        out[at++] = (char) (0x80 | (code & 0x3f));
    } else {
        out[at++] = (char) (0xe0 | (code >> 12));
        out[at++] = (char) (0x80 | ((code >> 6) & 0x3f));
        out[at++] = (char) (0x80 | (code & 0x3f));
    }
    return at;
}

char *cs2_sjis_to_utf8(const uint8_t *data, size_t size) {
    /* Three UTF-8 bytes is the most any CP932 character becomes. */
    char *out = malloc(size * 3 + 1);
    if (out == NULL) return NULL;
    size_t at = 0;
    for (size_t i = 0; i < size; ) {
        unsigned byte = data[i];
        if (byte < 0x80) {
            out[at++] = (char) byte;
            i++;
            continue;
        }
        if (byte >= 0xa1 && byte <= 0xdf) {
            /* Halfwidth katakana sit in one byte, straight after U+FF61. */
            at = append_utf8(out, at, 0xff61 + (byte - 0xa1));
            i++;
            continue;
        }
        int row = lead_row(byte);
        unsigned trail = i + 1 < size ? data[i + 1] : 0;
        unsigned code = 0;
        if (row >= 0 && trail >= CP932_TRAIL_FIRST && trail <= CP932_TRAIL_LAST) {
            code = cp932_table[row][trail - CP932_TRAIL_FIRST];
        }
        if (code == 0) {
            at = append_utf8(out, at, 0xfffd);
            i++;
            continue;
        }
        at = append_utf8(out, at, code);
        i += 2;
    }
    out[at] = '\0';
    return out;
}
