/*
 * MachSuite KMP kernel adapted for OpenHLS.
 *
 * The module provides a compact Knuth-Morris-Pratt string matching benchmark.
 * It stresses loop-carried control state, data-dependent while loops, array
 * reads through an evolving automaton state, and scalar-to-memory result
 * communication.  The implementation follows the original MachSuite KMP
 * algorithm while using only plain C constructs accepted by the OpenHLS C to
 * LLVM to OHLS pipeline.
 */

#include "kmp.h"

#include <stdio.h>

/*
 * Build the KMP prefix table for the fixed-size pattern.
 *
 * The prefix table records the length of the longest proper prefix that is also
 * a suffix for each pattern prefix.  The main search loop uses this table to
 * fall back without rescanning input characters after a mismatch.  Keeping this
 * preprocessing step in the kernel mirrors the source MachSuite benchmark and
 * gives the scheduler both a short data-dependent loop nest and the longer
 * search loop in one top-level computation.
 */
static inline __attribute__((always_inline)) void
compute_prefix_function(const char pattern[PATTERN_SIZE],
                        int32_t kmp_next[PATTERN_SIZE]) {
    int32_t k = 0;
    kmp_next[0] = 0;
    printf("kmp_dbg_prefix_init %d\n", kmp_next[0]);

    for (int32_t q = 1; q < PATTERN_SIZE; q++) {
        printf("kmp_dbg_prefix_q %d\n", q);
        printf("kmp_dbg_prefix_enter_k %d\n", k);
        while (k > 0 && pattern[k] != pattern[q]) {
            printf("kmp_dbg_prefix_fallback_from %d\n", k);
            k = kmp_next[k - 1];
            printf("kmp_dbg_prefix_fallback_to %d\n", k);
        }
        if (pattern[k] == pattern[q]) {
            k++;
        }
        kmp_next[q] = k;
        printf("kmp_dbg_prefix_exit_k %d\n", k);
        printf("kmp_dbg_prefix_next %d\n", kmp_next[q]);
    }
}

/*
 * Count all occurrences of a fixed-size pattern in a fixed-size input string.
 *
 * OpenHLS treats this function as the hardware top level.  The input and
 * pattern arrays are read-only, `kmp_next` is an observable scratch/output array
 * for the prefix table, and `n_matches[0]` returns the final match count.  The
 * search permits overlapping matches by falling back through the prefix table
 * after each full match, which is the standard KMP behavior.
 */
__attribute__((annotate("hls.toplevel")))
void kmp(const char pattern[PATTERN_SIZE], const char input[STRING_SIZE],
         int32_t kmp_next[PATTERN_SIZE], int32_t n_matches[1]) {
    int32_t q = 0;
    n_matches[0] = 0;
    printf("kmp_dbg_search_init %d\n", n_matches[0]);

    compute_prefix_function(pattern, kmp_next);

    #pragma HLS pipeline
    for (int32_t i = 0; i < STRING_SIZE; i++) {
        printf("kmp_dbg_search_i %d\n", i);
        printf("kmp_dbg_search_enter_q %d\n", q);
        printf("kmp_dbg_search_enter_matches %d\n", n_matches[0]);
        while (q > 0 && pattern[q] != input[i]) {
            printf("kmp_dbg_search_fallback_from %d\n", q);
            q = kmp_next[q - 1];
            printf("kmp_dbg_search_fallback_to %d\n", q);
        }
        if (pattern[q] == input[i]) {
            q++;
            printf("kmp_dbg_search_matched_q %d\n", q);
        } else {
            printf("kmp_dbg_search_no_match_q %d\n", q);
        }
        if (q >= PATTERN_SIZE) {
            n_matches[0]++;
            printf("kmp_dbg_search_full_match_i %d\n", i);
            printf("kmp_dbg_search_full_match_count %d\n", n_matches[0]);
            q = kmp_next[q - 1];
            printf("kmp_dbg_search_full_match_fallback_q %d\n", q);
        }
        printf("kmp_dbg_search_exit_q %d\n", q);
        printf("kmp_dbg_search_exit_matches %d\n", n_matches[0]);
    }
}
