/*
 * MachSuite KMP benchmark interface for the OpenHLS benchmark flow.
 *
 * This header fixes the compact benchmark sizes and exposes the top-level
 * Knuth-Morris-Pratt string matching kernel.  The kernel is intentionally kept
 * as a plain C array interface so the frontend, scheduler, cycle simulator,
 * and RTL verifier all exercise the same deterministic computation without
 * relying on file I/O or host-only data structures.
 */

#ifndef OPENHLS_BENCHMARK_MACHSUITE_KMP_H
#define OPENHLS_BENCHMARK_MACHSUITE_KMP_H

#include <stdint.h>

#define PATTERN_SIZE 4
#define STRING_SIZE 96

void kmp(const char pattern[PATTERN_SIZE], const char input[STRING_SIZE],
         int32_t kmp_next[PATTERN_SIZE], int32_t n_matches[1]);

#endif
