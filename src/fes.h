/*
 * FES, the game's own UI layout - the file the whole front end is written in.
 *
 * fes.int holds thirty-five of these and they are not a data format with a
 * program somewhere else: a .fes declares the objects a screen is made of and
 * then carries the screen's own little script, in named sections, over the
 * game's numbered flags. The title screen, the logo, save and load, the config
 * pages and the message window are each one of these files.
 *
 * The container is a header and one zlib stream, the same shape as .kcs and
 * .cst, and what comes out is plain CP932 text with CRLF line endings:
 *
 *     0x00  char[4]  "FES\0"
 *     0x04  u32      the packed size
 *     0x08  u32      the unpacked size
 *     0x0C  u32      0
 *     0x10  ...      one zlib stream
 *
 * The text is sections introduced by a #WORD line. Three of them are tables and
 * are read here: #DEFINE names the objects by kind and count, #OBJECT is a
 * column-headed table over them ("#OBJECT FILE ID.0 PL DISP PRI" and then one
 * line per object), and #SUBCOMMAND names the motions. Every other section is a
 * run of script lines, kept as they lie for layout.c to run.
 *
 * Format read from the game's own files; see /root/re/fes/FES-FORMAT.md.
 */
#ifndef CS2_FES_H
#define CS2_FES_H

#include "files.h"

#define CS2_FES_IDS 6

typedef enum {
    CS2_FES_OTHER = 0,
    CS2_FES_PLANE,
    CS2_FES_IMAGE,
    CS2_FES_BUTTON,
    CS2_FES_SOUND
} cs2_fes_kind;

/*
 * One object as the file declares it. An array declared "BUTTON btn_d[6]" is
 * six of these, sharing a name and numbered from zero; a scalar has index -1.
 * A table row addressed by the bare name ("pl 0,0 0,0 1280,720") writes every
 * element of the array, which is how the file gives them their defaults.
 */
typedef struct {
    char name[40];
    int index;                   /* -1 when the object is not an array */
    cs2_fes_kind kind;
    char file[64];               /* FILE: the image or sound it draws from */
    char plane[40];              /* PL: the plane it is drawn on */
    int ids[CS2_FES_IDS];        /* ID.0..ID.5, -1 where the table has none */
    int x, y;                    /* POS, inside its plane */
    int base_x, base_y;          /* BASE */
    int width, height;           /* SIZE */
    int priority;
    int disp;
    int enable;
} cs2_fes_object;

typedef struct cs2_fes cs2_fes;

/* Reads "fes.int/<name>.fes". NULL with an error set when there is no such layout. */
cs2_fes *cs2_fes_load(cs2_files *files, const char *name);
void cs2_fes_free(cs2_fes *fes);

const char *cs2_fes_name(const cs2_fes *fes);

/* A script section by name, without its '#'. -1 when the layout has none. */
int cs2_fes_section(const cs2_fes *fes, const char *name);
size_t cs2_fes_section_lines(const cs2_fes *fes, int section);
const char *cs2_fes_line(const cs2_fes *fes, int section, size_t line);
const char *cs2_fes_section_name(const cs2_fes *fes, int section);

size_t cs2_fes_object_count(const cs2_fes *fes);
const cs2_fes_object *cs2_fes_object_at(const cs2_fes *fes, size_t index);

#endif
