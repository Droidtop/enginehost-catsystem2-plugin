package dev.enginehost.plugin.catsystem2;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * A CatSystem2 ".int" (KIF) archive.
 *
 * <p>Plain archives are a header followed by one 0x48-byte entry per file: a
 * name padded to 0x40 bytes, then a 32-bit offset and size. When the first
 * entry is named {@code __key__.dat} the archive is encrypted, and that entry
 * is not a file at all: its size field is the seed for the engine's Mersenne
 * Twister, whose first number is the Blowfish key covering every offset/size
 * pair and every file's bytes. Entry names are scrambled separately, with a
 * per-entry number drawn from the same generator re-seeded from the key
 * derived from the game's passphrase (see {@link GameKey}).
 */
final class IntArchive implements Closeable {
    /** Entry offsets and sizes are 32-bit, so no archive can exceed this. */
    private static final long MAX_ARCHIVE = 0xffffffffL;
    private static final int MAX_ENTRIES = 200_000;
    private static final Charset SHIFT_JIS = Charset.forName("Shift_JIS");
    private static final String ALPHABET =
        "zyxwvutsrqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA";

    record Entry(String name, long offset, int size) {}

    private final RandomAccessFile file;
    private final Blowfish cipher;
    private final Map<String, Entry> entries;

    private IntArchive(RandomAccessFile file, Blowfish cipher, Map<String, Entry> entries) {
        this.file = file;
        this.cipher = cipher;
        this.entries = entries;
    }

    /** Entry names as stored, in archive order. */
    List<String> names() {
        List<String> names = new ArrayList<>(entries.size());
        for (Entry entry : entries.values()) names.add(entry.name());
        return Collections.unmodifiableList(names);
    }

    boolean contains(String name) { return entries.containsKey(key(name)); }

    /** The decrypted bytes of one entry, or null when the archive has no such name. */
    byte[] read(String name) throws IOException {
        Entry entry = entries.get(key(name));
        if (entry == null) return null;
        byte[] data = Bytes.read(file, entry.offset(), entry.size());
        if (cipher != null) cipher.decipherLittleEndian(data, data.length / 8 * 8);
        return data;
    }

    @Override public void close() throws IOException { file.close(); }

    private static String key(String name) { return name.toLowerCase(Locale.ROOT); }

    /**
     * Opens an archive. {@code gameKey} is consulted only for encrypted
     * archives; pass null and an encrypted archive fails saying so.
     */
    static IntArchive open(File path, Integer gameKey) throws IOException {
        RandomAccessFile file = new RandomAccessFile(path, "r");
        boolean opened = false;
        try {
            long length = file.length();
            if (length < 8 || length > MAX_ARCHIVE) {
                throw new IOException(path.getName() + " is not a KIF archive");
            }
            byte[] head = Bytes.read(file, 0, 8);
            if (head[0] != 'K' || head[1] != 'I' || head[2] != 'F' || head[3] != 0) {
                throw new IOException(path.getName() + " is not a KIF archive");
            }
            int count = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN).getInt(4);
            if (count <= 0 || count > MAX_ENTRIES || 8L + (long) count * 0x48 > length) {
                throw new IOException(path.getName() + " has an implausible entry count");
            }
            byte[] table = Bytes.read(file, 8, count * 0x48);
            ByteBuffer buffer = ByteBuffer.wrap(table).order(ByteOrder.LITTLE_ENDIAN);

            IntArchive archive = isKeyEntry(table)
                ? openEncrypted(path, file, buffer, table, count, length, gameKey)
                : openPlain(path, file, buffer, table, count, length);
            opened = true;
            return archive;
        } finally {
            if (!opened) file.close();
        }
    }

    private static boolean isKeyEntry(byte[] table) {
        byte[] marker = "__key__.dat".getBytes(StandardCharsets.US_ASCII);
        if (table.length < marker.length + 1) return false;
        for (int i = 0; i < marker.length; i++) if (table[i] != marker[i]) return false;
        return table[marker.length] == 0;
    }

    private static IntArchive openPlain(File path, RandomAccessFile file, ByteBuffer buffer,
                                        byte[] table, int count, long length) throws IOException {
        Map<String, Entry> entries = new LinkedHashMap<>();
        for (int i = 0; i < count; i++) {
            int base = i * 0x48;
            String name = trimmed(table, base, 0x40);
            if (name.isEmpty()) throw new IOException(path.getName() + " has an unnamed entry");
            put(entries, path, name,
                Integer.toUnsignedLong(buffer.getInt(base + 0x40)),
                Integer.toUnsignedLong(buffer.getInt(base + 0x44)), length);
        }
        return new IntArchive(file, null, entries);
    }

    private static IntArchive openEncrypted(File path, RandomAccessFile file, ByteBuffer buffer,
                                            byte[] table, int count, long length,
                                            Integer gameKey) throws IOException {
        if (gameKey == null) {
            throw new IOException(path.getName()
                + " is encrypted and the game key could not be read from its executable");
        }
        MersenneTwister twister = new MersenneTwister(buffer.getInt(0x44));
        int seed = twister.next();
        Blowfish cipher = new Blowfish(new byte[] {
            (byte) seed, (byte) (seed >>> 8), (byte) (seed >>> 16), (byte) (seed >>> 24)
        });

        Map<String, Entry> entries = new LinkedHashMap<>();
        int[] block = new int[2];
        byte[] nameBuffer = new byte[0x40];
        for (int i = 1; i < count; i++) {
            int base = i * 0x48;
            System.arraycopy(table, base, nameBuffer, 0, 0x40);
            block[0] = buffer.getInt(base + 0x40) + i;
            block[1] = buffer.getInt(base + 0x44);
            cipher.decipher(block);
            twister.seed(gameKey + i);
            String name = decipherName(nameBuffer, twister.next());
            if (name.isEmpty()) throw new IOException(path.getName() + " has an unnamed entry");
            put(entries, path, name,
                Integer.toUnsignedLong(block[0]), Integer.toUnsignedLong(block[1]), length);
        }
        return new IntArchive(file, cipher, entries);
    }

    private static void put(Map<String, Entry> entries, File path, String name,
                            long offset, long size, long length) throws IOException {
        if (size > Integer.MAX_VALUE || offset + size > length) {
            throw new IOException(path.getName() + " entry \"" + name + "\" lies outside the archive");
        }
        entries.putIfAbsent(key(name), new Entry(name, offset, (int) size));
    }

    private static String trimmed(byte[] data, int offset, int limit) {
        int end = offset;
        while (end < offset + limit && data[end] != 0) end++;
        return new String(data, offset, end - offset, SHIFT_JIS);
    }

    /**
     * Unscrambles one entry name. Letters are rotated through a reversed
     * alphabet by a shift that advances one position per character; digits,
     * punctuation and the dot are left alone, which is why an encrypted
     * archive's table still reads as "xxxxx_1.xxx".
     */
    private static String decipherName(byte[] name, int key) {
        byte[] plain = name.clone();
        int shift = ((key >>> 24) + (key >>> 16) + (key >>> 8) + key) & 0xff;
        int i = 0;
        for (; i < plain.length && plain[i] != 0; i++) {
            int index = ALPHABET.indexOf((char) (plain[i] & 0xff));
            if (index != -1) {
                index -= Integer.remainderUnsigned(shift, 0x34);
                if (index < 0) index += 0x34;
                plain[i] = (byte) ALPHABET.charAt(0x33 - index);
            }
            shift++;
        }
        return new String(plain, 0, i, SHIFT_JIS);
    }
}
