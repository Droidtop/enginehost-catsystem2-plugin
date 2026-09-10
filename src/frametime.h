/*
 * Where the frame goes.
 *
 * The console said it first: Grisaia ran, and ran INCREDIBLY SLOWLY. A frame
 * that takes too long is not one thing being slow, it is one of six things
 * being slow, and on a handheld there is no profiler to ask - so the engine
 * says for itself, in the log the console already keeps.
 *
 * Every frame is divided into spans that do not overlap on purpose: the script
 * step, the front end's layouts, composing the picture and handing it to
 * whoever shows it. Two more are counted inside composing, because that is
 * where the time was - decoding pictures and drawing text - and a line that
 * says "composing 30 ms" without saying how much of it was decoding answers
 * nothing. Counts go with them: a picture decoded twice in one frame is the
 * fault itself, not a symptom of one.
 *
 * One line a second, so a run of any length can be read, and cheap enough to
 * leave on: two clock reads per span and nothing allocated.
 */
#ifndef CS2_FRAMETIME_H
#define CS2_FRAMETIME_H

#include <stdint.h>

typedef enum {
    CS2_SPAN_SCRIPT,      /* the game's own scripts, stepped */
    CS2_SPAN_LAYOUT,      /* the .fes front end, a frame of each screen */
    CS2_SPAN_COMPOSE,     /* the picture built out of planes and layouts */
    CS2_SPAN_DECODE,      /* inside composing: HG-3 pictures turned into pixels */
    CS2_SPAN_READ,        /* inside composing: the archive entries read for them */
    CS2_SPAN_TEXT,        /* inside composing: glyphs drawn */
    CS2_SPAN_UPLOAD,      /* the picture handed to the screen */
    CS2_SPAN_COUNT
} cs2_span;

/* The monotonic clock, in nanoseconds. */
uint64_t cs2_now(void);

/* Adds to a span, and counts one of whatever the span is of. */
void cs2_span_add(cs2_span which, uint64_t from);
void cs2_span_count(cs2_span which, int how_many);

/*
 * The end of one frame. Answers how many frames have gone by, and writes the
 * line through cs2_log once a second. Whoever drives the frame loop calls it.
 */
void cs2_frame_done(void);

/* Nothing is reported until this is on: a tool that draws one frame says so. */
void cs2_frametime(int wanted);

#endif
