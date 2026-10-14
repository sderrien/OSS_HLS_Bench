/*
 * HLS top for the PolyBench/C ATAX benchmark.
 *
 * Why: ATAX stresses two matrix-vector passes with a carried accumulation into
 * the output vector. What: the kernel computes y = A^T * (A * x). How: the
 * implementation preserves the PolyBench loop ordering and adds innermost-loop
 * pipeline directives for OHLS scheduling experiments.
 *
 * Adapted from PolyBench/C 4.2.1. Original copyright: 2010-2016, Ohio State
 * University. Distributed under the Ohio State University Software Distribution
 * License bundled with PolyBench/C.
 */
#include "polybench_atax.h"

__attribute__((annotate("hls.toplevel")))
void polybench_atax(const float A[ATAX_M][ATAX_N], const float x[ATAX_N],
                    float y[ATAX_N], float tmp[ATAX_M]) {
  #pragma HLS pipeline
  for (int i = 0; i < ATAX_N; ++i) {
    y[i] = 0.0f;
  }

  for (int i = 0; i < ATAX_M; ++i) {
    tmp[i] = 0.0f;
#pragma HLS pipeline
    for (int j = 0; j < ATAX_N; ++j) {
      tmp[i] += A[i][j] * x[j];
    }
#pragma HLS pipeline
    for (int j = 0; j < ATAX_N; ++j) {
      y[j] += A[i][j] * tmp[i];
    }
  }
}
