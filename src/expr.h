/*
 * The script's variables, and the integer expressions it is written in.
 *
 * Expressions appear wherever a number does: a coordinate can be "96+96", a
 * variable slot can itself be computed as "#(950+#300)", and a condition is an
 * expression that is true when it is not zero, as in "if ((#612&(1<<3))!=0)".
 * The operators are the ones the shipped scripts use, with C's precedence,
 * which is what the engine's own parser follows. A hexadecimal constant is
 * written with a leading "$", so a colour reads as "$80000000".
 *
 * Slots are sparse and a script reads slots it never wrote, so an unset slot
 * reads as zero or as the empty string rather than failing.
 */
#ifndef CS2_EXPR_H
#define CS2_EXPR_H

#include <stddef.h>
#include <stdint.h>

typedef struct cs2_vars cs2_vars;

cs2_vars *cs2_vars_new(void);
void cs2_vars_free(cs2_vars *vars);

int32_t cs2_vars_number(const cs2_vars *vars, int slot);
void cs2_vars_set_number(cs2_vars *vars, int slot, int32_t value);
const char *cs2_vars_string(const cs2_vars *vars, int slot);
void cs2_vars_set_string(cs2_vars *vars, int slot, const char *value);

/*
 * Evaluates a whole expression, which must use up the entire text. Returns 0 on
 * success and -1 when the text is not one, leaving the value untouched.
 */
int cs2_expr_eval(cs2_vars *vars, const char *text, int32_t *value);

#endif
