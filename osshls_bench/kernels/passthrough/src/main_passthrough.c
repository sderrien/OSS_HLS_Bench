/*
 * Passthrough benchmark testbench.
 *
 * Role in the benchmark flow:
 * Initialize deterministic input data, call the passthrough kernel, compare the
 * copied output against a software reference, and write a compact textual result
 * for OpenHLS/VibeHLS golden-vs-RTL output comparison.
 *
 * How it works:
 * The input sequence uses the same linear congruential generator pattern as the
 * existing small integer benchmarks.  The output is summarized by FNV-1a byte
 * checksums and a mismatch count so functional failures are visible without
 * printing all 1024 elements.
 */

#include <stdint.h>
#include <stdio.h>

#include "passthrough.h"

/*
 * Initialize the kernel input array with deterministic integer samples.
 *
 * Role:
 * Produce a non-trivial but reproducible 1024-sample data set for native C,
 * cycle simulation, RTL simulation, and synthesis-driven testbench generation.
 *
 * Goal:
 * Avoid all-zero inputs while keeping every tool on identical data.
 */
static void init_data(int data[PASSTHROUGH_N], uint32_t seed) {
  for (int i = 0; i < PASSTHROUGH_N; ++i) {
    seed = seed * 1664525u + 1013904223u;
    data[i] = (int)((seed >> 8) % 2048u) - 1024;
  }
}

/*
 * Compute a byte-level checksum over one integer array.
 *
 * Role:
 * Compactly summarize all 1024 output samples in the textual testbench report.
 *
 * Goal:
 * Let OpenHLS/VibeHLS and other runners compare functional output without dumping
 * every element, while still detecting bit-level copy errors.
 */
static unsigned long long checksum_bytes(const int data[PASSTHROUGH_N]) {
  const unsigned char *bytes = (const unsigned char *)data;
  unsigned long long checksum = 1469598103934665603ull;
  for (unsigned long long i = 0; i < sizeof(int) * (unsigned long long)PASSTHROUGH_N; ++i) {
    checksum ^= (unsigned long long)bytes[i];
    checksum *= 1099511628211ull;
  }
  return checksum;
}

/*
 * Run the passthrough kernel and compare its output against the input array.
 *
 * Role:
 * Act as the executable software/RTL testbench for HLS tools that compile the
 * benchmark's C main function.
 *
 * Goal:
 * Return a non-zero status on functional mismatch and write deterministic output
 * lines suitable for VibeHLS golden-vs-RTL diffing.
 */
int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "failed to open output file\n");
      return 1;
    }
  }

  static int data_in[PASSTHROUGH_N];
  static int data_out[PASSTHROUGH_N];

  init_data(data_in, 7u);
  for (int i = 0; i < PASSTHROUGH_N; ++i) {
    data_out[i] = 0;
  }

  passthrough(data_in, data_out);

  int mismatches = 0;
  for (int i = 0; i < PASSTHROUGH_N; ++i) {
    if (data_out[i] != data_in[i]) {
      ++mismatches;
    }
  }

  fprintf(out, "input_checksum=%llu\n", checksum_bytes(data_in));
  fprintf(out, "output_checksum=%llu\n", checksum_bytes(data_out));
  fprintf(out, "mismatches=%d\n", mismatches);
  fprintf(out, "match=%s\n", mismatches == 0 ? "yes" : "no");

  if (out != stdout) {
    fclose(out);
  }
  return mismatches == 0 ? 0 : 2;
}
