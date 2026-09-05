package dev.enginehost.plugin.catsystem2;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.zip.InflaterInputStream;

/**
 * CatSystem2's HG-3 image format.
 *
 * <p>A file is a chain of frames; a frame is a chain of tags. {@code stdinfo}
 * carries the size and where the frame sits on the screen, {@code img####} the
 * pixels. The pixels are packed in four steps, undone here in reverse: two zlib
 * streams (the data and a bit-level command stream), a run-length pass in which
 * the commands say how many bytes to copy and how many are zero, a transposition
 * that stores the four bytes of every pixel in four separate sections of the
 * buffer with their bit pairs interleaved, and finally a delta filter along the
 * first row and then down the rows. Standard frames are stored bottom-up.
 *
 * <p>Format reference: TriggersTools.CatSystem2 (MIT), which documents the tags
 * and the packing; see THIRD_PARTY.md.
 */
final class Hg3Image {
    /** A decoded frame, in ARGB_8888, with the placement the engine draws it at. */
    record Frame(int width, int height, int offsetX, int offsetY,
                 int totalWidth, int totalHeight, int baseX, int baseY, int[] pixels) {}

    private Hg3Image() {}

    private static final int MAX_PIXELS = 64 << 20;

    /** Decodes one frame of an HG-3 file, counting from zero. */
    static Frame decode(byte[] file, int frameIndex) throws IOException {
        ByteBuffer buffer = header(file);
        int frame = require(buffer.getInt(4), file.length, "HG-3 header size");
        for (int index = 0; ; index++) {
            require(frame + 8, file.length, "HG-3 frame");
            int nextFrame = buffer.getInt(frame);
            if (index == frameIndex) return decodeFrame(file, buffer, frame + 8);
            if (nextFrame == 0) throw new IOException("HG-3 image has no frame " + frameIndex);
            frame = require(frame + nextFrame, file.length, "HG-3 frame chain");
        }
    }

    /** How many frames the file holds, which is an animation's length. */
    static int frameCount(byte[] file) throws IOException {
        ByteBuffer buffer = header(file);
        int frame = require(buffer.getInt(4), file.length, "HG-3 header size");
        for (int count = 1; ; count++) {
            require(frame + 8, file.length, "HG-3 frame");
            int next = buffer.getInt(frame);
            if (next == 0) return count;
            frame = require(frame + next, file.length, "HG-3 frame chain");
        }
    }

