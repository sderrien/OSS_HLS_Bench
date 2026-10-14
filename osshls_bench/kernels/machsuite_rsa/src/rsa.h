/*
 * MachSuite-style RSA benchmark adapter for OpenHLS.
 */
#ifndef MACHSUITE_RSA_H
#define MACHSUITE_RSA_H

#include <stdint.h>

#define RSA_COUNT 8
#define RSA_MODULUS 2147483647u
#define RSA_EXPONENT 65537u

#ifdef __cplusplus
extern "C" {
#endif

void rsa_encrypt(const uint32_t input[RSA_COUNT], uint32_t output[RSA_COUNT]);

#ifdef __cplusplus
}
#endif

#endif /* MACHSUITE_RSA_H */
