#include <stdint.h>
#include <stdio.h>
#include <ohls_trace.h>


static inline __attribute__((always_inline)) uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static inline __attribute__((always_inline)) uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static inline __attribute__((always_inline)) uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static inline __attribute__((always_inline)) uint32_t big0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

static inline __attribute__((always_inline)) uint32_t big1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

static inline __attribute__((always_inline)) uint32_t small0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

static inline __attribute__((always_inline)) uint32_t small1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

__attribute__((annotate("hls.toplevel")))
void sha256_block(const uint8_t block[64], uint32_t state[8]) {
    // This function computes one full SHA-256 compression round on a single
    // 512-bit block and accumulates the result into the running state vector.
    // The code is written in a hardware-friendly style so each pipeline stage
    // sees explicit data movement and no hidden aliasing side effects.
    uint32_t w[64];
#pragma HLS pipeline
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
#pragma HLS pipeline
    for (int i = 16; i < 64; ++i) {
        w[i] = small1(w[i - 2]) + w[i - 7] + small0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

#pragma HLS pipeline
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + big1(e) + ch(e, f, g) + k[i] + w[i];
        uint32_t t2 = big0(a) + maj(a, b, c);
        printf(
            "sha256 round=%d t1=0x%08x t2=0x%08x a=0x%08x e=0x%08x", i, t1, t2, a, e);
        // Keep next-state values explicit to preserve the exact round-update
        // semantics under scheduled lowering and avoid accidental dependence
        // reordering in generated cycle-sim code.
        uint32_t next_a = t1 + t2;
        uint32_t next_b = a;
        uint32_t next_c = b;
        uint32_t next_d = c;
        uint32_t next_e = d + t1;
        uint32_t next_f = e;
        uint32_t next_g = f;
        uint32_t next_h = g;

        a = next_a;
        b = next_b;
        c = next_c;
        d = next_d;
        e = next_e;
        f = next_f;
        g = next_g;
        h = next_h;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;

}
