/*
 * ScriptDecode2, the machine a CatSystem2 game's own system script runs on.
 *
 * Written from the reading of the game's engine in /root/re/kcs/KCS-FORMAT.md:
 * the container and its string table, the 63 opcodes and their handler table at
 * 0x00868CB8, the operand fetcher at 0x6763E0, and GCALL's return codes. Where
 * this file looks odd - a byte-wide increment that pushes a whole dword, a
 * divide by zero that answers zero - it is odd in the same way the original is,
 * because the game's scripts were compiled against that behaviour.
 *
 * Nothing here trusts the file: every stack move, every address and every read
 * of the code is bounded, and a script that steps outside stops with an error
 * rather than reading someone else's memory.
 */
#include "kcs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs2.h"

#define KCS_HEADER      0x34u
#define KCS_MAX_IMAGE   (64u * 1024u * 1024u)
#define KCS_GCALL_COUNT 1024u

/*
 * The persistent variables, which addresses with their top bit set reach. In
 * the game these belong to whatever runs the scripts, not to a script, and its
 * size is set somewhere this reading has not followed yet. The highest offset
 * the eleven scripts of Labyrinth of Grisaia name outright is 0x5acc, so this
 * is that with room for anything they index into at run time; a script that
 * reaches past it says so rather than reading anything else.
 */
#define KCS_VARIABLES   (64u * 1024u)

struct cs2_kcs {
    char name[96];

    uint8_t *image;         /* the header, then the inflated content */
    uint32_t image_size;

    uint8_t *code;          /* image + the header's code offset */
    uint32_t code_size;
    uint32_t pc;

    uint8_t *memory;        /* the globals, then the stack growing upwards */
    uint32_t memory_size;
    uint32_t globals_size;
    uint32_t sp;            /* all three marks start at the end of the globals */
    uint32_t frame;
    uint32_t base;          /* the arguments of the call being run */

    uint8_t *saved;         /* the persistent region, addressed with the top bit */
    uint32_t saved_size;
    int saved_is_ours;

    uint32_t switch_value;

    int suspended;              /* a call that could not answer this frame */
    uint32_t suspend_address;
    uint32_t suspend_answer;

    cs2_kcs_gcall gcall;
    void *gcall_context;
    uint8_t seen[KCS_GCALL_COUNT];
    int trace;

    int fault;
    uint64_t instructions;
};

/* ---------------------------------------------------------------- loading */

static uint32_t header_u32(const uint8_t *data, size_t at) {
    return cs2_u32(data, KCS_HEADER, at);
}

