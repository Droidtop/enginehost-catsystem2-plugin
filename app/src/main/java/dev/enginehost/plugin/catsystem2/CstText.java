package dev.enginehost.plugin.catsystem2;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Turns a CST message line into the text a reader should see.
 *
 * <p>Two things happen. First the game's own substitutions from
 * {@code SCRIPT/replace} in startup.xml, which every release defines for
 * itself: Labyrinth of Grisaia maps {@code \_} to a space. Then the markup the
 * message format itself carries: {@code [word]} groups, which mark where a line
 * may break and are not meant to be seen, and backslash escapes such as
 * {@code \fn}, which switch fonts and waits this renderer has no equivalent
 * for. {@code \n} is the one escape with a plain meaning here.
 */
final class CstText {
    private final List<String[]> replacements;

    private CstText(List<String[]> replacements) { this.replacements = replacements; }

    static CstText from(Startup startup) {
        List<String[]> replacements = new ArrayList<>();
        if (startup != null) {
            for (Map.Entry<String, String> entry : startup.values().entrySet()) {
                String path = entry.getKey();
                if (!path.startsWith("SCRIPT/replace/") || !path.endsWith("/find")) continue;
                String replacement = startup.value(path.substring(0, path.length() - "find".length()) + "rep");
                replacements.add(new String[] { entry.getValue(), replacement == null ? "" : replacement });
            }
        }
        return new CstText(replacements);
    }

    String display(String raw) {
        String value = raw;
        for (String[] rule : replacements) {
            if (!rule[0].isEmpty()) value = value.replace(rule[0], rule[1]);
        }
        StringBuilder out = new StringBuilder(value.length());
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '\\' && i + 1 < value.length()) {
                // A short, closed set: anything not listed drops the backslash
                // and the single character naming the effect, and nothing else,
                // so an unknown escape can never swallow the words after it.
                if (value.startsWith("\\fn", i)) {
                    i += 2;
                } else if (value.charAt(i + 1) == 'n') {
                    out.append('\n');
                    i++;
                } else {
                    i++;
                }
            } else if (c != '[' && c != ']') {
                out.append(c);
            }
        }
        return out.toString();
    }
}
