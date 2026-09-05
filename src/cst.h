/*
 * CatSystem2 CatScene scripts.
 *
 * A .cst file is an eight-byte signature, the packed and unpacked sizes, and
 * then one zlib stream. Inside is a table of offsets and, after it, the lines
 * themselves: a marker byte, a type, and the text in CP932. The types are few:
 * a line of the message, the speaker's name, a pause for the reader, the end of
 * a page, and a command.
 */
#ifndef CS2_CST_H
#define CS2_CST_H

#include "cs2.h"

#define CS2_CST_INPUT   0x02
#define CS2_CST_PAGE    0x03
#define CS2_CST_MESSAGE 0x20
#define CS2_CST_NAME    0x21
#define CS2_CST_COMMAND 0x30

typedef struct {
    unsigned type;
    const char *text;   /* UTF-8, owned by the script */
} cs2_cst_line;

typedef struct cs2_cst cs2_cst;

/* Parses one script. The name is only used in error messages. */
cs2_cst *cs2_cst_parse(const uint8_t *data, size_t size, const char *name);

size_t cs2_cst_count(const cs2_cst *script);
cs2_cst_line cs2_cst_at(const cs2_cst *script, size_t index);

void cs2_cst_free(cs2_cst *script);

#endif
