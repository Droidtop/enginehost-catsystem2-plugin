package dev.enginehost.plugin.catsystem2;

import java.util.HashMap;
import java.util.Map;

/**
 * The script's variables. CatSystem2 scripts address numbers by slot, written
 * {@code #300}, and indirectly, written {@code #(950+#300)}; strings live in a
 * parallel set of slots addressed by {@code str}. Slots are sparse and a script
 * reads slots it never wrote, so an unset slot reads as zero or as the empty
 * string rather than failing.
 */
final class Variables {
    private static final int MAX_SLOT = 100_000;

    private final Map<Integer, Integer> numbers = new HashMap<>();
    private final Map<Integer, String> strings = new HashMap<>();

    int number(int slot) {
        Integer value = numbers.get(slot);
        return value == null ? 0 : value;
    }

    void number(int slot, int value) {
        if (slot >= 0 && slot < MAX_SLOT) numbers.put(slot, value);
    }

    String string(int slot) {
        String value = strings.get(slot);
        return value == null ? "" : value;
    }

    void string(int slot, String value) {
        if (slot >= 0 && slot < MAX_SLOT) strings.put(slot, value);
    }
}
