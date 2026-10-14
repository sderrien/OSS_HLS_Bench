#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "matmul.h"

static void init_matrix(int M[N][N], uint32_t seed) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      seed = seed * 1664525u + 1013904223u;
      M[i][j] = (int)((seed >> 8) % 17u) - 8;
    }
  }
}

static void zero_matrix(int M[N][N]) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      M[i][j] = 0;
    }
  }
}

static void matmul_reference(const int A[N][N], const int B[N][N], int C[N][N]) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      int acc = 0;
      for (int k = 0; k < N; ++k) {
        acc += A[i][k] * B[k][j];
      }
      C[i][j] = acc;
    }
  }
}

static uint64_t checksum_matrix(const int M[N][N]) {
  uint64_t acc = 0;
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      acc = (acc * 1315423911ULL) ^ (uint64_t)(M[i][j] + 0x9e3779b9);
    }
  }
  return acc;
}

int main(int argc, char **argv) {
  if (argc > 1) {
    if (freopen(argv[1], "w", stdout) == NULL) {
      perror("freopen");
      return 1;
    }
  }

  FILE *out = stdout;

  static int A[N][N];
  static int B[N][N];
  static int C_kernel[N][N];
  static int C_ref[N][N];

  init_matrix(A, 1u);
  init_matrix(B, 2u);
  zero_matrix(C_kernel);
  zero_matrix(C_ref);

  matmul_unroll_part_x4(A, B, C_kernel);

  matmul_reference(A, B, C_ref);

  int mismatch = memcmp(C_kernel, C_ref, sizeof(C_ref));
  uint64_t ksum = checksum_matrix(C_kernel);
  uint64_t rsum = checksum_matrix(C_ref);
  
  fprintf(out, "kernel_checksum=%llu\n", (unsigned long long)ksum);
  fprintf(out, "reference_checksum=%llu\n", (unsigned long long)rsum);
  fprintf(out, "match=%s\n", mismatch == 0 ? "yes" : "no");

  return mismatch == 0 ? 0 : 2;
}
