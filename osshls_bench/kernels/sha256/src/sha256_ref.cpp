/*
 * Role:
 * Build a software reference copy of the SHA-256 block function under a
 * distinct symbol for rtl-runtime fallback checks.
 *
 * Goal:
 * Allow the rtl-runtime harness to recover deterministic golden behavior when
 * the generated RTL wrapper ABI does not yet match the software byte-memory
 * conventions for this tutorial.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define sha256_block sha256_block_ref
#include "sha256.c"

#ifdef __cplusplus
}
#endif
