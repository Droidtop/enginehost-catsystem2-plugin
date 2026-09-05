/*
 * Writing a PNG, which is how the engine is checked without a screen: the
 * desktop runner can draw a scene and save it instead of opening a window, so
 * what the engine puts on screen can be looked at in this container and
 * compared between builds.
 */
#ifndef CS2_PNG_H
#define CS2_PNG_H

#include "cs2.h"

/* Writes a width * height canvas of 0xAARRGGBB pixels. Returns 0 or -1. */
int cs2_png_write(const char *path, const uint32_t *pixels, int width, int height);

#endif
