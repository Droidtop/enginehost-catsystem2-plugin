package dev.enginehost.plugin.catsystem2;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The game folder as the engine sees it: loose files layered over the ".int"
 * archives beside them.
 *
 * <p>CatSystem2 addresses content as {@code archive.int/name}, and a folder
 * named after the archive minus its extension shadows it file by file. Every
 * shipping game relies on that: Labyrinth of Grisaia carries a loose
 * {@code config/startup.xml} that must win over the one inside
 * {@code config.int}.
 *
 * <p>Archives are opened on demand. One of them is a 2.8 GB image bank, so
 * nothing here reads an entry table until something asks that archive for a
 * file.
 */
final class GameFiles implements Closeable {
    private final File root;
    private final GameKey gameKey;
    private final Map<String, File> archiveFiles = new LinkedHashMap<>();
    private final Map<String, IntArchive> openArchives = new LinkedHashMap<>();

    private GameFiles(File root, GameKey gameKey) {
        this.root = root;
        this.gameKey = gameKey;
    }

    static GameFiles open(File gameRoot) throws IOException {
        File root = gameRoot.getCanonicalFile();
        if (!root.isDirectory()) throw new IOException("CatSystem2 game folder is unreadable");
        GameFiles files = new GameFiles(root, GameKey.read(root));
        File[] children = root.listFiles();
        if (children != null) {
            java.util.Arrays.sort(children, java.util.Comparator.comparing(File::getName));
            for (File child : children) {
                String name = child.getName().toLowerCase(Locale.ROOT);
                if (child.isFile() && name.endsWith(".int")) files.archiveFiles.put(name, child);
            }
        }
        return files;
    }

    File root() { return root; }

    /** The key read from the game's executable, or null when none was found. */
    GameKey gameKey() { return gameKey; }

    /** Archive file names present in the folder, lower-cased, in name order. */
    List<String> archiveNames() { return new ArrayList<>(archiveFiles.keySet()); }

    /**
     * Reads {@code archive.int/name}, or a bare name searched across the loose
     * folder and then every archive. Returns null when nothing has that name.
     */
    byte[] read(String path) throws IOException {
        int slash = path.lastIndexOf('/');
        if (slash < 0) slash = path.lastIndexOf('\\');
        if (slash >= 0) {
            String container = path.substring(0, slash);
            String name = path.substring(slash + 1);
            byte[] loose = readLoose(stripExtension(container) + File.separator + name);
            if (loose != null) return loose;
            IntArchive archive = archive(container);
            return archive == null ? null : archive.read(name);
        }
        byte[] loose = readLoose(path);
        if (loose != null) return loose;
        for (String archiveName : archiveFiles.keySet()) {
            IntArchive archive = archive(archiveName);
            if (archive != null && archive.contains(path)) return archive.read(path);
        }
        return null;
    }

    /** Entry names of one archive, or an empty list when it is absent or unreadable. */
    List<String> namesIn(String archiveName) {
        try {
            IntArchive archive = archive(archiveName);
            return archive == null ? List.of() : archive.names();
        } catch (IOException error) {
            return List.of();
        }
    }

    /** Opens an archive by file name ("scene.int"), caching it. Null when absent. */
    IntArchive archive(String archiveName) throws IOException {
        String key = archiveName.toLowerCase(Locale.ROOT);
        if (!key.endsWith(".int")) key = key + ".int";
        IntArchive open = openArchives.get(key);
        if (open != null) return open;
        File file = archiveFiles.get(key);
        if (file == null) return null;
        IntArchive archive = IntArchive.open(file, gameKey == null ? null : gameKey.key());
        openArchives.put(key, archive);
        return archive;
    }

    private byte[] readLoose(String relative) throws IOException {
        File file = new File(root, relative).getCanonicalFile();
        if (!file.isFile() || !file.getPath().startsWith(root.getPath() + File.separator)) return null;
        return Bytes.readAll(file, 128L * 1024 * 1024);
    }

    private static String stripExtension(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    @Override public void close() {
        for (IntArchive archive : openArchives.values()) {
            try {
                archive.close();
            } catch (IOException ignored) {
                // Closing a read-only handle can only fail in ways nothing can act on.
            }
        }
        openArchives.clear();
    }
}
