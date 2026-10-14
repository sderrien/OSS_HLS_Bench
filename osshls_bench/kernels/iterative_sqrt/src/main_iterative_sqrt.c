/*
 * Iterative square-root benchmark testbench.
 *
 * Role in the benchmark flow:
 * Run the scalar iterative_sqrt kernel on deterministic inputs, compare each
 * result against a small software reference, and write compact output suitable
 * for OpenHLS/VibeHLS golden-vs-RTL diffing.
 *
 * How it works:
 * The testbench exercises exact-square and non-square values.  The output file
 * records the final checksum and mismatch count rather than every individual
 * sample, keeping logs small while preserving functional checking.
 */

#include <stdio.h>

#include "iterative_sqrt.h"

/*
 * Compute the integer floor square root used as the software reference.
 *
 * Role:
 * Provide an independent expected value for each scalar input.
 *
 * Goal:
 * Detect functional errors in the HLS kernel without depending on libm or
 * floating-point behavior.
 */
static int reference_sqrt(in_int_t n) {
  int low = 0;
  int high = n;
  int answer = 0;
  while (low <= high) {
    int mid = (low + high) >> 1;
    int square = mid * mid;
    if (square <= n) {
      answer = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return answer;
}

/*
 * Run the kernel on deterministic scalar test cases and summarize the results.
 *
 * Role:
 * Act as the executable C/RTL testbench for benchmark runners that use the
 * benchmark-local main file.
 *
 * Goal:
 * Return non-zero on functional mismatch and support argv[1] redirection for
 * OpenHLS/VibeHLS output comparison.
 */
int main(int argc, char **argv) {
  static const in_int_t samples[] = {0, 1, 2, 3, 4, 15, 16, 17, 255, 256, 257, 1023, 1024, 4095};
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "failed to open output file\n");
      return 1;
    }
  }

  int mismatches = 0;
  unsigned long long checksum = 1469598103934665603ull;
  for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    int got = iterative_sqrt(samples[i]);
    int expected = reference_sqrt(samples[i]);
    if (got != expected) {
      ++mismatches;
    }
    checksum ^= (unsigned long long)(unsigned int)got;
    checksum *= 1099511628211ull;
    checksum ^= (unsigned long long)(unsigned int)expected;
    checksum *= 1099511628211ull;
  }

  fprintf(out, "checksum=%llu\n", checksum);
  fprintf(out, "mismatches=%d\n", mismatches);
  fprintf(out, "match=%s\n", mismatches == 0 ? "yes" : "no");

  if (out != stdout) {
    fclose(out);
  }
  return mismatches == 0 ? 0 : 2;
}
