/* Self-checking host harness for the MachSuite-style RSA benchmark. */

#include "rsa.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" void rsa_encrypt(const uint32_t input[RSA_COUNT],
                             uint32_t output[RSA_COUNT]);
#endif

static void init_messages(uint32_t input[RSA_COUNT]) {
    static const uint32_t kMessages[RSA_COUNT] = {
        1234567u, 2345678u, 3456789u, 4567890u,
        5678901u, 6789012u, 7890123u, 8901234u,
    };
    for (int i = 0; i < RSA_COUNT; ++i) {
        input[i] = kMessages[i];
    }
}

static void reference_rsa_encrypt(const uint32_t input[RSA_COUNT],
                                  uint32_t output[RSA_COUNT]) {
    for (int i = 0; i < RSA_COUNT; ++i) {
        uint32_t base = input[i] % RSA_MODULUS;
        uint32_t result = 1u;
        uint32_t exponent = RSA_EXPONENT;

        while (exponent > 0u) {
            if ((exponent & 1u) != 0u) {
                uint64_t prod = (uint64_t)result * (uint64_t)base;
                result = (uint32_t)(prod % (uint64_t)RSA_MODULUS);
            }
            uint64_t squared = (uint64_t)base * (uint64_t)base;
            base = (uint32_t)(squared % (uint64_t)RSA_MODULUS);
            exponent >>= 1;
        }

        output[i] = result;
    }
}

static uint64_t checksum_output(const uint32_t output[RSA_COUNT]) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < RSA_COUNT; ++i) {
        h = (h * 1099511628211ULL) ^ (uint64_t)output[i];
    }
    return h;
}

static int compare_output(const uint32_t lhs[RSA_COUNT],
                          const uint32_t rhs[RSA_COUNT], int *bad_idx) {
    for (int i = 0; i < RSA_COUNT; ++i) {
        if (lhs[i] != rhs[i]) {
            *bad_idx = i;
            return 0;
        }
    }
    *bad_idx = -1;
    return 1;
}

int main(int argc, char **argv) {
    FILE *out = stdout;
    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            fprintf(stderr, "FAIL machsuite_rsa: cannot open output file\n");
            return 2;
        }
    }

    uint32_t input[RSA_COUNT];
    uint32_t kernel[RSA_COUNT];
    uint32_t reference[RSA_COUNT];

    init_messages(input);
    rsa_encrypt(input, kernel);
    reference_rsa_encrypt(input, reference);

    int bad_idx = -1;
    int match = compare_output(kernel, reference, &bad_idx);
    fprintf(out, "kernel_checksum=%llu\n", (unsigned long long)checksum_output(kernel));
    fprintf(out, "reference_checksum=%llu\n", (unsigned long long)checksum_output(reference));
    fprintf(out, "match=%s\n", match ? "yes" : "no");
    if (!match) {
        fprintf(out, "first_mismatch=%d\n", bad_idx);
    }

    if (out != stdout) {
        fclose(out);
    }
    return match ? 0 : 2;
}
