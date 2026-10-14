/*
 * Self-checking host harness for the OpenHLS MachSuite AES benchmark.
 *
 * The harness uses the standard AES-128 known-answer vector from FIPS-197:
 * plaintext 00112233445566778899aabbccddeeff encrypted with key
 * 000102030405060708090a0b0c0d0e0f must produce
 * 69c4e0d86a7b0430d8cdb78070b4c55a.  This independent fixed vector keeps the
 * benchmark deterministic across LLVM, CycleSim, and RTL verification targets.
 */

#include "aes.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" void aes(const uint8_t plaintext[AES_BLOCK_SIZE],
                    const uint8_t key[AES_KEY_SIZE],
                    uint8_t ciphertext[AES_BLOCK_SIZE]);
#endif

int main(int argc, char **argv) {
    if (argc > 1) {
        if (freopen(argv[1], "w", stdout) == NULL) {
            perror("freopen");
            return 1;
        }
    }

    const uint8_t plaintext[AES_BLOCK_SIZE] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    const uint8_t key[AES_KEY_SIZE] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    const uint8_t expected[AES_BLOCK_SIZE] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
        0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a
    };
    uint8_t ciphertext[AES_BLOCK_SIZE] = {0};

    aes(plaintext, key, ciphertext);

    int failures = 0;
    for (int i = 0; i < AES_BLOCK_SIZE; i++) {
        fprintf(stdout, "machsuite_aes cipher[%d]=%u expected=%u\n", i,
                (unsigned)ciphertext[i], (unsigned)expected[i]);
        if (ciphertext[i] != expected[i]) {
            failures = 1;
        }
    }

    if (failures) {
        fprintf(stdout, "FAIL machsuite_aes ciphertext mismatch\n");
    } else {
        fprintf(stdout, "PASS machsuite_aes\n");
    }

    return failures ? 1 : 0;
}
