/*
 * The planes the game's own system script builds its screen out of.
 *
 * A KCS script does not draw: it makes named planes, places them, gives them a
 * colour or a layout, and the engine draws the tree. In the game's own engine a
 * plane is an object reached by a numbered message; here it is a record in a
 * flat table addressed by a handle, because what the script does with a plane -
 * where it is, how big, how bright, in front of what - is all this file needs
 * to know, and copying that object graph would buy nothing.
 *
 * Handles are small integers and 0 means "no plane". Planes are drawn in
 * priority order, lowest first, so the game's own numbers (a background button
 * at 10000, the top plane at 61440) come out in the order it intends.
 */
#ifndef CS2_PLANE_H
#define CS2_PLANE_H

#include <stddef.h>
#include <stdint.h>

typedef struct cs2_planes cs2_planes;
typedef uint32_t cs2_plane;

typedef struct {
    cs2_plane handle;
    cs2_plane parent;
    int type;                /* the game's own plane kind, kept as it is given */
    char name[64];
    float x, y;              /* where it sits inside its parent */
    float width, height;
    float priority;          /* drawn lowest first */
    int visible;
    int filled;              /* whether a colour has been given */
    uint32_t colour;         /* ARGB, as the script writes it */
    char layout[64];         /* the .fes layout it was given, if any */
    cs2_plane attached;      /* the plane it was handed to work with */
    int running;             /* started, and not yet finished */
    int result;              /* what it finished with; -1 while it runs */
} cs2_plane_state;

cs2_planes *cs2_planes_new(int width, int height);
void cs2_planes_free(cs2_planes *planes);

/* The screen itself: every plane the script makes hangs under this one. */
cs2_plane cs2_planes_root(const cs2_planes *planes);

cs2_plane cs2_plane_create(cs2_planes *planes, int type, cs2_plane parent, const char *name);
void cs2_plane_destroy(cs2_planes *planes, cs2_plane handle);

/* NULL when the handle names no plane, which is how a script's mistake shows. */
cs2_plane_state *cs2_plane_get(cs2_planes *planes, cs2_plane handle);

/* Walking them in the order they were made, for logs and for saving. */
size_t cs2_planes_count(const cs2_planes *planes);
const cs2_plane_state *cs2_planes_at(const cs2_planes *planes, size_t index);

/*
 * Composes the tree onto an ARGB canvas: every visible plane that has been
 * given a colour is painted over what is under it, in priority order, at the
 * place its parents put it. A plane with a layout but no colour draws nothing
 * yet - that is the .fes work, and it is not here.
 */
void cs2_planes_draw(const cs2_planes *planes, uint32_t *canvas, int width, int height);

#endif
