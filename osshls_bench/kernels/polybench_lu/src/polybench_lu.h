/*
 * OHLS benchmark adapter for PolyBench/C LU.
 *
 * Role in the flow: define a static 32x32 in-place LU decomposition workload
 * and declare the HLS top function adapted from PolyBench/C 4.2.1 solvers/lu.
 */
#ifndef POLYBENCH_LU_H
#define POLYBENCH_LU_H

#define LU_N 32

#ifdef __cplusplus
extern "C" {
#endif

/* Factor A in place using the PolyBench LU loop nest. */
void polybench_lu(float A[LU_N][LU_N]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_LU_H */
