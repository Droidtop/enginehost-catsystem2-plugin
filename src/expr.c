#include "expr.h"

#include <stdlib.h>
#include <string.h>

#include "cs2.h"

#define MAX_SLOT 100000
#define MAX_STRINGS 512

typedef struct {
    int slot;
    char *value;
} string_slot;

struct cs2_vars {
    int32_t *numbers;
    string_slot strings[MAX_STRINGS];
    size_t string_count;
};

cs2_vars *cs2_vars_new(void) {
    cs2_vars *vars = calloc(1, sizeof *vars);
    if (vars == NULL) return NULL;
    vars->numbers = calloc(MAX_SLOT, sizeof *vars->numbers);
    if (vars->numbers == NULL) {
        free(vars);
        return NULL;
    }
    return vars;
}

void cs2_vars_free(cs2_vars *vars) {
    if (vars == NULL) return;
    for (size_t i = 0; i < vars->string_count; i++) free(vars->strings[i].value);
    free(vars->numbers);
    free(vars);
}

int32_t cs2_vars_number(const cs2_vars *vars, int slot) {
    return (slot < 0 || slot >= MAX_SLOT) ? 0 : vars->numbers[slot];
}

void cs2_vars_set_number(cs2_vars *vars, int slot, int32_t value) {
    if (slot >= 0 && slot < MAX_SLOT) vars->numbers[slot] = value;
}

const char *cs2_vars_string(const cs2_vars *vars, int slot) {
    for (size_t i = 0; i < vars->string_count; i++) {
        if (vars->strings[i].slot == slot) return vars->strings[i].value;
    }
    return "";
}

void cs2_vars_set_string(cs2_vars *vars, int slot, const char *value) {
    char *copy = malloc(strlen(value) + 1);
    if (copy == NULL) return;
    memcpy(copy, value, strlen(value) + 1);
    for (size_t i = 0; i < vars->string_count; i++) {
        if (vars->strings[i].slot == slot) {
            free(vars->strings[i].value);
            vars->strings[i].value = copy;
            return;
        }
    }
    if (vars->string_count >= MAX_STRINGS) {
        free(copy);
        return;
    }
    vars->strings[vars->string_count].slot = slot;
    vars->strings[vars->string_count].value = copy;
    vars->string_count++;
}

typedef struct {
    cs2_vars *vars;
    const char *text;
    size_t at;
    int failed;
} parser;

static int32_t parse_or(parser *p);

static void skip_space(parser *p) {
    while (p->text[p->at] == ' ' || p->text[p->at] == '\t') p->at++;
}

static int match(parser *p, const char *operator) {
    skip_space(p);
    size_t length = strlen(operator);
    if (strncmp(p->text + p->at, operator, length) != 0) return 0;
    p->at += length;
    return 1;
}

/*
 * True when the next thing is this one-character operator and not the start of a
 * longer one, so "&" does not swallow the first half of "&&" and "<" does not
 * swallow "<=" or "<<".
 */
static int peek_operator(parser *p, char operator) {
    skip_space(p);
    if (p->text[p->at] != operator) return 0;
    char next = p->text[p->at + 1];
    return next != '=' && next != operator;
}

