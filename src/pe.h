/*
 * Just enough of the Windows PE format to read string-named resources out of a
 * CatSystem2 executable. The archive key lives there, so the engine has to look
 * inside the game's own binary; it never runs it.
 */
#ifndef CS2_PE_H
#define CS2_PE_H

#include <stddef.h>
#include <stdint.h>

typedef struct cs2_pe cs2_pe;

/*
 * Reads the string-named resources of a PE image. The image must stay alive and
 * unchanged for as long as the result is used, because resources point into it.
 * Returns NULL when the file is not a PE or carries no such resources.
 */
cs2_pe *cs2_pe_open(const uint8_t *image, size_t size);

/*
 * One resource, addressed by the two named levels of the resource tree in
 * either order: CatSystem2 executables store the pair the opposite way round
 * from most Windows binaries, and it is the pair that identifies it either way.
 */
const uint8_t *cs2_pe_resource(const cs2_pe *pe, const char *type, const char *name, size_t *size);

void cs2_pe_free(cs2_pe *pe);

#endif
