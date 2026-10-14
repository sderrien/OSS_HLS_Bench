/*
 * OHLS benchmark adapter for PolyBench/C covariance.
 *
 * Role in the flow: define a static 64-sample by 32-feature datamining kernel
 * and declare the HLS top function adapted from PolyBench/C 4.2.1
 * datamining/covariance.
 */
#ifndef POLYBENCH_COVARIANCE_H
#define POLYBENCH_COVARIANCE_H

#define COVARIANCE_N 64
#define COVARIANCE_M 32

#ifdef __cplusplus
extern "C" {
#endif

/* Center the input columns and compute the symmetric covariance matrix. */
void polybench_covariance(float data[COVARIANCE_N][COVARIANCE_M],
                          float cov[COVARIANCE_M][COVARIANCE_M],
                          float mean[COVARIANCE_M]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_COVARIANCE_H */
