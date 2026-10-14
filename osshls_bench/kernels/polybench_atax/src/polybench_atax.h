/*
 * OHLS benchmark adapter for PolyBench/C ATAX.
 *
 * Role in the flow: this header fixes a static 64x64 problem size and exposes
 * the HLS top function used by ohls-cli. The computational formula is adapted
 * from PolyBench/C 4.2.1 linear-algebra/kernels/atax.
 */
#ifndef POLYBENCH_ATAX_H
#define POLYBENCH_ATAX_H

#define ATAX_M 64
#define ATAX_N 64

#ifdef __cplusplus
extern "C" {
#endif

/* Compute y = A^T * (A * x) using the PolyBench ATAX loop structure. */
void polybench_atax(const float A[ATAX_M][ATAX_N], const float x[ATAX_N],
                    float y[ATAX_N], float tmp[ATAX_M]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_ATAX_H */