cs2_kcs *cs2_kcs_load(cs2_files *files, const char *path) {
    cs2_bytes file = {NULL, 0};
    if (cs2_files_read(files, path, &file) != 0) return NULL;
    if (file.size < KCS_HEADER + 2 || memcmp(file.data, "KCS", 4) != 0) {
        cs2_set_error("%s is not a KCS script", path);
        cs2_bytes_free(&file);
        return NULL;
    }

    uint32_t code_at  = header_u32(file.data, 0x0C);
    uint32_t code_end = header_u32(file.data, 0x10) + KCS_HEADER;
    uint32_t strings_at = header_u32(file.data, 0x14);
    uint32_t strings    = header_u32(file.data, 0x18);
    uint32_t globals    = header_u32(file.data, 0x20);
    uint32_t stack      = header_u32(file.data, 0x24);
    uint32_t plain      = header_u32(file.data, 0x28);
    uint32_t image_size = header_u32(file.data, 0x30) + KCS_HEADER;

    if (plain < KCS_HEADER || plain > KCS_MAX_IMAGE || image_size < plain
        || image_size > KCS_MAX_IMAGE || code_at < KCS_HEADER || code_end > plain
        || code_end < code_at || globals > KCS_MAX_IMAGE || stack > KCS_MAX_IMAGE
        || globals + stack < globals) {
        cs2_set_error("%s has a KCS header that does not hang together", path);
        cs2_bytes_free(&file);
        return NULL;
    }

    cs2_kcs *script = calloc(1, sizeof *script);
    uint8_t *image = calloc(1, image_size);
    uint8_t *memory = calloc(1, globals + stack + 4u);
    uint8_t *variables = calloc(1, KCS_VARIABLES);
    if (script == NULL || image == NULL || memory == NULL || variables == NULL) {
        cs2_set_error("out of memory for %s", path);
        free(script);
        free(image);
        free(memory);
        free(variables);
        cs2_bytes_free(&file);
        return NULL;
    }

    memcpy(image, file.data, KCS_HEADER);
    if (cs2_inflate(file.data + KCS_HEADER, file.size - KCS_HEADER,
                    image + KCS_HEADER, plain - KCS_HEADER) != 0) {
        cs2_set_error("%s will not unpack: %s", path, cs2_error());
        free(script);
        free(image);
        free(memory);
        free(variables);
        cs2_bytes_free(&file);
        return NULL;
    }
    cs2_bytes_free(&file);

    snprintf(script->name, sizeof script->name, "%s", path);
    script->image = image;
    script->image_size = image_size;
    script->code = image + code_at;
    script->code_size = code_end - code_at;
    script->memory = memory;
    script->memory_size = globals + stack;
    script->globals_size = globals;
    script->sp = script->frame = script->base = globals;
    script->saved = variables;
    script->saved_size = KCS_VARIABLES;
    script->saved_is_ours = 1;

    /*
     * The strings are laid into the script's memory where the script expects to
     * find them: a table of {where it is in the file, where it goes in memory}
     * and the bytes after it.
     */
    for (uint32_t i = 0; i < strings; i++) {
        size_t entry = (size_t) strings_at + (size_t) i * 8u;
        if (entry + 8u > plain) break;
        uint32_t from = cs2_u32(image, plain, entry);
        uint32_t to   = cs2_u32(image, plain, entry + 4);
        size_t at = (size_t) strings_at + from;
        if (at >= plain || to >= script->memory_size) continue;
        size_t length = strnlen((const char *) image + at, plain - at);
        if (to + length + 1u > script->memory_size) continue;
        memcpy(memory + to, image + at, length + 1u);
    }
    return script;
}

void cs2_kcs_free(cs2_kcs *script) {
    if (script == NULL) return;
    free(script->image);
    free(script->memory);
    if (script->saved_is_ours) free(script->saved);
    free(script);
}

/*
 * The persistent variables. One script is one screen of the game and they hand
 * over to each other, so whatever runs them owns this block and lends the same
 * one to each; a script left to itself gets one of its own.
 */
void cs2_kcs_set_variables(cs2_kcs *script, void *variables, uint32_t size) {
    if (script->saved_is_ours) free(script->saved);
    script->saved = variables;
    script->saved_size = size;
    script->saved_is_ours = 0;
}

void *cs2_kcs_variables(cs2_kcs *script, uint32_t *size) {
    if (size != NULL) *size = script->saved_size;
    return script->saved;
}

void cs2_kcs_set_gcall(cs2_kcs *script, cs2_kcs_gcall gcall, void *context) {
    script->gcall = gcall;
    script->gcall_context = context;
}

void cs2_kcs_trace(cs2_kcs *script, int on) {
    script->trace = on;
}

void cs2_kcs_suspend(cs2_kcs *script, uint32_t answer_address) {
    script->suspended = 1;
    script->suspend_address = answer_address;
    script->suspend_answer = 0;
}

void cs2_kcs_answer(cs2_kcs *script, uint32_t value) {
    script->suspend_answer = value;
}

int cs2_kcs_suspended(const cs2_kcs *script) { return script->suspended; }
uint32_t cs2_kcs_suspend_address(const cs2_kcs *script) { return script->suspend_address; }

uint64_t cs2_kcs_instructions(const cs2_kcs *script) { return script->instructions; }
uint32_t cs2_kcs_pc(const cs2_kcs *script) { return script->pc; }
const char *cs2_kcs_name(const cs2_kcs *script) { return script->name; }

/* ------------------------------------------------------------- the memory */

