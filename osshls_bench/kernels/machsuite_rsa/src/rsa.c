/*
 * MachSuite-style RSA kernel adapted for OpenHLS.
 */

#include "rsa.h"

static inline __attribute__((always_inline)) uint32_t modmul(uint32_t a, uint32_t b) {
    uint64_t prod = (uint64_t)a * (uint64_t)b;
    return (uint32_t)(prod % (uint64_t)RSA_MODULUS);
}

static inline __attribute__((always_inline)) uint32_t modexp(uint32_t base, uint32_t exponent) {
    uint32_t result = 1u;
    uint32_t acc = base % RSA_MODULUS;
    uint32_t e = exponent;

    while (e > 0u) {
        if ((e & 1u) != 0u) {
            result = modmul(result, acc);
        }
        acc = modmul(acc, acc);
        e >>= 1;
    }
    return result;
}

__attribute__((annotate("hls.toplevel")))
void rsa_encrypt(const uint32_t input[RSA_COUNT], uint32_t output[RSA_COUNT]) {
    #pragma HLS pipeline
    for (int i = 0; i < RSA_COUNT; ++i) {
        output[i] = modexp(input[i], RSA_EXPONENT);
    }
}
