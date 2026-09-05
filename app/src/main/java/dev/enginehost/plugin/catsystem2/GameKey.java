package dev.enginehost.plugin.catsystem2;

import java.io.File;
import java.io.IOException;
import java.nio.charset.Charset;

/**
 * The per-game archive key.
 *
 * <p>Every CatSystem2 release scrambles its archives with a short passphrase of
 * its own, and keeps that passphrase inside its own executable: resource
 * {@code DATA/V_CODE2}, Blowfish-encrypted under {@code KEY/KEY_CODE} (each
 * byte XOR 0xCD) or, on releases with no such resource, under the constant
 * "windmill". The archive key is a normal CRC-32 of the passphrase, folded the
 * way the engine folds it.
 *
 * <p>The executable is usually not the {@code .exe} the person clicks: that is
 * a small launcher, and the engine proper is a {@code .bin} beside it. So every
 * PE-shaped file in the game folder is examined and the first one carrying
 * V_CODE2 wins.
 */
final class GameKey {
    private static final Charset CP932 = Charset.forName("Shift_JIS");
    private static final String[] BINARY_SUFFIXES = { ".bin", ".exe", ".dll" };

    private final File source;
    private final String passphrase;
    private final int key;

    private GameKey(File source, String passphrase, int key) {
        this.source = source;
        this.passphrase = passphrase;
        this.key = key;
    }

    /** The file the passphrase came from, for diagnostics. */
    File source() { return source; }

    String passphrase() { return passphrase; }

    int key() { return key; }

    /** Reads the key out of the game folder, or returns null if no binary carries one. */
    static GameKey read(File gameRoot) {
        File[] candidates = gameRoot.listFiles();
        if (candidates == null) return null;
        java.util.Arrays.sort(candidates, java.util.Comparator.comparing(File::getName));
        for (String suffix : BINARY_SUFFIXES) {
            for (File candidate : candidates) {
                if (!candidate.isFile()) continue;
                if (!candidate.getName().toLowerCase(java.util.Locale.ROOT).endsWith(suffix)) continue;
                GameKey found = readFrom(candidate);
                if (found != null) return found;
            }
        }
        return null;
    }

    static GameKey readFrom(File binary) {
        try {
            PeResources resources = PeResources.read(binary);
            byte[] code = resources.resource("DATA", "V_CODE2");
            if (code == null || code.length < 8) return null;
            byte[] keyResource = resources.resource("KEY", "KEY_CODE");
            byte[] blowfishKey;
            if (keyResource != null && keyResource.length > 0) {
                blowfishKey = new byte[keyResource.length];
                for (int i = 0; i < keyResource.length; i++) blowfishKey[i] = (byte) (keyResource[i] ^ 0xCD);
            } else {
                blowfishKey = "windmill".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
            }
            byte[] plain = code.clone();
            new Blowfish(blowfishKey).decipherLittleEndian(plain, plain.length / 8 * 8);
            int end = 0;
            while (end < plain.length && plain[end] != 0) end++;
            if (end == 0) return null;
            String passphrase = new String(plain, 0, end, CP932);
            return new GameKey(binary, passphrase, encode(passphrase.getBytes(CP932)));
        } catch (IOException | RuntimeException error) {
            return null;
        }
    }

    /** The engine's passphrase fold: a normal (MSB-first) CRC-32, inverted per byte. */
    static int encode(byte[] passphrase) {
        int key = 0xffffffff;
        for (byte b : passphrase) {
            key = ~CRC32_NORMAL[((key >>> 24) ^ (b & 0xff)) & 0xff] ^ (key << 8);
        }
        return key;
    }

    private static final int[] CRC32_NORMAL = new int[256];
    static {
        for (int i = 0; i < 256; i++) {
            int value = i << 24;
            for (int bit = 0; bit < 8; bit++) {
                value = (value & 0x80000000) != 0 ? (value << 1) ^ 0x04C11DB7 : value << 1;
            }
            CRC32_NORMAL[i] = value;
        }
    }
}
