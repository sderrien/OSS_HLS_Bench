/*
 * Host verification driver for the OHLS PolyBench MVT benchmark.
 *
 * Role in the flow: prepare PolyBench-style vectors/matrix, compare the HLS top
 * result against an unannotated C reference, and emit deterministic status.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_mvt.h"

static const float kTolerance = 1e-3f;

/* Initialize all MVT arrays using the original PolyBench formulas. */
static void init_arrays(float x1[MVT_N], float x2[MVT_N], float y1[MVT_N],
                        float y2[MVT_N], float A[MVT_N][MVT_N]) {
  for (int i = 0; i < MVT_N; ++i) {
    x1[i] = (float)(i % MVT_N) / (float)MVT_N;
    x2[i] = (float)((i + 1) % MVT_N) / (float)MVT_N;
    y1[i] = (float)((i + 3) % MVT_N) / (float)MVT_N;
    y2[i] = (float)((i + 4) % MVT_N) / (float)MVT_N;
    for (int j = 0; j < MVT_N; ++j) {
      A[i][j] = (float)((i * j) % MVT_N) / (float)MVT_N;
    }
  }
}

/* Golden MVT implementation with the same accumulation order as PolyBench. */
static void reference_mvt(float x1[MVT_N], float x2[MVT_N],
                          const float y1[MVT_N], const float y2[MVT_N],
                          const float A[MVT_N][MVT_N]) {
  for (int i = 0; i < MVT_N; ++i) {
    for (int j = 0; j < MVT_N; ++j) {
      x1[i] += A[i][j] * y1[j];
    }
  }
  for (int i = 0; i < MVT_N; ++i) {
    for (int j = 0; j < MVT_N; ++j) {
      x2[i] += A[j][i] * y2[j];
    }
  }
}

/* Combine both live-out vectors into one checksum. */
static float checksum_outputs(const float x1[MVT_N], const float x2[MVT_N]) {
  float sum = 0.0f;
  for (int i = 0; i < MVT_N; ++i) {
    sum += x1[i] * (float)(i + 1) + x2[i] * (float)(MVT_N - i);
  }
  return sum;
}

/* Compare both output vectors and record which vector/index first differs. */
static int compare_outputs(const float lhs1[MVT_N], const float lhs2[MVT_N],
                           const float rhs1[MVT_N], const float rhs2[MVT_N],
                           int *which, int *index) {
  for (int i = 0; i < MVT_N; ++i) {
    if (fabsf(lhs1[i] - rhs1[i]) > kTolerance) {
      *which = 1;
      *index = i;
      return 0;
    }
    if (fabsf(lhs2[i] - rhs2[i]) > kTolerance) {
      *which = 2;
      *index = i;
      return 0;
    }
  }
  *which = 0;
  *index = -1;
  return 1;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL polybench_mvt: cannot open output file\n");
      return 2;
    }
  }

  static float A[MVT_N][MVT_N];
  static float y1[MVT_N];
  static float y2[MVT_N];
  static float x1_kernel[MVT_N];
  static float x2_kernel[MVT_N];
  static float x1_reference[MVT_N];
  static float x2_reference[MVT_N];

  init_arrays(x1_kernel, x2_kernel, y1, y2, A);
  init_arrays(x1_reference, x2_reference, y1, y2, A);
  polybench_mvt(x1_kernel, x2_kernel, y1, y2, A);
  reference_mvt(x1_reference, x2_reference, y1, y2, A);

  int which = 0;
  int index = -1;
  int match = compare_outputs(x1_kernel, x2_kernel, x1_reference, x2_reference,
                              &which, &index);
  fprintf(out, "kernel_checksum=%f\n", checksum_outputs(x1_kernel, x2_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_outputs(x1_reference, x2_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=x%d[%d]\n", which, index);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
