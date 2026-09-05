/*
 * CatSystem2's HG-3 image format.
 *
 * A file is a chain of frames; a frame is a chain of tags. "stdinfo" carries the
 * size and where the frame sits on the screen, "img####" the pixels. The pixels
 * are packed four ways over, undone here in reverse: two zlib streams (the data
 * and a bit-level command stream), a run-length pass in which the commands say
 * how many bytes to copy and how many are zero, a transposition that stores the
 * four bytes of every pixel in four separate quarters of the buffer with their
 * bit pairs interleaved, and a delta filter along the first row and then down
 * the rows. Standard frames are stored bottom-up.
 *
 * Format reference: TriggersTools.CatSystem2 (MIT), which documents the tags and
 * the packing; see THIRD_PARTY.md.
 */
#ifndef CS2_HG3_H
#define CS2_HG3_H

#include "cs2.h"

typedef struct {
    int width;
    int height;
    int offset_x;
    int offset_y;
    int total_width;
    int total_height;
    int base_x;
    int base_y;
    uint32_t *pixels;   /* width * height, 0xAARRGGBB, top row first */
} cs2_hg3_frame;

/* Decodes one frame, counting from zero. Returns 0, or -1 with an error set. */
int cs2_hg3_decode(const uint8_t *file, size_t size, int frame_index, cs2_hg3_frame *out);

/* How many frames the file holds, which is an animation's length. */
int cs2_hg3_frame_count(const uint8_t *file, size_t size);

void cs2_hg3_frame_free(cs2_hg3_frame *frame);

#endif