static uint32_t parse_number(parser *p, int radix) {
    size_t start = p->at;
    uint32_t value = 0;
    for (;;) {
        char c = p->text[p->at];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (radix == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (radix == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        value = value * (uint32_t) radix + (uint32_t) digit;
        p->at++;
    }
    if (p->at == start) p->failed = 1;
    return value;
}

static int32_t parse_primary(parser *p) {
    skip_space(p);
    char c = p->text[p->at];
    if (c == '\0') {
        p->failed = 1;
        return 0;
    }
    if (c == '(') {
        p->at++;
        int32_t value = parse_or(p);
        skip_space(p);
        if (p->text[p->at] != ')') {
            p->failed = 1;
            return 0;
        }
        p->at++;
        return value;
    }
    if (c == '#') {
        p->at++;
        skip_space(p);
        int32_t slot = p->text[p->at] == '(' ? parse_primary(p) : (int32_t) parse_number(p, 10);
        return p->failed ? 0 : cs2_vars_number(p->vars, slot);
    }
    if (c == '$') {
        p->at++;
        return (int32_t) parse_number(p, 16);
    }
    if (c >= '0' && c <= '9') return (int32_t) parse_number(p, 10);
    p->failed = 1;
    return 0;
}

static int32_t parse_unary(parser *p) {
    skip_space(p);
    if (match(p, "-")) return -parse_unary(p);
    if (match(p, "+")) return parse_unary(p);
    if (match(p, "!")) return parse_unary(p) == 0 ? 1 : 0;
    if (match(p, "~")) return ~parse_unary(p);
    return parse_primary(p);
}

static int32_t parse_product(parser *p) {
    int32_t value = parse_unary(p);
    for (;;) {
        if (match(p, "*")) {
            value *= parse_unary(p);
        } else if (match(p, "/")) {
            int32_t divisor = parse_unary(p);
            value = divisor == 0 ? 0 : value / divisor;
        } else if (match(p, "%")) {
            int32_t divisor = parse_unary(p);
            value = divisor == 0 ? 0 : value % divisor;
        } else {
            return value;
        }
        if (p->failed) return 0;
    }
}

static int32_t parse_sum(parser *p) {
    int32_t value = parse_product(p);
    for (;;) {
        if (match(p, "+")) value += parse_product(p);
        else if (match(p, "-")) value -= parse_product(p);
        else return value;
        if (p->failed) return 0;
    }
}

static int32_t parse_shift(parser *p) {
    int32_t value = parse_sum(p);
    for (;;) {
        if (match(p, "<<")) value = (int32_t) ((uint32_t) value << (parse_sum(p) & 31));
        else if (match(p, ">>")) value >>= parse_sum(p) & 31;
        else return value;
        if (p->failed) return 0;
    }
}

static int32_t parse_comparison(parser *p) {
    int32_t value = parse_shift(p);
    for (;;) {
        if (match(p, "<=")) value = value <= parse_shift(p);
        else if (match(p, ">=")) value = value >= parse_shift(p);
        else if (peek_operator(p, '<')) { p->at++; value = value < parse_shift(p); }
        else if (peek_operator(p, '>')) { p->at++; value = value > parse_shift(p); }
        else return value;
        if (p->failed) return 0;
    }
}

static int32_t parse_equality(parser *p) {
    int32_t value = parse_comparison(p);
    for (;;) {
        if (match(p, "==")) value = value == parse_comparison(p);
        else if (match(p, "!=")) value = value != parse_comparison(p);
        else return value;
        if (p->failed) return 0;
    }
}

static int32_t parse_bit_and(parser *p) {
    int32_t value = parse_equality(p);
    while (peek_operator(p, '&')) {
        p->at++;
        value &= parse_equality(p);
        if (p->failed) return 0;
    }
    return value;
}

static int32_t parse_bit_or(parser *p) {
    int32_t value = parse_bit_and(p);
    while (peek_operator(p, '|')) {
        p->at++;
        value |= parse_bit_and(p);
        if (p->failed) return 0;
    }
    return value;
}

/*
 * Both sides of a logical operator are always evaluated: skipping one would
 * leave its text unparsed and the rest of the line would be read as gibberish.
 */
static int32_t parse_and(parser *p) {
    int32_t value = parse_bit_or(p);
    while (match(p, "&&")) {
        int32_t right = parse_bit_or(p);
        value = (value != 0 && right != 0) ? 1 : 0;
        if (p->failed) return 0;
    }
    return value;
}

static int32_t parse_or(parser *p) {
    int32_t value = parse_and(p);
    while (match(p, "||")) {
        int32_t right = parse_and(p);
        value = (value != 0 || right != 0) ? 1 : 0;
        if (p->failed) return 0;
    }
    return value;
}

int cs2_expr_eval(cs2_vars *vars, const char *text, int32_t *value) {
    parser p = { vars, text, 0, 0 };
    int32_t result = parse_or(&p);
    skip_space(&p);
    if (p.failed || p.text[p.at] != '\0') {
        cs2_set_error("cannot read the expression \"%s\"", text);
        return -1;
    }
    *value = result;
    return 0;
}
