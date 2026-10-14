/*
 * HLS top for the PolyBench/C heat-3d benchmark.
 *
 * Why: heat-3d stresses regular 3-D stencil memory traffic and repeated
 * producer/consumer array phases. What: each timestep computes B from A and
 * then A from B. How: the PolyBench loop nest is retained with the innermost
 * spatial loop pipelined.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include "polybench_heat_3d.h"

__attribute__((annotate("hls.toplevel")))
void polybench_heat_3d(float A[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N],
                       float B[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N]) {
  for (int t = 1; t <= HEAT_3D_TSTEPS; ++t) {
    for (int i = 1; i < HEAT_3D_N - 1; ++i) {
      for (int j = 1; j < HEAT_3D_N - 1; ++j) {
#pragma HLS pipeline
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
#pragma HLS pipeline
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
