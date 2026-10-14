/*
 * Host verification driver for the OHLS PolyBench Cholesky benchmark.
 *
 * Role in the flow: construct the PolyBench positive semidefinite input matrix,
 * run kernel/reference factorizations, and report deterministic match status.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_cholesky.h"

static const float kTolerance = 1e-3f;

/* Initialize A with the PolyBench Cholesky positive semidefinite construction. */
static void init_matrix(float A[CHOLESKY_N][CHOLESKY_N]) {
  static float B[CHOLESKY_N][CHOLESKY_N];
  for (int i = 0; i < CHOLESKY_N; ++i) {
    for (int j = 0; j <= i; ++j) {
      A[i][j] = (float)(-j % CHOLESKY_N) / (float)CHOLESKY_N + 1.0f;
    }
    for (int j = i + 1; j < CHOLESKY_N; ++j) {
      A[i][j] = 0.0f;
    }
    A[i][i] = 1.0f;
  }
  for (int r = 0; r < CHOLESKY_N; ++r) {
    for (int s = 0; s < CHOLESKY_N; ++s) {
      B[r][s] = 0.0f;
    }
  }
  for (int t = 0; t < CHOLESKY_N; ++t) {
    for (int r = 0; r < CHOLESKY_N; ++r) {
      for (int s = 0; s < CHOLESKY_N; ++s) {
        B[r][s] += A[r][t] * A[s][t];
      }
    }
  }
  for (int r = 0; r < CHOLESKY_N; ++r) {
    for (int s = 0; s < CHOLESKY_N; ++s) {
      A[r][s] = B[r][s];
    }
  }
}

/* Golden Cholesky implementation without HLS annotations. */
static void reference_cholesky(float A[CHOLESKY_N][CHOLESKY_N]) {
  for (int i = 0; i < CHOLESKY_N; ++i) {
    for (int j = 0; j < i; ++j) {
      for (int k = 0; k < j; ++k) {
        A[i][j] -= A[i][k] * A[j][k];
      }
      A[i][j] /= A[j][j];
    }
    for (int k = 0; k < i; ++k) {
      A[i][i] -= A[i][k] * A[i][k];
    }
    A[i][i] = sqrtf(A[i][i]);
  }
}

/* Checksum the lower triangle that Cholesky updates. */
static float checksum_lower(const float A[CHOLESKY_N][CHOLESKY_N]) {
  float sum = 0.0f;
  for (int i = 0; i < CHOLESKY_N; ++i) {
    for (int j = 0; j <= i; ++j) {
      sum += A[i][j] * (float)(1 + i + j);
    }
  }
  return sum;
}

/* Compare lower triangles and return first mismatching coordinates. */
static int compare_lower(const float lhs[CHOLESKY_N][CHOLESKY_N],
                         const float rhs[CHOLESKY_N][CHOLESKY_N], int *bad_i,
                         int *bad_j) {
  for (int i = 0; i < CHOLESKY_N; ++i) {
    for (int j = 0; j <= i; ++j) {
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
      fprintf(stderr, "FAIL polybench_cholesky: cannot open output file\n");
      return 2;
    }
  }

  static float A_kernel[CHOLESKY_N][CHOLESKY_N];
  static float A_reference[CHOLESKY_N][CHOLESKY_N];
  init_matrix(A_kernel);
  init_matrix(A_reference);
  polybench_cholesky(A_kernel);
  reference_cholesky(A_reference);

  int bad_i = -1;
  int bad_j = -1;
  int match = compare_lower(A_kernel, A_reference, &bad_i, &bad_j);
  fprintf(out, "kernel_checksum=%f\n", checksum_lower(A_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_lower(A_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_i, bad_j);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
