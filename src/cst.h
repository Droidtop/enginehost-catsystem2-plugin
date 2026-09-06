/*
 * CatSystem2 CatScene scripts.
 *
 * A .cst file is an eight-byte signature, the packed and unpacked sizes, and
 * then one zlib stream. What comes out is in two levels, and the difference
 * matters: the script is a list of LINES, and a line is a run of WORDS.
 *
 *     0x00  u32  the unpacked size again
 *     0x04  u32  how many lines
 *     0x08  u32  where the word table is, from 0x10
 *     0x0C  u32  where the words themselves are, from 0x10
 *     0x10  ...  the lines: {u32 how many words, u32 the first of them}
 *     ...        the word table: one u32 offset each
 *     ...        the words: a marker byte 0x01, a type, the text in CP932
 *
 * A word's type is the few the format has: a piece of the message, the
 * speaker's name, a pause for the reader, the end of a page, and a command.
 *
 * The system script addresses a scenario as (line, word) and asks the engine
 * what kind the word is, so both levels have to be here; playing a script
 * through is a walk of the words in file order, which is the same words.
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
} cs2_cst_word;

typedef struct cs2_cst cs2_cst;

/* Parses one script. The name is only used in error messages. */
cs2_cst *cs2_cst_parse(const uint8_t *data, size_t size, const char *name);

/* The words, in the order the file holds them: playing a script walks these. */
size_t cs2_cst_word_count(const cs2_cst *script);
cs2_cst_word cs2_cst_word_at(const cs2_cst *script, size_t index);

/* The lines they are grouped into, and where each line's run of words begins. */
size_t cs2_cst_line_count(const cs2_cst *script);
int cs2_cst_line_words(const cs2_cst *script, size_t line, size_t *first, size_t *count);

void cs2_cst_free(cs2_cst *script);

#endif
