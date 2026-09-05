/*
 * Shared vocabulary for the CatSystem2 engine: a growable byte buffer, the
 * little-endian reads every one of the game's formats is written in, and the
 * one place errors are reported from.
 *
 * Everything here reads files that came off a disc, so nothing is trusted for
 * its own size: every read is bounded and every allocation has a ceiling.
 */
#ifndef CS2_H
#define CS2_H

#include <stddef.h>
#include <stdint.h>

/* A block of bytes the engine owns. data is NULL when there are none. */
typedef struct {
    uint8_t *data;
    size_t size;
} cs2_bytes;

void cs2_bytes_free(cs2_bytes *bytes);

/*
 * The last thing that went wrong, as a sentence. Engine functions return 0 on
 * success and -1 on failure, having set this; the caller prints it.
 */
const char *cs2_error(void);
void cs2_set_error(const char *format, ...);

/* Diagnostics the frontend prints: what was played, what was skipped. */
void cs2_log(const char *format, ...);
void cs2_log_quiet(int quiet);

/* Little-endian reads out of a bounded buffer. 0 when the read is out of range. */
uint32_t cs2_u32(const uint8_t *data, size_t size, size_t at);
uint16_t cs2_u16(const uint8_t *data, size_t size, size_t at);

/* Case-insensitive compare for ASCII, which is what archive names need. */
int cs2_ieq(const char *left, const char *right);

/* Inflates a zlib stream into exactly plain bytes. Returns 0 or -1. */
int cs2_inflate(const uint8_t *packed, size_t packed_size, uint8_t *plain, size_t plain_size);

/* Reads a whole file, up to limit bytes. */
int cs2_read_file(const char *path, size_t limit, cs2_bytes *out);

#endif
