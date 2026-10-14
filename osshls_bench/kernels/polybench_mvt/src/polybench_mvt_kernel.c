/*
 * HLS top for the PolyBench/C MVT benchmark.
 *
 * Why: MVT provides two independent matrix-vector update patterns over the same
 * matrix. What: x1 accumulates rows of A while x2 accumulates columns of A.
 * How: the PolyBench loop order is retained with pipelined innermost loops.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include "polybench_mvt.h"

__attribute__((annotate("hls.toplevel")))
void polybench_mvt(float x1[MVT_N], float x2[MVT_N], const float y1[MVT_N],
                   const float y2[MVT_N], const float A[MVT_N][MVT_N]) {
  for (int i = 0; i < MVT_N; ++i) {
#pragma HLS pipeline
    for (int j = 0; j < MVT_N; ++j) {
      x1[i] += A[i][j] * y1[j];
    }
  }
  for (int i = 0; i < MVT_N; ++i) {
#pragma HLS pipeline
    for (int j = 0; j < MVT_N; ++j) {
      x2[i] += A[j][i] * y2[j];
    }
  }
}
