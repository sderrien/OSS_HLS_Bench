/*
 * OHLS benchmark adapter for PolyBench/C TRMM.
 *
 * Role in the flow: define a static 64x64 triangular matrix multiply workload
 * and expose the HLS top function adapted from PolyBench/C 4.2.1
 * linear-algebra/blas/trmm.
 */
#ifndef POLYBENCH_TRMM_H
#define POLYBENCH_TRMM_H

#define TRMM_M 64
#define TRMM_N 64

#ifdef __cplusplus
extern "C" {
#endif

/* Form B := alpha * A^T * B for unit-lower-triangular A. */
void polybench_trmm(float alpha, const float A[TRMM_M][TRMM_M],
                    float B[TRMM_M][TRMM_N]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_TRMM_H */
