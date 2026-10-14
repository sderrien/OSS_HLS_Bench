/*
 * OHLS benchmark adapter for PolyBench/C heat-3d.
 *
 * Role in the flow: define a static 32^3 heat diffusion stencil and expose the
 * HLS top function. The kernel formula is adapted from PolyBench/C 4.2.1
 * stencils/heat-3d.
 */
#ifndef POLYBENCH_HEAT_3D_H
#define POLYBENCH_HEAT_3D_H

#define HEAT_3D_N 32
#define HEAT_3D_TSTEPS 4

#ifdef __cplusplus
extern "C" {
#endif

/* Run alternating 3-D heat updates between A and B for fixed timesteps. */
void polybench_heat_3d(float A[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N],
                       float B[HEAT_3D_N][HEAT_3D_N][HEAT_3D_N]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_HEAT_3D_H */
