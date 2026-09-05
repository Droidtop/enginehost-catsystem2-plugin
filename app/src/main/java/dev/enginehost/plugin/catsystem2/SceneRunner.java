package dev.enginehost.plugin.catsystem2;

import java.io.IOException;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Plays a CatSystem2 scene script.
 *
 * <p>A scene script is a flat list of lines: message text, a speaker's name, a
 * pause for the reader, and commands. {@link #advance()} runs commands until the
 * script wants the reader, so when it returns the stage and the message hold
 * what the screen should show. It is deliberately a scene player and not the
 * engine: it keeps no history, runs no system menu, and a command with no
 * meaning here yet is noted and skipped rather than guessed at.
 *
 * <p>What is understood: the layer commands bg, cg, fg, fw and eg with their
 * clear, blend and colour-fill forms; next, which chains one script to the one
 * after it; str, which names things; and the assignments and if conditions the
 * scripts use to work out where a character stands. Timing, transitions, sound
 * and the system screens are not.
 */
final class SceneRunner {
    /** Why the script stopped, so the screen can show the right thing. */
    enum Stop { READER, END }

    /** A script chain this long is a loop, not a story. */
    private static final int MAX_SCRIPTS = 500;

    private final GameFiles files;
    private final CstText formatter;
    private final Variables variables = new Variables();
    private final Stage stage = new Stage();
    private final Set<String> skipped = new LinkedHashSet<>();

    private String path = "";
    private List<CstScript.Line> lines = List.of();
    private int cursor;
    private String speaker = "";
    private String text = "";
    private Stop stop = Stop.READER;
    private int scriptsPlayed;

    SceneRunner(GameFiles files, CstText formatter) {
        this.files = files;
        this.formatter = formatter;
    }

    Stage stage() { return stage; }

    String path() { return path; }

    String speaker() { return speaker; }

    String text() { return text; }

    Stop stop() { return stop; }

    int cursor() { return cursor; }

    int lineCount() { return lines.size(); }

    /** The commands met but not carried out, for the log. Emptied as it is read. */
    Set<String> takeSkippedCommands() {
        Set<String> value = new LinkedHashSet<>(skipped);
        skipped.clear();
        return value;
    }

    /** Loads a script by archive path, such as {@code scene.int/ama_001.cst}. */
    void play(String scriptPath) throws IOException {
        byte[] data = files.read(scriptPath.replace('\\', '/'));
        if (data == null) throw new IOException("This game has no script \"" + scriptPath + "\"");
        lines = CstScript.read(data, scriptPath);
        path = scriptPath;
        cursor = 0;
        scriptsPlayed++;
    }

    /** Puts the reader back where a saved cursor left off, without replaying. */
    void seek(int position) { cursor = Math.max(0, Math.min(lines.size(), position)); }

    /**
     * Runs until the script asks for the reader, or until it runs out. The
     * message and the stage are what the screen should show afterwards.
     */
    void advance() {
        StringBuilder message = new StringBuilder();
        while (cursor < lines.size()) {
            CstScript.Line line = lines.get(cursor++);
            int type = line.type();
            if (type == CstScript.NAME) {
                speaker = formatter.display(line.content());
            } else if (type == CstScript.MESSAGE) {
                if (message.length() > 0) message.append('\n');
                message.append(formatter.display(line.content()));
            } else if (type == CstScript.INPUT || type == CstScript.PAGE) {
                if (message.length() > 0) {
                    text = message.toString();
                    stop = Stop.READER;
                    return;
                }
            } else if (type == CstScript.COMMAND) {
                String next = command(line.content());
                if (next == null) continue;
                if (scriptsPlayed >= MAX_SCRIPTS) break;
                try {
                    play(next);
                } catch (IOException error) {
                    note("next " + next);
                    break;
                }
            }
        }
        text = message.toString();
        stop = Stop.END;
    }

    /**
     * Carries out one command line. Returns the script to continue with when the
     * command was a jump, and null otherwise.
     */
    private String command(String line) {
        String trimmed = line.trim();
        if (trimmed.isEmpty()) return null;
        try {
            if (trimmed.startsWith("if")) return conditional(trimmed);
            if (trimmed.charAt(0) == '#') {
                assign(trimmed);
                return null;
            }
            String[] words = trimmed.split("\\s+");
            String name = words[0].toLowerCase(Locale.ROOT);
            switch (name) {
                case "bg" -> layer(Stage.BACKGROUND, words);
                case "cg" -> layer(Stage.CHARACTER, words);
                case "fg" -> layer(Stage.FOREGROUND, words);
                case "fw" -> layer(Stage.FACE, words);
                case "eg" -> layer(Stage.EFFECT, words);
                case "str" -> {
                    if (words.length >= 3) variables.string(number(words[1]), formatter.display(words[2]));
                }
                case "next" -> {
                    if (words.length >= 2) return "scene.int/" + words[1] + ".cst";
                }
                case "wait", "rdraw", "frameoff", "frameon", "wipe", "wipe2", "rwipe", "wipedef",
                     "draw", "drawdef", "auto", "autoface", "mesdraw", "view", "scene", "jumpstop",
                     "undo", "cgflag", "cgreg", "place", "title", "end_of_kaisou" -> { }
                default -> note(name);
            }
        } catch (IOException error) {
            note(trimmed);
        }
        return null;
    }

    /** {@code if (condition) command}, with or without a space after "if". */
    private String conditional(String line) throws IOException {
        int open = line.indexOf('(');
        if (open < 0) throw new IOException("if without a condition");
        int depth = 0;
        int close = -1;
        for (int i = open; i < line.length(); i++) {
            if (line.charAt(i) == '(') {
                depth++;
            } else if (line.charAt(i) == ')' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close < 0) throw new IOException("if without a closing bracket");
        int value = Expressions.evaluate(variables, line.substring(open + 1, close));
        return value == 0 ? null : command(line.substring(close + 1));
    }

    /** {@code #slot = expression}, where the slot may itself be computed. */
    private void assign(String line) throws IOException {
        int equals = -1;
        int depth = 0;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
            } else if (c == '=' && depth == 0
                       && !(i + 1 < line.length() && line.charAt(i + 1) == '=')
                       && !(i > 0 && "!<>=".indexOf(line.charAt(i - 1)) >= 0)) {
                equals = i;
                break;
            }
        }
        if (equals < 0) throw new IOException("Assignment without a value");
        int slot = Expressions.evaluate(variables, line.substring(1, equals).trim());
        variables.number(slot, Expressions.evaluate(variables, line.substring(equals + 1).trim()));
    }

    /**
     * The layer commands. They share one shape: the kind, the slot, and then
     * either nothing (clear it), a named sub-command, an image name with its
     * position, or a {@code $}-prefixed colour with the rectangle to fill.
     */
    private void layer(int kind, String[] words) throws IOException {
        if (words.length == 1) {
            stage.clear(kind);
            return;
        }
        int slot = number(words[1]);
        if (words.length == 2) {
            stage.clear(kind, slot);
            return;
        }
        String third = words[2];
        if (third.equalsIgnoreCase("blend")) {
            if (words.length >= 4) stage.blend(kind, slot, number(words[3]));
            return;
        }
        if (third.equalsIgnoreCase("scale") || third.equalsIgnoreCase("move")
            || third.equalsIgnoreCase("rotate")) {
            note(words[0] + " " + third.toLowerCase(Locale.ROOT));
            return;
        }
        if (third.startsWith("$")) {
            int colour = number(third);
            int width = words.length > 3 ? number(words[3]) : 0;
            int height = words.length > 4 ? number(words[4]) : 0;
            int x = words.length > 5 ? number(words[5]) : 0;
            int y = words.length > 6 ? number(words[6]) : 0;
            stage.place(new Stage.Layer(kind, slot, null, 0, colour, x, y, width, height, 255));
            return;
        }
        // An image name can carry the engine's own pose and animation selectors
        // after commas. Which frame each selector picks is not worked out yet,
        // so the first frame is drawn and the selectors are noted.
        int comma = third.indexOf(',');
        String image = comma < 0 ? third : third.substring(0, comma);
        if (comma >= 0) note(words[0] + " parts");
        int x = words.length > 3 ? number(words[3]) : 0;
        int y = words.length > 4 ? number(words[4]) : 0;
        stage.place(new Stage.Layer(kind, slot, image, 0, 0, x, y, 0, 0, 255));
    }

    private int number(String word) throws IOException {
        return Expressions.evaluate(variables, word);
    }

    private void note(String what) {
        if (!what.isEmpty() && skipped.size() < 200) skipped.add(what);
    }
}
