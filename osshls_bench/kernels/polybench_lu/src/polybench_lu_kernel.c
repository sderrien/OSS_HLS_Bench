/*
 * HLS top for the PolyBench/C LU benchmark.
 *
 * Why: LU stresses triangular loop bounds and in-place dependencies without the
 * square-root used by Cholesky. What: it factors A using the PolyBench no-pivot
 * recurrence. How: innermost update loops are annotated for pipelining.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include "polybench_lu.h"

__attribute__((annotate("hls.toplevel")))
void polybench_lu(float A[LU_N][LU_N]) {
  for (int i = 0; i < LU_N; ++i) {
    for (int j = 0; j < i; ++j) {
#pragma HLS pipeline
      for (int k = 0; k < j; ++k) {
        A[i][j] -= A[i][k] * A[k][j];
      }
      A[i][j] /= A[j][j];
    }
    for (int j = i; j < LU_N; ++j) {
#pragma HLS pipeline
      for (int k = 0; k < i; ++k) {
        A[i][j] -= A[i][k] * A[k][j];
      }
    }
  }
}