static void fault(cs2_kcs *script, const char *format, ...) {
    char message[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    if (!script->fault) cs2_set_error("%s at %#x: %s", script->name, script->pc, message);
    script->fault = 1;
}

/*
 * An address with its top bit set is in the persistent region - the game's own
 * saved variables - and everything else is an offset into the script's memory.
 * The original follows a chain of further blocks past the end of the memory;
 * nothing in this game's scripts has reached one, and reaching one here is a
 * fault rather than a silent zero.
 */
void *cs2_kcs_at(cs2_kcs *script, uint32_t address, uint32_t size) {
    if (address & 0x80000000u) {
        uint32_t at = address & 0x7FFFFFFFu;
        if (script->saved == NULL || at + size > script->saved_size || at + size < at) {
            fault(script, "address %#x is outside the saved variables", address);
            return NULL;
        }
        return script->saved + at;
    }
    if (address + size > script->memory_size || address + size < address) {
        fault(script, "address %#x is outside the script's memory", address);
        return NULL;
    }
    return script->memory + address;
}

const char *cs2_kcs_string(cs2_kcs *script, uint32_t address) {
    uint8_t *at = cs2_kcs_at(script, address, 1);
    if (at == NULL) return NULL;
    uint8_t *end = (address & 0x80000000u) ? script->saved + script->saved_size
                                           : script->memory + script->memory_size;
    for (uint8_t *look = at; look < end; look++) {
        if (*look == 0) return (const char *) at;
    }
    return NULL;
}

uint32_t cs2_kcs_argument(const uint8_t *arguments, uint32_t argument_size, uint32_t index) {
    if (arguments == NULL || (index + 1u) * 4u > argument_size) return 0;
    return cs2_u32(arguments, argument_size, (size_t) index * 4u);
}

static uint32_t load32(cs2_kcs *script, uint32_t address) {
    uint8_t *at = cs2_kcs_at(script, address, 4);
    if (at == NULL) return 0;
    return cs2_u32(at, 4, 0);
}

static void push(cs2_kcs *script, uint32_t value) {
    if (script->sp + 4u > script->memory_size) {
        fault(script, "the stack is full");
        return;
    }
    memcpy(script->memory + script->sp, &value, 4);
    script->sp += 4;
}

static uint32_t pop(cs2_kcs *script) {
    if (script->sp < 4u || script->sp - 4u < script->globals_size) {
        fault(script, "the stack is empty");
        return 0;
    }
    script->sp -= 4;
    uint32_t value;
    memcpy(&value, script->memory + script->sp, 4);
    return value;
}

/* -------------------------------------------------------------- operands */

static int is_float(uint8_t flags) { return (flags & 0x0Fu) == 4u; }
static int is_signed(uint8_t flags) { return (flags & 0x20u) != 0u; }

static float as_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

static uint32_t from_float(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return bits;
}

/* Reads what is at an address, in the width and sign the operand's flags say. */
static uint32_t widen(cs2_kcs *script, uint32_t address, uint8_t flags) {
    if (flags & 0x80u) return load32(script, address);
    switch (flags & 0x0Fu) {
    case 1: {
        uint8_t *at = cs2_kcs_at(script, address, 1);
        if (at == NULL) return 0;
        return is_signed(flags) ? (uint32_t) (int32_t) (int8_t) *at : (uint32_t) *at;
    }
    case 2: {
        uint8_t *at = cs2_kcs_at(script, address, 2);
        if (at == NULL) return 0;
        uint16_t value = cs2_u16(at, 2, 0);
        return is_signed(flags) ? (uint32_t) (int32_t) (int16_t) value : (uint32_t) value;
    }
    default:
        return load32(script, address);
    }
}

/* The raw parts of an operand: its flags byte and its dword, out of the code. */
static int take_operand(cs2_kcs *script, uint8_t *flags, uint32_t *word) {
    if (script->pc + 5u > script->code_size) {
        fault(script, "the script ends in the middle of an instruction");
        return -1;
    }
    *flags = script->code[script->pc];
    *word = cs2_u32(script->code, script->code_size, script->pc + 1u);
    script->pc += 5u;
    return 0;
}

/*
 * One operand as the machine sees it. Flag 0x40 means the value is on the stack
 * rather than in the code (the dword is still there and is stepped over), and
 * flag 0x10 means what we have is an address to read through.
 */
static uint32_t operand(cs2_kcs *script, uint8_t *flags_out) {
    uint8_t flags;
    uint32_t word;
    if (take_operand(script, &flags, &word) != 0) {
        if (flags_out != NULL) *flags_out = 0;
        return 0;
    }
    if (flags_out != NULL) *flags_out = flags;
    uint32_t value = (flags & 0x40u) ? pop(script) : word;
    /* Without 0x10 the value is what it is: neither path widens it. */
    return (flags & 0x10u) ? widen(script, value, flags) : value;
}

/* An operand that is an address to be written through, never dereferenced. */
static uint32_t address_operand(cs2_kcs *script, uint8_t *flags_out) {
    uint8_t flags;
    uint32_t word;
    if (take_operand(script, &flags, &word) != 0) {
        if (flags_out != NULL) *flags_out = 0;
        return 0;
    }
    if (flags_out != NULL) *flags_out = flags;
    return (flags & 0x40u) ? pop(script) : word;
}

/* Stores a value at an address in the width the address operand's flags say. */
static void store(cs2_kcs *script, uint32_t address, uint8_t flags, uint32_t value) {
    if (flags & 0x80u) {
        uint8_t *at = cs2_kcs_at(script, address, 4);
        if (at != NULL) memcpy(at, &value, 4);
        return;
    }
    switch (flags & 0x0Fu) {
    case 1: {
        uint8_t *at = cs2_kcs_at(script, address, 1);
        if (at != NULL) *at = (uint8_t) value;
        break;
    }
    case 2: {
        uint8_t *at = cs2_kcs_at(script, address, 2);
        if (at != NULL) {
            uint16_t narrow = (uint16_t) value;
            memcpy(at, &narrow, 2);
        }
        break;
    }
    default: {
        uint8_t *at = cs2_kcs_at(script, address, 4);
        if (at != NULL) memcpy(at, &value, 4);
        break;
    }
    }
}

/* What a store of that width pushes back: the value as it now reads. */
static uint32_t stored_value(uint8_t flags, uint32_t value) {
    if (flags & 0x80u) return value;
    switch (flags & 0x0Fu) {
    case 1: return (uint32_t) (uint8_t) value;
    case 2: return (uint32_t) (uint16_t) value;
    default: return value;
    }
}

/* ------------------------------------------------------------ the opcodes */

static uint32_t arithmetic(uint8_t op,
                           uint32_t left, uint8_t left_flags,
                           uint32_t right, uint8_t right_flags) {
    int floating = is_float(left_flags) || is_float(right_flags);
    int signed_ = is_signed(left_flags) || is_signed(right_flags);
    switch (op) {
    case 0x01:
        return floating ? from_float(as_float(left) + as_float(right)) : left + right;
    case 0x02:
        return floating ? from_float(as_float(left) - as_float(right)) : left - right;
    case 0x03:
        return floating ? from_float(as_float(left) * as_float(right))
                        : (uint32_t) ((int32_t) left * (int32_t) right);
    case 0x04:
        if (floating) {
            float divisor = as_float(right);
            return divisor == 0.0f ? 0u : from_float(as_float(left) / divisor);
        }
        if (right == 0) return 0;
        return signed_ ? (uint32_t) ((int32_t) left / (int32_t) right) : left / right;
    case 0x05:
        if (right == 0) return 0;
        return signed_ ? (uint32_t) ((int32_t) left % (int32_t) right) : left % right;
    case 0x06: return left & right;
    case 0x07: return left | right;
    case 0x09: return left ^ right;
    case 0x0A: return left << (right & 31u);
    case 0x0B: return left >> (right & 31u);
    case 0x0D: return (left && right) ? 1u : 0u;
    case 0x0E: return (left || right) ? 1u : 0u;
    default: break;
    }

    /* The comparisons: floating by the left operand's tag, as the original is. */
    int result = 0;
    if (is_float(left_flags)) {
        float a = as_float(left), b = as_float(right);
        switch (op) {
        case 0x0F: result = a > b; break;
        case 0x10: result = a < b; break;
        case 0x11: result = a >= b; break;
        case 0x12: result = a <= b; break;
        case 0x13: result = a == b; break;
        default:   result = a != b; break;
        }
    } else if (signed_) {
        int32_t a = (int32_t) left, b = (int32_t) right;
        switch (op) {
        case 0x0F: result = a > b; break;
        case 0x10: result = a < b; break;
        case 0x11: result = a >= b; break;
        case 0x12: result = a <= b; break;
        case 0x13: result = a == b; break;
        default:   result = a != b; break;
        }
    } else {
        switch (op) {
        case 0x0F: result = left > right; break;
        case 0x10: result = left < right; break;
        case 0x11: result = left >= right; break;
        case 0x12: result = left <= right; break;
        case 0x13: result = left == right; break;
        default:   result = left != right; break;
        }
    }
    return (uint32_t) result;
}

/* The compound assignments, 0x1B to 0x24, mapped to the plain operation. */
static uint8_t compound_operation(uint8_t op) {
    switch (op) {
    case 0x1B: return 0x05;  /* %=  */
    case 0x1C: return 0x06;  /* &=  */
    case 0x1D: return 0x03;  /* *=  */
    case 0x1E: return 0x01;  /* +=  */
    case 0x1F: return 0x02;  /* -=  */
    case 0x20: return 0x04;  /* /=  */
    case 0x21: return 0x0A;  /* <<= */
    case 0x22: return 0x0B;  /* >>= */
    case 0x23: return 0x09;  /* ^=  */
    default:   return 0x07;  /* |=  */
    }
}

static int do_gcall(cs2_kcs *script, uint32_t at_opcode) {
    uint32_t id = operand(script, NULL);
    uint32_t argument_size = operand(script, NULL);
    if (script->fault) return 0;
    if (argument_size > script->sp - script->globals_size) {
        fault(script, "engine function %u wants %u argument bytes that are not there",
              id, argument_size);
        return 0;
    }
    script->sp -= argument_size;

    uint32_t answer = 0;
    const uint8_t *arguments = script->memory + script->sp;
    int result = script->gcall != NULL
        ? script->gcall(script->gcall_context, script, id, arguments, argument_size, &answer)
        : CS2_KCS_UNWRITTEN;
    /*
     * A call nobody has written yet answers zero and is let through, so that a
     * run gets as far as it can and reports the whole path it wanted rather
     * than stopping at the first function that is missing. Answering is the
     * safe half of that guess: a caller that wanted no answer drops it at the
     * end of the statement anyway, while a caller that wanted one and got
     * nothing would take someone else's value off the stack.
     */
    int written = result >= 0;
    if (!written) {
        answer = 0;
        result = CS2_KCS_DONE_VALUE;
    }

    /* Each function is named once: this is how the boot path names itself. */
    if (id < KCS_GCALL_COUNT && !script->seen[id]) {
        script->seen[id] = 1;
        if (script->trace) {
            cs2_log("%s: engine function %u (%u argument bytes)%s", script->name, id,
                    argument_size, written ? "" : ", which is not written yet");
        }
    }

    switch (result) {
    case CS2_KCS_DONE_VALUE:
        push(script, answer);
        return 1;
    case CS2_KCS_DONE:
        return 1;
    case CS2_KCS_DONE_VALUE_YIELD:
        push(script, answer);
        return 0;
    case CS2_KCS_AGAIN:
        /* Not finished: put the arguments back and come here again next frame. */
        script->sp += argument_size;
        script->pc = at_opcode;
        return 0;
    default:
        return 0;
    }
}

/*
 * One instruction. Returns 1 to keep going this frame, 0 to stop until the next
 * one, and -1 when the script has ended or something has gone wrong.
 */
static int step(cs2_kcs *script) {
    if (script->pc >= script->code_size) return -1;
    uint32_t at_opcode = script->pc;
    uint8_t op = script->code[script->pc++];
    script->instructions++;

    uint8_t left_flags = 0, right_flags = 0;
    uint32_t left = 0, right = 0, address = 0, value = 0;

    switch (op) {
    case 0x00:
        return 1;

    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x06: case 0x07: case 0x09: case 0x0A: case 0x0B:
    case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11:
    case 0x12: case 0x13: case 0x14:
        left = operand(script, &left_flags);
        right = operand(script, &right_flags);
        push(script, arithmetic(op, left, left_flags, right, right_flags));
        return 1;

    case 0x08:  /* bitwise not */
        left = operand(script, &left_flags);
        push(script, ~left);
        return 1;

    case 0x0C:  /* logical not */
        left = operand(script, &left_flags);
        push(script, left == 0 ? 1u : 0u);
        return 1;

    case 0x1A:  /* negate */
        left = operand(script, &left_flags);
        push(script, is_float(left_flags) ? from_float(-as_float(left))
                                          : (uint32_t) (-(int32_t) left));
        return 1;

    case 0x15:  /* store as an expression: the address's width picks the store */
        address = address_operand(script, &left_flags);
        value = operand(script, &right_flags);
        store(script, address, left_flags, value);
        push(script, stored_value(left_flags, value));
        return 1;

    case 0x16: case 0x17: case 0x18: case 0x19: {  /* ++x, --x, x++, x-- */
        address = address_operand(script, &left_flags);
        uint32_t before = widen(script, address, left_flags);
        uint32_t after;
        if (is_float(left_flags)) {
            float step_by = (op == 0x16 || op == 0x18) ? 1.0f : -1.0f;
            after = from_float(as_float(before) + step_by);
        } else {
            after = (op == 0x16 || op == 0x18) ? before + 1u : before - 1u;
        }
        store(script, address, left_flags, after);
        /*
         * The original pushes the whole dword that is at the address afterwards,
         * whatever the width of the store was; a pre-form pushes it after the
         * change and a post-form before it.
         */
        push(script, (op == 0x16 || op == 0x17) ? load32(script, address)
                                                : stored_value(left_flags, before));
        return 1;
    }

    case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: {
        address = address_operand(script, &left_flags);
        value = operand(script, &right_flags);
        uint32_t before = load32(script, address);  /* read as a dword, as the original is */
        uint32_t after = arithmetic(compound_operation(op),
                                    before, left_flags, value, right_flags);
        store(script, address, left_flags, after);
        push(script, after);
        return 1;
    }

    case 0x25: {  /* push a value: a raw four bytes, with no widening */
        uint8_t flags;
        uint32_t word;
        if (take_operand(script, &flags, &word) != 0) return -1;
        if ((flags & 0x40u) && !(flags & 0x10u)) return 1;  /* already on the stack */
        value = (flags & 0x40u) ? pop(script) : word;
        push(script, (flags & 0x10u) ? load32(script, value) : value);
        return 1;
    }

    case 0x26:  /* assignment as a statement: pops the value, pushes nothing */
        address = address_operand(script, &left_flags);
        value = pop(script);
        store(script, address, left_flags, value);
        return 1;

    case 0x27: case 0x28:  /* jump if zero, jump if not zero */
        left = operand(script, &left_flags);
        value = pop(script);
        if ((op == 0x27) == (value == 0)) script->pc = left;
        script->sp = script->frame;
        return 1;

    case 0x29:  /* jump */
        left = operand(script, &left_flags);
        script->pc = left;
        return 1;

    case 0x2A: {  /* call: the return address, the marks, and a new frame */
        uint32_t target = operand(script, &left_flags);
        uint32_t argument_size = operand(script, &right_flags);
        push(script, script->pc);
        push(script, script->base);
        push(script, script->frame);
        if (script->fault) return -1;
        script->frame = script->sp;
        script->base = script->sp - argument_size - 12u;
        script->pc = target;
        return 1;
    }

    case 0x2B:
        return do_gcall(script, at_opcode);

    case 0x2C: {  /* return: the answer is copied down over the arguments */
        uint32_t answer_size = operand(script, &left_flags);
        uint32_t argument_size = operand(script, &right_flags);
        uint32_t answer_at = script->sp - answer_size;
        script->sp = script->base + 12u + argument_size;
        script->frame = pop(script);
        script->base = pop(script);
        script->pc = pop(script);
        if (script->fault) return -1;
        script->sp -= argument_size;
        if (answer_size != 0) {
            if (answer_at + answer_size > script->memory_size
                || script->sp + answer_size > script->memory_size) {
                fault(script, "a return value does not fit");
                return -1;
            }
            memmove(script->memory + script->sp, script->memory + answer_at, answer_size);
            script->sp += answer_size;
        }
        return 1;
    }

    case 0x2D:  /* drop one slot */
        pop(script);
        return 1;

    case 0x2E: {  /* push a value with no dereference: an address */
        uint8_t flags;
        uint32_t word;
        if (take_operand(script, &flags, &word) != 0) return -1;
        push(script, (flags & 0x40u) ? pop(script) : word);
        return 1;
    }

    case 0x2F: {  /* push what an address points at */
        uint8_t flags;
        uint32_t word;
        if (take_operand(script, &flags, &word) != 0) return -1;
        value = (flags & 0x40u) ? pop(script) : word;
        push(script, load32(script, value));
        return 1;
    }

    case 0x30: {  /* block copy from the top of the stack */
        address = address_operand(script, &left_flags);
        uint32_t size = operand(script, &right_flags);
        uint8_t *to = cs2_kcs_at(script, address, size);
        if (to == NULL) return -1;
        if (size > script->sp - script->globals_size) {
            fault(script, "a block copy of %u bytes is not on the stack", size);
            return -1;
        }
        memmove(to, script->memory + script->sp - size, size);
        return 1;
    }

    case 0x31:  /* enter: room for the locals */
        left = operand(script, &left_flags);
        script->frame += left;
        script->sp = script->frame;
        if (script->frame > script->memory_size) {
            fault(script, "the stack is full");
            return -1;
        }
        return 1;

    case 0x32:  /* leave */
        left = operand(script, &left_flags);
        script->frame -= left;
        return 1;

    case 0x33:  /* switch: remember what is being compared against */
        script->switch_value = pop(script);
        return 1;

    case 0x34:  /* case */
        left = operand(script, &left_flags);
        right = operand(script, &right_flags);
        if (left == script->switch_value) script->pc = right;
        return 1;

    case 0x35: {  /* case of a range, either way round */
        uint32_t low = operand(script, &left_flags);
        uint32_t high = operand(script, &right_flags);
        uint32_t target = operand(script, NULL);
        uint32_t least = low < high ? low : high;
        uint32_t most = low < high ? high : low;
        if (script->switch_value >= least && script->switch_value <= most) script->pc = target;
        return 1;
    }

    case 0x36:  /* unsigned to float */
        left = operand(script, &left_flags);
        push(script, from_float((float) left));
        return 1;

    case 0x37:  /* signed to float */
        left = operand(script, &left_flags);
        push(script, from_float((float) (int32_t) left));
        return 1;

    case 0x38:  /* float to unsigned */
        left = operand(script, &left_flags);
        push(script, (uint32_t) as_float(left));
        return 1;

    case 0x39:  /* float to signed, truncating */
        left = operand(script, &left_flags);
        push(script, (uint32_t) (int32_t) as_float(left));
        return 1;

    case 0x3A:  /* duplicate the top of the stack */
        value = pop(script);
        push(script, value);
        push(script, value);
        return 1;

    case 0x3B:  /* the script is finished */
        return -1;

    case 0x3C:  /* end of statement: whatever the expression left is dropped */
        script->sp = script->frame;
        return 1;

    case 0x3D:  /* the address of a local */
        left = operand(script, &left_flags);
        push(script, script->base + left);
        return 1;

    case 0x3E: {  /* an element or a member: an address plus an offset */
        uint8_t flags;
        uint32_t word;
        if (take_operand(script, &flags, &word) != 0) return -1;
        value = (flags & 0x40u) ? pop(script) : word;
        if (flags & 0x10u) value = load32(script, value);
        push(script, value + operand(script, NULL));
        return 1;
    }

    default:
        fault(script, "opcode %#x is not part of the machine", op);
        return -1;
    }
}

int cs2_kcs_frame(cs2_kcs *script, uint32_t budget) {
    if (script->fault) return -1;
    /*
     * A frame begins by answering whatever the last one stopped waiting for.
     * The answer is on the stack for the test that follows the call, and it is
     * zero unless something happened; the address the caller gave is where the
     * engine will have put what happened.
     */
    if (script->suspended) {
        script->suspended = 0;
        push(script, script->suspend_answer);
        script->suspend_answer = 0;
        if (script->fault) return -1;
    }
    for (uint32_t i = 0; budget == 0 || i < budget; i++) {
        int more = step(script);
        if (script->fault) return -1;
        if (more < 0) return 0;
        if (more == 0) return 1;
    }
    fault(script, "the script has run %u instructions without stopping for a frame", budget);
    return -1;
}
