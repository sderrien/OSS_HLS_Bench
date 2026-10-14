/*
 * Self-checking host harness for the OpenHLS MachSuite KMP benchmark.
 *
 * The harness supplies deterministic in-memory input data, invokes the HLS top
 * function, and compares the result against a simple independent reference.  It
 * deliberately avoids file I/O and dynamic allocation so the same benchmark can
 * be used by the LLVM, untimed C, scheduled C, cycle simulation, and RTL runtime
 * verification targets with minimal host-side assumptions.
 */

#include "kmp.h"

#include <stdio.h>

/* Export the kernel with C linkage when the RTL runtime compiles this harness
 * with a C++ compiler. */
#ifdef __cplusplus
extern "C" void kmp(const char pattern[PATTERN_SIZE],
                    const char input[STRING_SIZE],
                    int32_t kmp_next[PATTERN_SIZE], int32_t n_matches[1]);
#endif

/*
 * Reference matcher used only by the host harness.
 *
 * This intentionally uses direct comparison at every input position instead of
 * the KMP prefix table.  Using a structurally different reference makes the
 * check useful: a bug in the KMP fallback state is unlikely to be reproduced by
 * the reference implementation.
 */
static int32_t reference_count(const char pattern[PATTERN_SIZE],
                               const char input[STRING_SIZE]) {
    int32_t matches = 0;

    for (int32_t i = 0; i <= STRING_SIZE - PATTERN_SIZE; i++) {
        int equal = 1;
        for (int32_t j = 0; j < PATTERN_SIZE; j++) {
            if (input[i + j] != pattern[j]) {
                equal = 0;
                break;
            }
        }
        if (equal) {
            matches++;
        }
    }
    return matches;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (freopen(argv[1], "w", stdout) == NULL) {
            perror("freopen");
            return 1;
        }
    }

    const char pattern[PATTERN_SIZE] = {'b', 'u', 'l', 'l'};
    const char input[STRING_SIZE] = {
        'b','u','l','l','m','o','o','s','e','b','u','l','l','x','b','u',
        'l','l','b','u','l','l','a','b','u','l','l','m','o','o','s','e',
        'b','u','l','x','b','u','l','l','b','u','l','l','b','u','l','l',
        't','r','b','u','l','l','m','o','o','s','e','b','u','l','l','b',
        'u','l','l','a','a','b','u','l','l','c','b','u','l','d','b','u',
        'l','l','m','o','o','s','e','b','u','l','l','b','u','l','l','z'
    };
    int32_t kmp_next[PATTERN_SIZE] = {0, 0, 0, 0};
    int32_t n_matches[1] = {0};

    kmp(pattern, input, kmp_next, n_matches);

    int32_t expected = reference_count(pattern, input);
    int failures = 0;

    fprintf(stdout, "machsuite_kmp matches=%d expected=%d\n", n_matches[0],
            expected);
    if (n_matches[0] != expected) {
        fprintf(stdout, "FAIL machsuite_kmp match count mismatch\n");
        failures = 1;
    }

    const int32_t expected_next[PATTERN_SIZE] = {0, 0, 0, 0};
    for (int32_t i = 0; i < PATTERN_SIZE; i++) {
        fprintf(stdout, "machsuite_kmp next[%d]=%d expected=%d\n", i,
                kmp_next[i], expected_next[i]);
        if (kmp_next[i] != expected_next[i]) {
            fprintf(stdout, "FAIL machsuite_kmp prefix table mismatch\n");
            failures = 1;
            break;
        }
    }

    if (!failures) {
        fprintf(stdout, "PASS machsuite_kmp\n");
    }

    return failures ? 1 : 0;
}
