package dev.enginehost.plugin.catsystem2;

import java.nio.charset.Charset;
import java.util.HashMap;
import java.util.Map;

/**
 * The handful of values the plugin needs out of {@code startup.xml}: the
 * window title, the virtual screen size, and above all {@code SCRIPT/start},
 * the file the engine boots into.
 *
 * <p>The document is Shift-JIS and Android's XML parsers do not accept that
 * encoding, so it is decoded here and walked with a small element scanner.
 * Only leaf text is collected, addressed by its element path.
 */
final class Startup {
    private static final Charset SHIFT_JIS = Charset.forName("Shift_JIS");

    private final Map<String, String> values;

    private Startup(Map<String, String> values) { this.values = values; }

    static Startup parse(byte[] document) {
        return new Startup(scan(new String(document, SHIFT_JIS)));
    }

    /** Leaf text at an element path such as "SCRIPT/start", or null. */
    String value(String path) { return values.get(path); }

    /** Every leaf, by element path. */
    Map<String, String> values() { return java.util.Collections.unmodifiableMap(values); }

    private static Map<String, String> scan(String xml) {
        Map<String, String> values = new HashMap<>();
        java.util.ArrayDeque<String> path = new java.util.ArrayDeque<>();
        java.util.ArrayDeque<Boolean> hasChildren = new java.util.ArrayDeque<>();
        StringBuilder text = new StringBuilder();
        int at = 0;
        while (at < xml.length()) {
            int open = xml.indexOf('<', at);
            if (open < 0) break;
            if (!path.isEmpty()) text.append(xml, at, open);
            int close = xml.indexOf('>', open);
            if (close < 0) break;
            String tag = xml.substring(open + 1, close).trim();
            at = close + 1;
            if (tag.startsWith("?") || tag.startsWith("!")) {
                if (tag.startsWith("!--") && !tag.endsWith("--")) {
                    int end = xml.indexOf("-->", open);
                    at = end < 0 ? xml.length() : end + 3;
                }
                text.setLength(0);
                continue;
            }
            if (tag.startsWith("/")) {
                String name = tag.substring(1).trim();
                if (!path.isEmpty() && path.peek().equals(name)) {
                    String leaf = leaf(text.toString(), hasChildren.peek() == Boolean.TRUE);
                    String key = join(path);
                    if (!leaf.isEmpty() && !key.isEmpty()) values.putIfAbsent(key, leaf);
                    path.pop();
                    hasChildren.pop();
                }
                text.setLength(0);
                continue;
            }
            boolean selfClosing = tag.endsWith("/");
            int space = tag.indexOf(' ');
            String name = (space < 0 ? tag : tag.substring(0, space));
            if (name.endsWith("/")) name = name.substring(0, name.length() - 1);
            text.setLength(0);
            if (!name.isEmpty() && !hasChildren.isEmpty()) {
                hasChildren.pop();
                hasChildren.push(Boolean.TRUE);
            }
            if (!selfClosing && !name.isEmpty()) {
                path.push(name);
                hasChildren.push(Boolean.FALSE);
                if (path.size() > 32) return values;
            }
        }
        return values;
    }

    /**
     * The text of one element. All-whitespace text is layout when it sits
     * between child elements or spans lines, but a value when an element with
     * no children holds it on one line: Labyrinth of Grisaia's startup.xml
     * replaces the escape {@code \_} with exactly one space, written
     * {@code <rep> </rep>}, and trimming that away deletes the spacing the
     * game's own text depends on.
     */
    private static String leaf(String raw, boolean hadChildren) {
        String trimmed = raw.trim();
        if (!trimmed.isEmpty()) return trimmed;
        boolean spansLines = raw.indexOf('\n') >= 0 || raw.indexOf('\r') >= 0;
        return hadChildren || spansLines ? "" : raw;
    }

    /**
     * The element path below the document root, so a value reads as
     * "SCRIPT/start" rather than "document/SCRIPT/start".
     */
    private static String join(java.util.ArrayDeque<String> path) {
        StringBuilder joined = new StringBuilder();
        String[] names = path.toArray(new String[0]);
        for (int i = names.length - 2; i >= 0; i--) {
            if (joined.length() > 0) joined.append('/');
            joined.append(names[i]);
        }
        return joined.toString();
    }
}
