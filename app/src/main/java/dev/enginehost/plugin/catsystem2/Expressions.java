package dev.enginehost.plugin.catsystem2;

import java.io.IOException;

/**
 * The integer expressions CatSystem2 scripts are written in.
 *
 * <p>They appear wherever a number does: a coordinate can be {@code 96+96}, a
 * variable slot can itself be computed as {@code #(950+#300)}, and a condition
 * is an expression that is true when it is not zero, as in
 * {@code if ((#612&(1<<3))!=0)}. The operators below are the ones the shipped
 * scripts actually use, with C's precedence, which is what the engine's own
 * parser follows.
 *
 * <p>A hexadecimal constant is written with a leading {@code $}; colours are
 * given that way, so {@code $80000000} is a half-transparent black.
 */
final class Expressions {
    private final Variables variables;
    private final String source;
    private int at;

    private Expressions(Variables variables, String source) {
        this.variables = variables;
        this.source = source;
    }

    /** Evaluates a whole expression, which must use up the entire text. */
    static int evaluate(Variables variables, String text) throws IOException {
        Expressions parser = new Expressions(variables, text);
        int value = parser.or();
        parser.skipSpace();
        if (parser.at < text.length()) {
            throw new IOException("Unreadable expression \"" + text + "\"");
        }
        return value;
    }

    /**
     * Evaluates the expression that starts at {@code from}, and reports where it
     * ended, so a command's arguments can be read one at a time out of a line
     * whose parts are separated by spaces but may contain spaces of their own.
     */
    static int evaluate(Variables variables, String text, int from, int[] end) throws IOException {
        Expressions parser = new Expressions(variables, text);
        parser.at = from;
        int value = parser.or();
        end[0] = parser.at;
        return value;
    }

    private int or() throws IOException {
        int value = and();
        // Both sides are always evaluated: skipping one would leave its text
        // unparsed and the rest of the line would be read as gibberish.
        while (match("||")) {
            int right = and();
            value = (value != 0 | right != 0) ? 1 : 0;
        }
        return value;
    }

    private int and() throws IOException {
        int value = bitOr();
        while (match("&&")) {
            int right = bitOr();
            value = (value != 0 & right != 0) ? 1 : 0;
        }
        return value;
    }

    private int bitOr() throws IOException {
        int value = bitAnd();
        while (peekOperator("|")) {
            expect("|");
            value |= bitAnd();
        }
        return value;
    }

    private int bitAnd() throws IOException {
        int value = equality();
        while (peekOperator("&")) {
            expect("&");
            value &= equality();
        }
        return value;
    }

    private int equality() throws IOException {
        int value = comparison();
        while (true) {
            if (match("==")) value = value == comparison() ? 1 : 0;
            else if (match("!=")) value = value != comparison() ? 1 : 0;
            else return value;
        }
    }

    private int comparison() throws IOException {
        int value = shift();
        while (true) {
            if (match("<=")) value = value <= shift() ? 1 : 0;
            else if (match(">=")) value = value >= shift() ? 1 : 0;
            else if (peekOperator("<")) { expect("<"); value = value < shift() ? 1 : 0; }
            else if (peekOperator(">")) { expect(">"); value = value > shift() ? 1 : 0; }
            else return value;
        }
    }

    private int shift() throws IOException {
        int value = sum();
        while (true) {
            if (match("<<")) value <<= sum() & 31;
            else if (match(">>")) value >>= sum() & 31;
            else return value;
        }
    }

    private int sum() throws IOException {
        int value = product();
        while (true) {
            if (match("+")) value += product();
            else if (match("-")) value -= product();
            else return value;
        }
    }

    private int product() throws IOException {
        int value = unary();
        while (true) {
            if (match("*")) {
                value *= unary();
            } else if (match("/")) {
                int divisor = unary();
                value = divisor == 0 ? 0 : value / divisor;
            } else if (match("%")) {
                int divisor = unary();
                value = divisor == 0 ? 0 : value % divisor;
            } else {
                return value;
            }
        }
    }

    private int unary() throws IOException {
        skipSpace();
        if (match("-")) return -unary();
        if (match("+")) return unary();
        if (match("!")) return unary() == 0 ? 1 : 0;
        if (match("~")) return ~unary();
        return primary();
    }

    private int primary() throws IOException {
        skipSpace();
        if (at >= source.length()) throw new IOException("Expression ends early: \"" + source + "\"");
        char c = source.charAt(at);
        if (c == '(') {
            at++;
            int value = or();
            skipSpace();
            if (at >= source.length() || source.charAt(at) != ')') {
                throw new IOException("Unclosed bracket in \"" + source + "\"");
            }
            at++;
            return value;
        }
        if (c == '#') {
            at++;
            return variables.number(slot());
        }
        if (c == '$') {
            at++;
            return (int) number(16);
        }
        if (c >= '0' && c <= '9') return (int) number(10);
        throw new IOException("Unreadable expression \"" + source + "\" at " + at);
    }

    /** The slot a {@code #} refers to: digits, or a bracketed expression. */
    private int slot() throws IOException {
        skipSpace();
        if (at < source.length() && source.charAt(at) == '(') return primary();
        return (int) number(10);
    }

    private long number(int radix) throws IOException {
        int start = at;
        long value = 0;
        while (at < source.length() && Character.digit(source.charAt(at), radix) >= 0) {
            value = value * radix + Character.digit(source.charAt(at), radix);
            if (value > 0xffffffffL) value &= 0xffffffffL;
            at++;
        }
        if (at == start) throw new IOException("Expected a number in \"" + source + "\"");
        return radix == 16 ? (int) value : value;
    }

    private void skipSpace() {
        while (at < source.length() && (source.charAt(at) == ' ' || source.charAt(at) == '\t')) at++;
    }

    private boolean match(String operator) {
        skipSpace();
        if (!source.startsWith(operator, at)) return false;
        at += operator.length();
        return true;
    }

    private void expect(String operator) {
        skipSpace();
        at += operator.length();
    }

    /**
     * True when the next thing is this one-character operator and not the start
     * of a longer one, so {@code &} does not swallow the first half of
     * {@code &&} and {@code <} does not swallow {@code <=} or {@code <<}.
     */
    private boolean peekOperator(String operator) {
        skipSpace();
        if (!source.startsWith(operator, at)) return false;
        if (at + 1 >= source.length()) return true;
        char next = source.charAt(at + 1);
        return next != '=' && next != operator.charAt(0);
    }
}
