/*
 * Host verification driver for the OHLS PolyBench ATAX benchmark.
 *
 * Role in the flow: initialize deterministic PolyBench-style inputs, run the
 * annotated kernel, run an unannotated reference, and report checksum/match
 * status for ohls-cli verification targets.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_atax.h"

static const float kTolerance = 1e-3f;

/* Initialize A and x with the formulas used by PolyBench/C ATAX. */
static void init_arrays(float A[ATAX_M][ATAX_N], float x[ATAX_N]) {
  const float fn = (float)ATAX_N;
  for (int i = 0; i < ATAX_N; ++i) {
    x[i] = 1.0f + ((float)i / fn);
  }
  for (int i = 0; i < ATAX_M; ++i) {
    for (int j = 0; j < ATAX_N; ++j) {
      A[i][j] = (float)((i + j) % ATAX_N) / (float)(5 * ATAX_M);
    }
  }
}

/* Golden ATAX implementation without HLS annotations. */
static void reference_atax(const float A[ATAX_M][ATAX_N], const float x[ATAX_N],
                           float y[ATAX_N], float tmp[ATAX_M]) {
  for (int i = 0; i < ATAX_N; ++i) {
    y[i] = 0.0f;
  }
  for (int i = 0; i < ATAX_M; ++i) {
    tmp[i] = 0.0f;
    for (int j = 0; j < ATAX_N; ++j) {
      tmp[i] += A[i][j] * x[j];
    }
    for (int j = 0; j < ATAX_N; ++j) {
      y[j] += A[i][j] * tmp[i];
    }
  }
}

/* Produce a compact floating-point signature for human-readable output. */
static float checksum_vector(const float values[ATAX_N]) {
  float sum = 0.0f;
  for (int i = 0; i < ATAX_N; ++i) {
    sum += values[i] * (float)(i + 1);
  }
  return sum;
}

/* Compare full output vectors and return the first mismatching index. */
static int compare_vectors(const float lhs[ATAX_N], const float rhs[ATAX_N],
                           int *bad_index) {
  for (int i = 0; i < ATAX_N; ++i) {
    if (fabsf(lhs[i] - rhs[i]) > kTolerance) {
      *bad_index = i;
      return 0;
    }
  }
  *bad_index = -1;
  return 1;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL polybench_atax: cannot open output file\n");
      return 2;
    }
  }

  static float A[ATAX_M][ATAX_N];
  static float x[ATAX_N];
  static float y_kernel[ATAX_N];
  static float y_reference[ATAX_N];
  static float tmp_kernel[ATAX_M];
  static float tmp_reference[ATAX_M];

  init_arrays(A, x);
  polybench_atax(A, x, y_kernel, tmp_kernel);
  reference_atax(A, x, y_reference, tmp_reference);

  int bad_index = -1;
  int match = compare_vectors(y_kernel, y_reference, &bad_index);
  fprintf(out, "kernel_checksum=%f\n", checksum_vector(y_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_vector(y_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d\n", bad_index);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
