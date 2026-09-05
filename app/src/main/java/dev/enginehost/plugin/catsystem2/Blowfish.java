package dev.enginehost.plugin.catsystem2;

/**
 * Blowfish as CatSystem2 uses it. The two byte-array helpers deliberately differ
 * in block byte order: the engine's own code loads big-endian words when it
 * encrypts and little-endian words when it decrypts, and both halves of that
 * asymmetry are needed here (V_CODE2 and archive payloads are deciphered
 * little-endian; the key-schedule feeds words directly).
 */
final class Blowfish {
    private static final int ROUNDS = 16;

    private final int[] p = new int[18];
    private final int[][] s = new int[4][256];

    Blowfish(byte[] key) {
        if (key == null || key.length == 0) throw new IllegalArgumentException("empty Blowfish key");
        System.arraycopy(BlowfishTables.P, 0, p, 0, p.length);
        for (int i = 0; i < 4; i++) System.arraycopy(BlowfishTables.S[i], 0, s[i], 0, 256);

        int j = 0;
        for (int i = 0; i < p.length; i++) {
            int data = 0;
            for (int k = 0; k < 4; k++) {
                data = (data << 8) | (key[j] & 0xff);
                if (++j >= key.length) j = 0;
            }
            p[i] ^= data;
        }
        int[] block = new int[2];
        for (int i = 0; i < p.length; i += 2) {
            encipher(block);
            p[i] = block[0];
            p[i + 1] = block[1];
        }
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 256; k += 2) {
                encipher(block);
                s[i][k] = block[0];
                s[i][k + 1] = block[1];
            }
        }
    }

    private int f(int x) {
        int a = (x >>> 24) & 0xff, b = (x >>> 16) & 0xff, c = (x >>> 8) & 0xff, d = x & 0xff;
        return ((s[0][a] + s[1][b]) ^ s[2][c]) + s[3][d];
    }

    /** In-place on a two-word block {left, right}. */
    private void encipher(int[] block) {
        int left = block[0], right = block[1];
        for (int i = 0; i < ROUNDS; i++) {
            left ^= p[i];
            right ^= f(left);
            int swap = left; left = right; right = swap;
        }
        int swap = left; left = right; right = swap;
        right ^= p[ROUNDS];
        left ^= p[ROUNDS + 1];
        block[0] = left;
        block[1] = right;
    }

    void decipher(int[] block) {
        int left = block[0], right = block[1];
        for (int i = ROUNDS + 1; i > 1; i--) {
            left ^= p[i];
            right ^= f(left);
            int swap = left; left = right; right = swap;
        }
        int swap = left; left = right; right = swap;
        right ^= p[1];
        left ^= p[0];
        block[0] = left;
        block[1] = right;
    }

    /** Deciphers whole 8-byte blocks in place, reading each block little-endian. */
    void decipherLittleEndian(byte[] data, int length) {
        int[] block = new int[2];
        for (int offset = 0; offset + 8 <= length; offset += 8) {
            block[0] = readLe(data, offset);
            block[1] = readLe(data, offset + 4);
            decipher(block);
            writeLe(data, offset, block[0]);
            writeLe(data, offset + 4, block[1]);
        }
    }

    private static int readLe(byte[] data, int offset) {
        return (data[offset] & 0xff) | ((data[offset + 1] & 0xff) << 8)
             | ((data[offset + 2] & 0xff) << 16) | ((data[offset + 3] & 0xff) << 24);
    }

    private static void writeLe(byte[] data, int offset, int value) {
        data[offset] = (byte) value;
        data[offset + 1] = (byte) (value >>> 8);
        data[offset + 2] = (byte) (value >>> 16);
        data[offset + 3] = (byte) (value >>> 24);
    }
}
