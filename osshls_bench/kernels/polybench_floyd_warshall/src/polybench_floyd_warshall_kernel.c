/*
 * HLS top for the PolyBench/C Floyd-Warshall benchmark.
 *
 * Why: Floyd-Warshall stresses loop-carried updates through a shared matrix.
 * What: path[i][j] is updated with min(path[i][j], path[i][k]+path[k][j]).
 * How: the original k/i/j loop order is preserved and the innermost j loop is
 * pipelined for scheduler coverage.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include "polybench_floyd_warshall.h"

__attribute__((annotate("hls.toplevel")))
void polybench_floyd_warshall(int path[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N]) {
  for (int k = 0; k < FLOYD_WARSHALL_N; ++k) {
    for (int i = 0; i < FLOYD_WARSHALL_N; ++i) {
#pragma HLS pipeline
      for (int j = 0; j < FLOYD_WARSHALL_N; ++j) {
        int through_k = path[i][k] + path[k][j];
        path[i][j] = path[i][j] < through_k ? path[i][j] : through_k;
      }
    }
  }
}
