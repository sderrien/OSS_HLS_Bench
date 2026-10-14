/*
 * OHLS benchmark adapter for PolyBench/C MVT.
 *
 * Role in the flow: fix a static 64x64 matrix-vector workload and declare the
 * HLS top function. The loop body is adapted from PolyBench/C 4.2.1
 * linear-algebra/kernels/mvt.
 */
#ifndef POLYBENCH_MVT_H
#define POLYBENCH_MVT_H

#define MVT_N 64

#ifdef __cplusplus
extern "C" {
#endif

/* Update x1 with A*y1 and x2 with A^T*y2 using PolyBench MVT semantics. */
void polybench_mvt(float x1[MVT_N], float x2[MVT_N], const float y1[MVT_N],
                   const float y2[MVT_N], const float A[MVT_N][MVT_N]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_MVT_H */
