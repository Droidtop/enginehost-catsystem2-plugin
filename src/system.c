/*
 * The engine functions the game's own system script calls.
 *
 * Each one is written from its handler in the game's engine, and the numbering
 * is that engine's table at 0x0076D948. Anything not written here answers zero
 * and says so once in the log, which is how the boot path of a game names the
 * functions it needs.
 */
#include "system.h"

#include <stdlib.h>
#include <string.h>

#include "cs2.h"

struct cs2_system {
    cs2_files *files;
    uint32_t event;      /* what the reader has done, nothing yet if zero */
};

cs2_system *cs2_system_new(cs2_files *files) {
    cs2_system *system = calloc(1, sizeof *system);
    if (system == NULL) {
        cs2_set_error("out of memory for the engine's system functions");
        return NULL;
    }
    system->files = files;
    return system;
}

void cs2_system_free(cs2_system *system) {
    free(system);
}

void cs2_system_event(cs2_system *system, uint32_t event) {
    system->event = event;
}

/*
 * 71: wait for the reader, and put what they did at the address given.
 *
 * This is the game's frame boundary. Its handler hands the address to the
 * script's own suspend fields and answers "no value, end the frame"; the next
 * frame begins by telling the script whether anything happened. The main menu
 * is a loop around this call: ask, and if nothing happened run one step of the
 * screen it is showing.
 */
static int wait_for_the_reader(cs2_system *system, cs2_kcs *script,
                               const uint8_t *arguments, uint32_t argument_size) {
    uint32_t where = cs2_kcs_argument(arguments, argument_size, 0);
    cs2_kcs_suspend(script, where);
    if (system->event != 0) {
        uint32_t *at = cs2_kcs_at(script, where, 4);
        if (at != NULL) {
            uint32_t event = system->event;
            memcpy(at, &event, 4);
            cs2_kcs_answer(script, 1);
        }
        system->event = 0;
    }
    return CS2_KCS_DONE_YIELD_AGAIN;
}

int cs2_system_gcall(void *context, cs2_kcs *script, uint32_t id,
                     const uint8_t *arguments, uint32_t argument_size, uint32_t *answer) {
    cs2_system *system = context;
    (void) answer;
    switch (id) {
    case 71:
        return wait_for_the_reader(system, script, arguments, argument_size);
    default:
        return CS2_KCS_UNWRITTEN;
    }
}
