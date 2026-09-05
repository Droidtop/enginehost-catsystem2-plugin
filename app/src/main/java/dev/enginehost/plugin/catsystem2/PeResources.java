package dev.enginehost.plugin.catsystem2;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * Just enough of the Windows PE format to read string-named resources out of a
 * CatSystem2 executable. The archive key lives there (DATA/V_CODE2, unlocked
 * with KEY/KEY_CODE), so the plugin has to look inside the game's own binary;
 * it never runs it.
 */
final class PeResources {
    private final Map<String, byte[]> named = new HashMap<>();

    private PeResources() {}

    /**
     * A string-named resource, addressed by the two named levels of the
     * resource tree in either order. CatSystem2 executables store this pair the
     * opposite way round from most Windows binaries -- V_CODE2 is the type and
     * DATA the name -- and it is the pair that identifies the resource either
     * way.
     */
    byte[] resource(String type, String name) {
        byte[] found = named.get(type + '/' + name);
        return found != null ? found : named.get(name + '/' + type);
    }

    /** Returns the string-named resources of a PE file, or an empty set if it is not one. */
    static PeResources read(byte[] image) {
        PeResources resources = new PeResources();
        try {
            resources.parse(image);
        } catch (RuntimeException ignored) {
            // A truncated or hand-edited binary simply yields no resources.
            resources.named.clear();
        }
        return resources;
    }

    static PeResources read(File file) throws IOException {
        return read(Bytes.readAll(file, 64L * 1024 * 1024));
    }

    private void parse(byte[] image) {
        ByteBuffer buffer = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN);
        if (image.length < 0x40 || buffer.getShort(0) != 0x5a4d) return; // 'MZ'
        int peOffset = buffer.getInt(0x3c);
        if (peOffset < 0 || peOffset + 24 > image.length || buffer.getInt(peOffset) != 0x00004550) return;
        int coff = peOffset + 4;
        int sectionCount = Short.toUnsignedInt(buffer.getShort(coff + 2));
        int optionalSize = Short.toUnsignedInt(buffer.getShort(coff + 16));
        int optional = coff + 20;
        int magic = Short.toUnsignedInt(buffer.getShort(optional));
        int directories = optional + (magic == 0x20b ? 108 : 92);
        if (directories + 4 + 8 * 3 > image.length) return;
        int resourceRva = buffer.getInt(directories + 4 + 8 * 2);
        if (resourceRva == 0) return;

        int[][] sections = new int[sectionCount][];
        int sectionTable = optional + optionalSize;
        for (int i = 0; i < sectionCount; i++) {
            int base = sectionTable + i * 40;
            if (base + 40 > image.length) return;
            sections[i] = new int[] {
                buffer.getInt(base + 12),                                   // virtual address
                Math.max(buffer.getInt(base + 8), buffer.getInt(base + 16)),// span
                buffer.getInt(base + 20)                                    // raw pointer
            };
        }
        int root = fileOffset(sections, resourceRva, image.length);
        if (root < 0) return;
        walk(buffer, image, sections, root, root, resourceRva, null, null, 0);
    }

    private static int fileOffset(int[][] sections, int rva, int limit) {
        for (int[] section : sections) {
            long start = Integer.toUnsignedLong(section[0]);
            long span = Integer.toUnsignedLong(section[1]);
            if (Integer.toUnsignedLong(rva) >= start && Integer.toUnsignedLong(rva) < start + span) {
                long offset = Integer.toUnsignedLong(section[2]) + (Integer.toUnsignedLong(rva) - start);
                return offset >= 0 && offset < limit ? (int) offset : -1;
            }
        }
        return -1;
    }

    private void walk(ByteBuffer buffer, byte[] image, int[][] sections,
                      int root, int directory, int resourceRva,
                      String type, String name, int depth) {
        if (depth > 2 || directory < 0 || directory + 16 > image.length || named.size() > 512) return;
        int namedCount = Short.toUnsignedInt(buffer.getShort(directory + 12));
        int idCount = Short.toUnsignedInt(buffer.getShort(directory + 14));
        int total = namedCount + idCount;
        for (int i = 0; i < total; i++) {
            int entry = directory + 16 + i * 8;
            if (entry + 8 > image.length) return;
            int nameField = buffer.getInt(entry);
            int dataField = buffer.getInt(entry + 4);
            String label = null;
            if ((nameField & 0x80000000) != 0) {
                label = readName(buffer, image, root, nameField & 0x7fffffff);
                if (label == null) continue;
            } else if (depth < 2) {
                // Only string-named types and names matter here; numeric ones are
                // icons, dialogs and the rest of the standard resource tree.
                continue;
            }
            if ((dataField & 0x80000000) != 0) {
                walk(buffer, image, sections, root, root + (dataField & 0x7fffffff), resourceRva,
                     depth == 0 ? label : type, depth == 1 ? label : name, depth + 1);
            } else if (depth == 2 && type != null && name != null) {
                int data = root + dataField;
                if (data < 0 || data + 8 > image.length) continue;
                int dataOffset = fileOffset(sections, buffer.getInt(data), image.length);
                int size = buffer.getInt(data + 4);
                if (dataOffset < 0 || size < 0 || size > 1 << 20 || dataOffset + size > image.length) continue;
                byte[] value = new byte[size];
                System.arraycopy(image, dataOffset, value, 0, size);
                named.put(type + '/' + name, value);
            }
        }
    }

    private static String readName(ByteBuffer buffer, byte[] image, int root, int offset) {
        int at = root + offset;
        if (at < 0 || at + 2 > image.length) return null;
        int length = Short.toUnsignedInt(buffer.getShort(at));
        if (length > 256 || at + 2 + length * 2 > image.length) return null;
        return new String(image, at + 2, length * 2, StandardCharsets.UTF_16LE);
    }
}
