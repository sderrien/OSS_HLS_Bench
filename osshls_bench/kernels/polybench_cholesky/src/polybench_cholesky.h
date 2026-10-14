/*
 * OHLS benchmark adapter for PolyBench/C cholesky.
 *
 * Role in the flow: define a static 32x32 Cholesky factorization workload and
 * declare the HLS top function adapted from PolyBench/C 4.2.1 solvers.
 */
#ifndef POLYBENCH_CHOLESKY_H
#define POLYBENCH_CHOLESKY_H

#define CHOLESKY_N 32

#ifdef __cplusplus
extern "C" {
#endif

/* Factor the positive semidefinite matrix A in place using Cholesky. */
void polybench_cholesky(float A[CHOLESKY_N][CHOLESKY_N]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_CHOLESKY_H */
