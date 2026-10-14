/*
 * Host verification driver for the OHLS PolyBench TRMM benchmark.
 *
 * Role in the flow: initialize the unit-lower-triangular PolyBench input,
 * execute kernel/reference implementations, and report deterministic equality
 * results to ohls-cli verification.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_trmm.h"

static const float kTolerance = 1e-3f;

/* Initialize A and B with the original PolyBench TRMM formulas. */
static void init_arrays(float *alpha, float A[TRMM_M][TRMM_M],
                        float B[TRMM_M][TRMM_N]) {
  *alpha = 1.5f;
  for (int i = 0; i < TRMM_M; ++i) {
    for (int j = 0; j < TRMM_M; ++j) {
      A[i][j] = 0.0f;
    }
    for (int j = 0; j < i; ++j) {
      A[i][j] = (float)((i + j) % TRMM_M) / (float)TRMM_M;
    }
    A[i][i] = 1.0f;
    for (int j = 0; j < TRMM_N; ++j) {
      B[i][j] = (float)((TRMM_N + (i - j)) % TRMM_N) / (float)TRMM_N;
    }
  }
}

/* Golden TRMM implementation with the same in-place update order as PolyBench. */
static void reference_trmm(float alpha, const float A[TRMM_M][TRMM_M],
                           float B[TRMM_M][TRMM_N]) {
  for (int i = 0; i < TRMM_M; ++i) {
    for (int j = 0; j < TRMM_N; ++j) {
      for (int k = i + 1; k < TRMM_M; ++k) {
        B[i][j] += A[k][i] * B[k][j];
      }
      B[i][j] = alpha * B[i][j];
    }
  }
}

/* Compute a deterministic signature of the updated B matrix. */
static float checksum_matrix(const float B[TRMM_M][TRMM_N]) {
  float sum = 0.0f;
  for (int i = 0; i < TRMM_M; ++i) {
    for (int j = 0; j < TRMM_N; ++j) {
      sum += B[i][j] * (float)(1 + i + j);
    }
  }
  return sum;
}

/* Compare two B matrices and return the first mismatching coordinates. */
static int compare_matrices(const float lhs[TRMM_M][TRMM_N],
                            const float rhs[TRMM_M][TRMM_N], int *bad_i,
                            int *bad_j) {
  for (int i = 0; i < TRMM_M; ++i) {
    for (int j = 0; j < TRMM_N; ++j) {
      if (fabsf(lhs[i][j] - rhs[i][j]) > kTolerance) {
        *bad_i = i;
        *bad_j = j;
        return 0;
      }
    }
  }
  *bad_i = -1;
  *bad_j = -1;
  return 1;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL polybench_trmm: cannot open output file\n");
      return 2;
    }
  }

  static float A[TRMM_M][TRMM_M];
  static float B_kernel[TRMM_M][TRMM_N];
  static float B_reference[TRMM_M][TRMM_N];
  float alpha = 0.0f;

  init_arrays(&alpha, A, B_kernel);
  init_arrays(&alpha, A, B_reference);
  polybench_trmm(alpha, A, B_kernel);
  reference_trmm(alpha, A, B_reference);

  int bad_i = -1;
  int bad_j = -1;
  int match = compare_matrices(B_kernel, B_reference, &bad_i, &bad_j);
  fprintf(out, "kernel_checksum=%f\n", checksum_matrix(B_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_matrix(B_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_i, bad_j);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
