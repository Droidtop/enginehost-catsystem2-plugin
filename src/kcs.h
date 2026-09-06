/*
 * KCS, the game's own system script, and the machine that runs it.
 *
 * A CatSystem2 game boots into a compiled system script - kcs.int/main.kcs for
 * Labyrinth of Grisaia - and that script is the game's front: the title screen,
 * the menus, the new-game path, and its save and load. The scene scripts the
 * engine already plays are what a KCS script hands over to.
 *
 * The format and the instruction set were read out of the game's own engine and
 * are written up in /root/re/kcs/KCS-FORMAT.md. The machine calls itself
 * ScriptDecode2: a stack of 32-bit slots, 63 opcodes, and one instruction is an
 * opcode byte followed by a fixed number of operands, each a flags byte and a
 * dword.
 *
 * Everything the game actually does - draw, wait, play a voice, save - is one
 * opcode, GCALL, into a table of a thousand engine functions addressed by
 * number. This file runs the machine; what the numbers mean is the engine's
 * business, so a GCALL is handed out through cs2_kcs_gcall.
 */
#ifndef CS2_KCS_H
#define CS2_KCS_H

#include <stddef.h>
#include <stdint.h>

#include "files.h"

typedef struct cs2_kcs cs2_kcs;

/*
 * What a GCALL says when it returns, in the game engine's own numbering. The
 * two that carry the machine are DONE_VALUE, an ordinary function that answers
 * something, and AGAIN, which is how every wait in the game is written: the
 * program counter is rewound to the call so the same call is made again on the
 * next frame, until it stops asking.
 */
enum {
    CS2_KCS_UNWRITTEN = -1,       /* not one of ours: answer zero and say so once */
    CS2_KCS_DONE_YIELD = 0,       /* finished, no value, stop for this frame */
    CS2_KCS_DONE_VALUE = 1,       /* finished, answers a value, keep running */
    CS2_KCS_DONE = 2,             /* finished, no value, keep running */
    CS2_KCS_DONE_VALUE_YIELD = 3, /* finished, answers a value, stop for this frame */
    CS2_KCS_DONE_YIELD_AGAIN = 4, /* finished, no value, stop for this frame */
    CS2_KCS_AGAIN = 5             /* not finished: make this same call again next frame */
};

/*
 * Not written yet, but the game's own return code for it has been read off its
 * handler. Answering zero is a guess; leaving the stack as the script expects
 * is not, and a function that answers a value where the original answered none
 * puts a word on the stack the next statement will take for its own. So a run
 * gets much further when the code is given even where the work is not done.
 */
#define CS2_KCS_UNWRITTEN_WITH(code) (-2 - (int) (code))

/*
 * One engine function, called with the argument block the script pushed (its
 * layout is that function's own business) and a slot to answer in.
 */
typedef int (*cs2_kcs_gcall)(void *context, cs2_kcs *script, uint32_t id,
                             const uint8_t *arguments, uint32_t argument_size,
                             uint32_t *answer);

/* Loads "kcs.int/main.kcs" and lays out its memory. NULL and an error if not. */
cs2_kcs *cs2_kcs_load(cs2_files *files, const char *path);
void cs2_kcs_free(cs2_kcs *script);

/* Where GCALL goes. Without one every call simply answers zero. */
void cs2_kcs_set_gcall(cs2_kcs *script, cs2_kcs_gcall gcall, void *context);

/*
 * The game's persistent variables, which the scripts reach through addresses
 * with their top bit set: what a save file is made of, and what one screen of
 * the game leaves for the next. A script left to itself keeps its own; whatever
 * runs several of them lends the same block to each.
 */
void cs2_kcs_set_variables(cs2_kcs *script, void *variables, uint32_t size);
void *cs2_kcs_variables(cs2_kcs *script, uint32_t *size);

/* Logs every GCALL id the first time the script reaches it. */
void cs2_kcs_trace(cs2_kcs *script, int on);

/*
 * How a script waits. A function that cannot answer now says so with
 * cs2_kcs_suspend and CS2_KCS_DONE_YIELD_AGAIN; the frame ends there, and the
 * next frame begins by handing the script the answer - zero, unless something
 * has happened in between and the engine has set one with cs2_kcs_answer. That
 * is the whole of the game's own waiting: the address is where the engine puts
 * what happened, and the answer is whether anything did.
 */
void cs2_kcs_suspend(cs2_kcs *script, uint32_t answer_address);
void cs2_kcs_answer(cs2_kcs *script, uint32_t value);
int cs2_kcs_suspended(const cs2_kcs *script);
uint32_t cs2_kcs_suspend_address(const cs2_kcs *script);

/*
 * Runs until the script yields, giving up after a budget of instructions so a
 * script that spins on an unimplemented wait says so instead of hanging.
 * Returns 1 when there is more to run, 0 when the script has ended, -1 on a
 * fault (cs2_error says what).
 */
int cs2_kcs_frame(cs2_kcs *script, uint32_t budget);

/* What the script has run so far, and where it is; for logs and for save. */
uint64_t cs2_kcs_instructions(const cs2_kcs *script);
uint32_t cs2_kcs_pc(const cs2_kcs *script);

/*
 * Where the call being run keeps its arguments and its locals. The game's own
 * string formatting names a variable by bank and offset, and its "L" bank is
 * counted from here, exactly as the frameaddr opcode counts.
 */
uint32_t cs2_kcs_base(const cs2_kcs *script);
const char *cs2_kcs_name(const cs2_kcs *script);

/*
 * The script's memory, as GCALL handlers see it: a string argument is an
 * address into it, and so is anything a handler is asked to write back.
 */
const char *cs2_kcs_string(cs2_kcs *script, uint32_t address);

/*
 * The same string with the game's own formatting expanded: a printf conversion
 * followed by the variable that fills it, as "plane %d[L12]" or
 * "config.int/%s[L280]". Every name, path and message an engine function is
 * given comes through here, because that is where the game expands it; only
 * the plain copy reads a string as it lies. The answer holds until a few more
 * strings have been asked for.
 */
const char *cs2_kcs_text(cs2_kcs *script, uint32_t address);
void *cs2_kcs_at(cs2_kcs *script, uint32_t address, uint32_t size);

/* Reading an argument block: the arguments are dwords, first at offset 0. */
uint32_t cs2_kcs_argument(const uint8_t *arguments, uint32_t argument_size, uint32_t index);

#endif
