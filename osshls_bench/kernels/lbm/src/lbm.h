#ifndef LBM_H
#define LBM_H

/* Lattice dimensions for the D2Q9 benchmark example. */
#define NX 32
#define NY 32
#define Q 9

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Execute one D2Q9 time step using the annotated HLS kernel.
 *
 * @param f The 3-D array storing the distribution functions.
 * @param omega Relaxation factor for the collision step.
 */
void lbm(float f[NX][NY][Q], float omega);

/**
 * Run the equivalent D2Q9 update on the host to serve as the golden reference.
 *
 * @param f The distribution function array that will be updated.
 * @param omega Relaxation factor for the collision step.
 */
void lbm_reference(float f[NX][NY][Q], float omega);

#ifdef __cplusplus
}
#endif

#endif /* LBM_H */
