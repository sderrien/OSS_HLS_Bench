/*
 * Host verification driver for the OHLS PolyBench LU benchmark.
 *
 * Role in the flow: construct the PolyBench LU input, execute kernel/reference
 * no-pivot factorizations, and emit deterministic checksums and match status.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_lu.h"

static const float kTolerance = 1e-3f;

/* Initialize A with the same positive semidefinite construction used upstream. */
static void init_matrix(float A[LU_N][LU_N]) {
  static float B[LU_N][LU_N];
  for (int i = 0; i < LU_N; ++i) {
    for (int j = 0; j <= i; ++j) {
      A[i][j] = (float)(-j % LU_N) / (float)LU_N + 1.0f;
    }
    for (int j = i + 1; j < LU_N; ++j) {
      A[i][j] = 0.0f;
    }
    A[i][i] = 1.0f;
  }
  for (int r = 0; r < LU_N; ++r) {
    for (int s = 0; s < LU_N; ++s) {
      B[r][s] = 0.0f;
    }
  }
  for (int t = 0; t < LU_N; ++t) {
    for (int r = 0; r < LU_N; ++r) {
      for (int s = 0; s < LU_N; ++s) {
        B[r][s] += A[r][t] * A[s][t];
      }
    }
  }
  for (int r = 0; r < LU_N; ++r) {
    for (int s = 0; s < LU_N; ++s) {
      A[r][s] = B[r][s];
    }
  }
}

/* Golden LU implementation without HLS annotations. */
static void reference_lu(float A[LU_N][LU_N]) {
  for (int i = 0; i < LU_N; ++i) {
    for (int j = 0; j < i; ++j) {
      for (int k = 0; k < j; ++k) {
        A[i][j] -= A[i][k] * A[k][j];
      }
      A[i][j] /= A[j][j];
    }
    for (int j = i; j < LU_N; ++j) {
      for (int k = 0; k < i; ++k) {
        A[i][j] -= A[i][k] * A[k][j];
      }
    }
  }
}

/* Compute a deterministic signature of the full factored matrix. */
static float checksum_matrix(const float A[LU_N][LU_N]) {
  float sum = 0.0f;
  for (int i = 0; i < LU_N; ++i) {
    for (int j = 0; j < LU_N; ++j) {
      sum += A[i][j] * (float)(1 + i + j);
    }
  }
  return sum;
}

/* Compare full matrices and return first mismatching coordinates. */
static int compare_matrices(const float lhs[LU_N][LU_N],
                            const float rhs[LU_N][LU_N], int *bad_i,
                            int *bad_j) {
  for (int i = 0; i < LU_N; ++i) {
    for (int j = 0; j < LU_N; ++j) {
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
      fprintf(stderr, "FAIL polybench_lu: cannot open output file\n");
      return 2;
    }
  }

  static float A_kernel[LU_N][LU_N];
  static float A_reference[LU_N][LU_N];
  init_matrix(A_kernel);
  init_matrix(A_reference);
  polybench_lu(A_kernel);
  reference_lu(A_reference);

  int bad_i = -1;
  int bad_j = -1;
  int match = compare_matrices(A_kernel, A_reference, &bad_i, &bad_j);
  fprintf(out, "kernel_checksum=%f\n", checksum_matrix(A_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_matrix(A_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_i, bad_j);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
