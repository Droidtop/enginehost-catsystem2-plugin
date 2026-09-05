package dev.enginehost.plugin.catsystem2;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;

/** Bounded reads. Everything here comes from game files, so nothing is trusted for its size. */
final class Bytes {
    private Bytes() {}

    static byte[] readAll(File file, long limit) throws IOException {
        long length = file.length();
        if (length > limit) throw new IOException(file.getName() + " is larger than " + limit + " bytes");
        byte[] data = new byte[(int) length];
        try (InputStream input = new java.io.FileInputStream(file)) {
            readFully(input, data, 0, data.length);
        }
        return data;
    }

    static void readFully(InputStream input, byte[] data, int offset, int length) throws IOException {
        int done = 0;
        while (done < length) {
            int count = input.read(data, offset + done, length - done);
            if (count < 0) throw new IOException("Unexpected end of file");
            done += count;
        }
    }

    static byte[] read(RandomAccessFile file, long offset, int length) throws IOException {
        byte[] data = new byte[length];
        file.seek(offset);
        file.readFully(data);
        return data;
    }
}
