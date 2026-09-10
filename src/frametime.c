#include "frametime.h"

#include <time.h>

#include "cs2.h"

/*
 * One set of totals, gathered over a second and then emptied. The engine runs
 * its game on one thread - the host's frame callback - so this needs no lock,
 * and a span measured on the sound thread is not measured at all rather than
 * measured wrongly: the spans here are all on the frame.
 */
static struct {
    int on;
    uint64_t spent[CS2_SPAN_COUNT];
    unsigned long times[CS2_SPAN_COUNT];
    unsigned long frames;
    uint64_t since;
} it;

uint64_t cs2_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000000000u + (uint64_t) now.tv_nsec;
}

void cs2_frametime(int wanted) {
    it.on = wanted;
    it.since = cs2_now();
}

void cs2_span_add(cs2_span which, uint64_t from) {
    if (!it.on || which < 0 || which >= CS2_SPAN_COUNT) return;
    it.spent[which] += cs2_now() - from;
    it.times[which]++;
}

void cs2_span_count(cs2_span which, int how_many) {
    if (!it.on || which < 0 || which >= CS2_SPAN_COUNT) return;
    it.times[which] += (unsigned long) how_many;
}

/* A span as milliseconds in one frame of the stretch being reported. */
static double each(cs2_span which) {
    if (it.frames == 0) return 0;
    return (double) it.spent[which] / (double) it.frames / 1000000.0;
}

static double per_frame(cs2_span which) {
    if (it.frames == 0) return 0;
    return (double) it.times[which] / (double) it.frames;
}

void cs2_frame_done(void) {
    if (!it.on) return;
    it.frames++;
    uint64_t now = cs2_now();
    if (now - it.since < 1000000000u) return;
    double seconds = (double) (now - it.since) / 1000000000.0;
    cs2_log("frame time: %.1f ms each over %lu frames (%.1f a second); script %.1f,"
            " front end %.1f, composing %.1f of which pictures %.1f, archives %.1f"
            " and text %.1f, to the screen %.1f; %.1f pictures decoded and %.1f"
            " archive reads a frame",
            seconds * 1000.0 / (double) it.frames, it.frames,
            (double) it.frames / seconds, each(CS2_SPAN_SCRIPT), each(CS2_SPAN_LAYOUT),
            each(CS2_SPAN_COMPOSE), each(CS2_SPAN_DECODE), each(CS2_SPAN_READ),
            each(CS2_SPAN_TEXT), each(CS2_SPAN_UPLOAD), per_frame(CS2_SPAN_DECODE),
            per_frame(CS2_SPAN_READ));
    for (int i = 0; i < CS2_SPAN_COUNT; i++) {
        it.spent[i] = 0;
        it.times[i] = 0;
    }
    it.frames = 0;
    it.since = now;
}
