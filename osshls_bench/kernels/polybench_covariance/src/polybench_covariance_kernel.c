/*
 * HLS top for the PolyBench/C covariance benchmark.
 *
 * Why: covariance combines column reductions, in-place data centering, and a
 * symmetric matrix update. What: it computes cov = centered(data)^T*data/(N-1).
 * How: the PolyBench loop ordering is retained with pipelined innermost loops.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include "polybench_covariance.h"

__attribute__((annotate("hls.toplevel")))
void polybench_covariance(float data[COVARIANCE_N][COVARIANCE_M],
                          float cov[COVARIANCE_M][COVARIANCE_M],
                          float mean[COVARIANCE_M]) {
  const float float_n = (float)COVARIANCE_N;

  for (int j = 0; j < COVARIANCE_M; ++j) {
    mean[j] = 0.0f;
#pragma HLS pipeline
    for (int i = 0; i < COVARIANCE_N; ++i) {
      mean[j] += data[i][j];
    }
    mean[j] /= float_n;
  }

  for (int i = 0; i < COVARIANCE_N; ++i) {
#pragma HLS pipeline
    for (int j = 0; j < COVARIANCE_M; ++j) {
      data[i][j] -= mean[j];
    }
  }

  for (int i = 0; i < COVARIANCE_M; ++i) {
    for (int j = i; j < COVARIANCE_M; ++j) {
      cov[i][j] = 0.0f;
#pragma HLS pipeline
      for (int k = 0; k < COVARIANCE_N; ++k) {
        cov[i][j] += data[k][i] * data[k][j];
      }
      cov[i][j] /= (float_n - 1.0f);
      cov[j][i] = cov[i][j];
    }
  }
}
