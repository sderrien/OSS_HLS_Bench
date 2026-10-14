/*
 * Public constants and top-level prototype for the compact MachSuite AES
 * benchmark port.
 *
 * The benchmark encrypts one AES-128 block with one 128-bit key.  Keeping the
 * dimensions fixed mirrors the hardware-oriented MachSuite style: the OpenHLS
 * frontend sees statically-sized external memories and the host harness can use
 * a deterministic known-answer test without dynamic allocation.
 */

#ifndef OPENHLS_MACHSUITE_AES_H
#define OPENHLS_MACHSUITE_AES_H

#include <stdint.h>

#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE 16
#define AES_ROUND_KEYS_SIZE 176

void aes(const uint8_t plaintext[AES_BLOCK_SIZE],
         const uint8_t key[AES_KEY_SIZE],
         uint8_t ciphertext[AES_BLOCK_SIZE]);

#endif
