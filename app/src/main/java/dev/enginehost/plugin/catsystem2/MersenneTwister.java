package dev.enginehost.plugin.catsystem2;

/**
 * MT19937 as CatSystem2 seeds it. The generator itself is stock; the seeding is
 * not: instead of Knuth's initializer the engine fills the state with the high
 * halves of two steps of a 69069 linear congruential generator per word, so a
 * stock MT19937 produces different numbers from the same seed.
 */
final class MersenneTwister {
    private static final int N = 624;
    private static final int M = 397;
    private static final int MATRIX_A = 0x9908b0df;
    private static final int SIGN_MASK = 0x80000000;
    private static final int LOWER_MASK = 0x7fffffff;

    private final int[] state = new int[N];
    private int index = N;

    MersenneTwister(int seed) { seed(seed); }

    void seed(int seed) {
        for (index = 0; index < N; index++) {
            int upper = seed & 0xffff0000;
            seed = 69069 * seed + 1;
            state[index] = upper | ((seed & 0xffff0000) >>> 16);
            seed = 69069 * seed + 1;
        }
        index = N;
    }

    int next() {
        if (index >= N) {
            int k = 0;
            for (; k < N - M; k++) {
                int y = (state[k] & SIGN_MASK) | (state[k + 1] & LOWER_MASK);
                state[k] = state[k + M] ^ (y >>> 1) ^ ((y & 1) != 0 ? MATRIX_A : 0);
            }
            for (; k < N - 1; k++) {
                int y = (state[k] & SIGN_MASK) | (state[k + 1] & LOWER_MASK);
                state[k] = state[k + M - N] ^ (y >>> 1) ^ ((y & 1) != 0 ? MATRIX_A : 0);
            }
            int y = (state[N - 1] & SIGN_MASK) | (state[0] & LOWER_MASK);
            state[N - 1] = state[M - 1] ^ (y >>> 1) ^ ((y & 1) != 0 ? MATRIX_A : 0);
            index = 0;
        }
        int y = state[index++];
        y ^= y >>> 11;
        y ^= (y << 7) & 0x9d2c5680;
        y ^= (y << 15) & 0xefc60000;
        y ^= y >>> 18;
        return y;
    }
}
