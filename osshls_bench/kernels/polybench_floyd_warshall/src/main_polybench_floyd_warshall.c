/*
 * Host verification driver for the OHLS PolyBench Floyd-Warshall benchmark.
 *
 * Role in the flow: initialize the integer path matrix, execute kernel and
 * reference shortest-path updates, and report exact match status.
 */
#include <stdio.h>

#include "polybench_floyd_warshall.h"

/* Initialize the path matrix with the original PolyBench formula. */
static void init_path(int path[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N]) {
  for (int i = 0; i < FLOYD_WARSHALL_N; ++i) {
    for (int j = 0; j < FLOYD_WARSHALL_N; ++j) {
      path[i][j] = (i * j) % 7 + 1;
      if ((i + j) % 13 == 0 || (i + j) % 7 == 0 || (i + j) % 11 == 0) {
        path[i][j] = 999;
      }
    }
  }
}

/* Golden Floyd-Warshall implementation without HLS annotations. */
static void reference_floyd_warshall(int path[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N]) {
  for (int k = 0; k < FLOYD_WARSHALL_N; ++k) {
    for (int i = 0; i < FLOYD_WARSHALL_N; ++i) {
      for (int j = 0; j < FLOYD_WARSHALL_N; ++j) {
        int through_k = path[i][k] + path[k][j];
        path[i][j] = path[i][j] < through_k ? path[i][j] : through_k;
      }
    }
  }
}

/* Compute a stable integer checksum over the final shortest-path matrix. */
static unsigned long long checksum_path(const int path[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N]) {
  unsigned long long sum = 1469598103934665603ULL;
  for (int i = 0; i < FLOYD_WARSHALL_N; ++i) {
    for (int j = 0; j < FLOYD_WARSHALL_N; ++j) {
      sum ^= (unsigned long long)(path[i][j] + 31 * i + j);
      sum *= 1099511628211ULL;
    }
  }
  return sum;
}

/* Compare two path matrices and return the first mismatching coordinates. */
static int compare_paths(const int lhs[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N],
                         const int rhs[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N],
                         int *bad_i, int *bad_j) {
  for (int i = 0; i < FLOYD_WARSHALL_N; ++i) {
    for (int j = 0; j < FLOYD_WARSHALL_N; ++j) {
      if (lhs[i][j] != rhs[i][j]) {
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
      fprintf(stderr, "FAIL polybench_floyd_warshall: cannot open output file\n");
      return 2;
    }
  }

  static int path_kernel[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N];
  static int path_reference[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N];

  init_path(path_kernel);
  init_path(path_reference);
  polybench_floyd_warshall(path_kernel);
  reference_floyd_warshall(path_reference);

  int bad_i = -1;
  int bad_j = -1;
  int match = compare_paths(path_kernel, path_reference, &bad_i, &bad_j);
  fprintf(out, "kernel_checksum=%llu\n", checksum_path(path_kernel));
  fprintf(out, "reference_checksum=%llu\n", checksum_path(path_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_i, bad_j);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