    private static ByteBuffer header(byte[] file) throws IOException {
        if (file.length < 12 || file[0] != 'H' || file[1] != 'G' || file[2] != '-' || file[3] != '3') {
            throw new IOException("Not an HG-3 image");
        }
        return ByteBuffer.wrap(file).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static Frame decodeFrame(byte[] file, ByteBuffer buffer, int firstTag) throws IOException {
        int[] std = null;
        int imageTag = -1;
        String kind = null;
        for (int tag = firstTag; ; ) {
            require(tag + 16, file.length, "HG-3 tag");
            String signature = signature(file, tag);
            int nextTag = buffer.getInt(tag + 8);
            int data = tag + 16;
            if (signature.startsWith("stdinfo")) {
                require(data + 40, file.length, "HG-3 stdinfo");
                std = new int[10];
                for (int i = 0; i < 10; i++) std[i] = buffer.getInt(data + i * 4);
            } else if (imageTag < 0 && signature.startsWith("img") && !signature.startsWith("imgmode")) {
                imageTag = data;
                kind = signature;
            }
            if (nextTag == 0) break;
            tag = require(tag + nextTag, file.length, "HG-3 tag chain");
        }
        if (std == null) throw new IOException("HG-3 frame has no stdinfo tag");
        if (imageTag < 0) throw new IOException("HG-3 frame has no image tag");
        if (!isNumberedImage(kind)) throw new IOException("Unsupported HG-3 image tag " + kind);

        int width = std[0];
        int height = std[1];
        int depthBits = std[2];
        if (width <= 0 || height <= 0 || (long) width * height > MAX_PIXELS) {
            throw new IOException("Implausible HG-3 frame size " + width + "x" + height);
        }
        if (depthBits != 24 && depthBits != 32) throw new IOException("Unsupported HG-3 depth " + depthBits);
        int depthBytes = (depthBits + 7) / 8;
        int stride = (width * depthBytes + 3) & ~3;

        // The image tag: an unused word, the height again, then both streams'
        // packed and unpacked lengths, and then the packed bytes themselves.
        require(imageTag + 24, file.length, "HG-3 image header");
        int packedData = buffer.getInt(imageTag + 8);
        int plainData = buffer.getInt(imageTag + 12);
        int packedCommands = buffer.getInt(imageTag + 16);
        int plainCommands = buffer.getInt(imageTag + 20);
        int at = imageTag + 24;
        byte[] data = inflate(file, at, packedData, plainData);
        byte[] commands = inflate(file, require(at + packedData, file.length, "HG-3 command stream"),
            packedCommands, plainCommands);

        byte[] pixels = undelta(unrle(data, commands), height, depthBytes, stride);
        return new Frame(width, height, std[3], std[4], std[5], std[6], std[8], std[9],
            toArgb(pixels, width, height, depthBytes, stride));
    }

    private static boolean isNumberedImage(String signature) {
        if (signature.length() < 7 || !signature.startsWith("img")) return false;
        for (int i = 3; i < signature.length(); i++) {
            if (signature.charAt(i) < '0' || signature.charAt(i) > '9') return false;
        }
        return true;
    }

    private static String signature(byte[] file, int at) {
        int end = at;
        while (end < at + 8 && file[end] != 0) end++;
        return new String(file, at, end - at, StandardCharsets.US_ASCII);
    }

    private static int require(int position, int length, String what) throws IOException {
        if (position < 0 || position > length) throw new IOException("Truncated " + what);
        return position;
    }

    private static byte[] inflate(byte[] file, int at, int packed, int plain) throws IOException {
        if (packed <= 0 || plain <= 0 || plain > MAX_PIXELS * 4L) {
            throw new IOException("Bad HG-3 stream length");
        }
        require(at + packed, file.length, "HG-3 stream");
        byte[] out = new byte[plain];
        try (InflaterInputStream inflater = new InflaterInputStream(new ByteArrayInputStream(file, at, packed))) {
            Bytes.readFully(inflater, out, 0, plain);
        }
        return out;
    }

    /**
     * Undoes the run-length pass. The command stream is read one bit at a time
     * and its lengths are Elias gamma coded: as many zero bits as the value has
     * bits below its leading one, then those bits. Its first bit says whether
     * the first run is copied from the data stream or is zero, and runs
     * alternate between the two from there.
     */
    private static byte[] unrle(byte[] data, byte[] commands) throws IOException {
        Bits bits = new Bits(commands);
        boolean copy = bits.bit();
        long length = bits.gamma();
        if (length <= 0 || length > MAX_PIXELS * 4L) throw new IOException("Implausible HG-3 unpacked size");
        byte[] out = new byte[(int) length];
        int position = 0;
        int source = 0;
        while (position < out.length) {
            long run = bits.gamma();
            if (run < 0 || position + run > out.length) throw new IOException("HG-3 run overruns the image");
            if (copy) {
                if (source + run > data.length) throw new IOException("HG-3 run overruns the data stream");
                System.arraycopy(data, source, out, position, (int) run);
                source += (int) run;
            }
            position += (int) run;
            copy = !copy;
        }
        return out;
    }

    /**
     * Undoes the transposition and the delta filter. Every pixel's four bytes
     * live in four separate quarters of the buffer, and each byte's four bit
     * pairs are spread one per quarter, so a byte is rebuilt from four lookups;
     * the low bit of the result then says whether the other seven are inverted.
     * What that leaves is a difference image: every byte is a delta from the
     * pixel to its left, and every row a delta from the row above.
     */
    private static byte[] undelta(byte[] packed, int height, int depthBytes, int stride) throws IOException {
        if (packed.length < (long) stride * height) {
            throw new IOException("HG-3 image is shorter than the size it declares");
        }
        int[] table = new int[256];
        for (int i = 0; i < 256; i++) {
            int value = i & 0xC0;
            value = (value << 6) | (i & 0x30);
            value = (value << 6) | (i & 0x0C);
            value = (value << 6) | (i & 0x03);
            table[i] = value;
        }
        byte[] out = new byte[packed.length];
        int section = packed.length / 4;
        int a = 0;
        int b = section;
        int c = section * 2;
        int d = section * 3;
        for (int at = 0; at + 4 <= out.length && d < packed.length; at += 4) {
            int value = (table[packed[a++] & 0xff] << 6) | (table[packed[b++] & 0xff] << 4)
                      | (table[packed[c++] & 0xff] << 2) | table[packed[d++] & 0xff];
            out[at] = unpack(value);
            out[at + 1] = unpack(value >>> 8);
            out[at + 2] = unpack(value >>> 16);
            out[at + 3] = unpack(value >>> 24);
        }
        for (int x = depthBytes; x < stride; x++) out[x] += out[x - depthBytes];
        for (int y = 1; y < height; y++) {
            int line = y * stride;
            for (int x = 0; x < stride; x++) out[line + x] += out[line + x - stride];
        }
        return out;
    }

    private static byte unpack(int value) {
        int single = value & 0xff;
        return (byte) ((single & 1) != 0 ? (single >>> 1) ^ 0xff : single >>> 1);
    }

    /** Rows are stored bottom-up, and each pixel as blue, green, red, alpha. */
    private static int[] toArgb(byte[] pixels, int width, int height, int depthBytes, int stride) {
        int[] argb = new int[width * height];
        for (int y = 0; y < height; y++) {
            int row = (height - 1 - y) * stride;
            int out = y * width;
            for (int x = 0; x < width; x++) {
                int at = row + x * depthBytes;
                int blue = pixels[at] & 0xff;
                int green = pixels[at + 1] & 0xff;
                int red = pixels[at + 2] & 0xff;
                int alpha = depthBytes == 4 ? pixels[at + 3] & 0xff : 0xff;
                argb[out + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }
        return argb;
    }

    /** The command stream, read as bits from the low end of each byte. */
    private static final class Bits {
        private final byte[] data;
        private int at;
        private int bit;

        Bits(byte[] data) { this.data = data; }

        boolean bit() throws IOException {
            if (bit > 7) {
                at++;
                bit = 0;
            }
            if (at >= data.length) throw new IOException("HG-3 command stream ran out");
            return ((data[at] >>> bit++) & 1) != 0;
        }

        long gamma() throws IOException {
            int digits = 0;
            while (!bit()) {
                if (++digits > 31) throw new IOException("HG-3 run length is implausibly long");
            }
            long value = 1L << digits;
            while (digits-- != 0) {
                if (bit()) value |= 1L << digits;
            }
            return value;
        }
    }
}
