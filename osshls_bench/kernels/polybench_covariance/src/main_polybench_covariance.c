/*
 * Host verification driver for the OHLS PolyBench covariance benchmark.
 *
 * Role in the flow: initialize the datamining input matrix, run kernel and
 * reference covariance computations, and emit deterministic checksum/match
 * status for benchmark verification.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_covariance.h"

static const float kTolerance = 1e-3f;

/* Initialize the input data matrix with the PolyBench covariance formula. */
static void init_data(float data[COVARIANCE_N][COVARIANCE_M]) {
  for (int i = 0; i < COVARIANCE_N; ++i) {
    for (int j = 0; j < COVARIANCE_M; ++j) {
      data[i][j] = ((float)(i * j)) / (float)COVARIANCE_M;
    }
  }
}

/* Golden covariance implementation without HLS annotations. */
static void reference_covariance(float data[COVARIANCE_N][COVARIANCE_M],
                                 float cov[COVARIANCE_M][COVARIANCE_M],
                                 float mean[COVARIANCE_M]) {
  const float float_n = (float)COVARIANCE_N;
  for (int j = 0; j < COVARIANCE_M; ++j) {
    mean[j] = 0.0f;
    for (int i = 0; i < COVARIANCE_N; ++i) {
      mean[j] += data[i][j];
    }
    mean[j] /= float_n;
  }
  for (int i = 0; i < COVARIANCE_N; ++i) {
    for (int j = 0; j < COVARIANCE_M; ++j) {
      data[i][j] -= mean[j];
    }
  }
  for (int i = 0; i < COVARIANCE_M; ++i) {
    for (int j = i; j < COVARIANCE_M; ++j) {
      cov[i][j] = 0.0f;
      for (int k = 0; k < COVARIANCE_N; ++k) {
        cov[i][j] += data[k][i] * data[k][j];
      }
      cov[i][j] /= (float_n - 1.0f);
      cov[j][i] = cov[i][j];
    }
  }
}

/* Compute a compact checksum for the symmetric covariance matrix. */
static float checksum_matrix(const float cov[COVARIANCE_M][COVARIANCE_M]) {
  float sum = 0.0f;
  for (int i = 0; i < COVARIANCE_M; ++i) {
    for (int j = 0; j < COVARIANCE_M; ++j) {
      sum += cov[i][j] * (float)(1 + i + j);
    }
  }
  return sum;
}

/* Compare covariance matrices and report first mismatching coordinates. */
static int compare_matrices(const float lhs[COVARIANCE_M][COVARIANCE_M],
                            const float rhs[COVARIANCE_M][COVARIANCE_M],
                            int *bad_i, int *bad_j) {
  for (int i = 0; i < COVARIANCE_M; ++i) {
    for (int j = 0; j < COVARIANCE_M; ++j) {
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
      fprintf(stderr, "FAIL polybench_covariance: cannot open output file\n");
      return 2;
    }
  }

  static float data_kernel[COVARIANCE_N][COVARIANCE_M];
  static float data_reference[COVARIANCE_N][COVARIANCE_M];
  static float cov_kernel[COVARIANCE_M][COVARIANCE_M];
  static float cov_reference[COVARIANCE_M][COVARIANCE_M];
  static float mean_kernel[COVARIANCE_M];
  static float mean_reference[COVARIANCE_M];

  init_data(data_kernel);
  init_data(data_reference);
  polybench_covariance(data_kernel, cov_kernel, mean_kernel);
  reference_covariance(data_reference, cov_reference, mean_reference);

  int bad_i = -1;
  int bad_j = -1;
  int match = compare_matrices(cov_kernel, cov_reference, &bad_i, &bad_j);
  fprintf(out, "kernel_checksum=%f\n", checksum_matrix(cov_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_matrix(cov_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_i, bad_j);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
