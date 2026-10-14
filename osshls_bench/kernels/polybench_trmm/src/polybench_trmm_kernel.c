/*
 * HLS top for the PolyBench/C TRMM benchmark.
 *
 * Why: TRMM stresses triangular data dependencies and in-place matrix updates.
 * What: it computes B := alpha * A^T * B for a unit lower triangular A. How:
 * the loop order follows PolyBench and pipelines the reduction over k.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include "polybench_trmm.h"

__attribute__((annotate("hls.toplevel")))
void polybench_trmm(float alpha, const float A[TRMM_M][TRMM_M],
                    float B[TRMM_M][TRMM_N]) {
  for (int i = 0; i < TRMM_M; ++i) {
    for (int j = 0; j < TRMM_N; ++j) {
#pragma HLS pipeline
      for (int k = i + 1; k < TRMM_M; ++k) {
        B[i][j] += A[k][i] * B[k][j];
      }
      B[i][j] = alpha * B[i][j];
    }
  }
}
