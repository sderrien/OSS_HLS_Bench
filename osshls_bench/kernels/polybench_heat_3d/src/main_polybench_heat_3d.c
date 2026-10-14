/*
 * Host verification driver for the OHLS PolyBench heat-3d benchmark.
 *
 * Role in the flow: build the PolyBench initial 3-D field, run kernel/reference
 * stencil updates, and report a checksum plus exact tolerance match status.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_heat_3d.h"

static const float kTolerance = 1e-3f;

/* Initialize A and B with the PolyBench heat-3d formula. */
static void init_arrays(float A[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N],
                        float B[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N]) {
  for (int i = 0; i < HEAT_3D_N; ++i) {
    for (int j = 0; j < HEAT_3D_N; ++j) {
      for (int k = 0; k < HEAT_3D_N; ++k) {
        float value = (float)(i + j + (HEAT_3D_N - k)) * 10.0f / (float)HEAT_3D_N;
        A[i][j][k] = value;
        B[i][j][k] = value;
      }
    }
  }
}

/* Golden heat-3d implementation without HLS annotations. */
static void reference_heat_3d(float A[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N],
                              float B[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N]) {
  for (int t = 1; t <= HEAT_3D_TSTEPS; ++t) {
    for (int i = 1; i < HEAT_3D_N - 1; ++i) {
      for (int j = 1; j < HEAT_3D_N - 1; ++j) {
        for (int k = 1; k < HEAT_3D_N - 1; ++k) {
          B[i][j][k] = 0.125f * (A[i + 1][j][k] - 2.0f * A[i][j][k] + A[i - 1][j][k]) +
                       0.125f * (A[i][j + 1][k] - 2.0f * A[i][j][k] + A[i][j - 1][k]) +
                       0.125f * (A[i][j][k + 1] - 2.0f * A[i][j][k] + A[i][j][k - 1]) +
                       A[i][j][k];
        }
      }
    }
    for (int i = 1; i < HEAT_3D_N - 1; ++i) {
      for (int j = 1; j < HEAT_3D_N - 1; ++j) {
        for (int k = 1; k < HEAT_3D_N - 1; ++k) {
          A[i][j][k] = 0.125f * (B[i + 1][j][k] - 2.0f * B[i][j][k] + B[i - 1][j][k]) +
                       0.125f * (B[i][j + 1][k] - 2.0f * B[i][j][k] + B[i][j - 1][k]) +
                       0.125f * (B[i][j][k + 1] - 2.0f * B[i][j][k] + B[i][j][k - 1]) +
                       B[i][j][k];
        }
      }
    }
  }
}

/* Checksum the live-out A array after the final timestep. */
static float checksum_field(const float A[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N]) {
  float sum = 0.0f;
  for (int i = 0; i < HEAT_3D_N; ++i) {
    for (int j = 0; j < HEAT_3D_N; ++j) {
      for (int k = 0; k < HEAT_3D_N; ++k) {
        sum += A[i][j][k] * (float)(1 + i + j + k);
      }
    }
  }
  return sum;
}

/* Compare two 3-D fields and report first mismatching coordinates. */
static int compare_fields(const float lhs[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N],
                          const float rhs[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N],
                          int *bad_i, int *bad_j, int *bad_k) {
  for (int i = 0; i < HEAT_3D_N; ++i) {
    for (int j = 0; j < HEAT_3D_N; ++j) {
      for (int k = 0; k < HEAT_3D_N; ++k) {
        if (fabsf(lhs[i][j][k] - rhs[i][j][k]) > kTolerance) {
          *bad_i = i;
          *bad_j = j;
          *bad_k = k;
          return 0;
        }
      }
    }
  }
  *bad_i = -1;
  *bad_j = -1;
  *bad_k = -1;
  return 1;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL polybench_heat_3d: cannot open output file\n");
      return 2;
    }
  }

  static float A_kernel[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N];
  static float B_kernel[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N];
  static float A_reference[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N];
  static float B_reference[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N];

  init_arrays(A_kernel, B_kernel);
  init_arrays(A_reference, B_reference);
  polybench_heat_3d(A_kernel, B_kernel);
  reference_heat_3d(A_reference, B_reference);

  int bad_i = -1;
  int bad_j = -1;
  int bad_k = -1;
  int match = compare_fields(A_kernel, A_reference, &bad_i, &bad_j, &bad_k);
  fprintf(out, "kernel_checksum=%f\n", checksum_field(A_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_field(A_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d,%d\n", bad_i, bad_j, bad_k);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
