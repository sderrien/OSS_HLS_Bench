/*
 * HLS top for the PolyBench/C Cholesky benchmark.
 *
 * Why: Cholesky stresses triangular loop bounds, loop-carried updates, and a
 * square-root operation. What: it factors A in place into a lower-triangular
 * Cholesky representation. How: the original PolyBench recurrence is retained
 * with the innermost accumulation loops pipelined.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include <math.h>

#include "polybench_cholesky.h"

__attribute__((annotate("hls.toplevel")))
void polybench_cholesky(float A[CHOLESKY_N][CHOLESKY_N]) {
  for (int i = 0; i < CHOLESKY_N; ++i) {
    for (int j = 0; j < i; ++j) {
#pragma HLS pipeline
      for (int k = 0; k < j; ++k) {
        A[i][j] -= A[i][k] * A[j][k];
      }
      A[i][j] /= A[j][j];
    }
#pragma HLS pipeline
    for (int k = 0; k < i; ++k) {
      A[i][i] -= A[i][k] * A[i][k];
    }
    A[i][i] = sqrtf(A[i][i]);
  }
}
