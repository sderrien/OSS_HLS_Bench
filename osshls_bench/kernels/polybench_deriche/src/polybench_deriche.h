/*
 * OHLS benchmark adapter for PolyBench/C deriche.
 *
 * Role in the flow: define a static 64x64 recursive image filter workload and
 * declare the HLS top function adapted from PolyBench/C 4.2.1 medley/deriche.
 */
#ifndef POLYBENCH_DERICHE_H
#define POLYBENCH_DERICHE_H

#define DERICHE_W 64
#define DERICHE_H 64

#ifdef __cplusplus
extern "C" {
#endif

/* Apply one Deriche image-filter pass using temporary y1/y2 work buffers. */
void polybench_deriche(float alpha, const float img_in[DERICHE_W][DERICHE_H],
                       float img_out[DERICHE_W][DERICHE_H],
                       float y1[DERICHE_W][DERICHE_H],
                       float y2[DERICHE_W][DERICHE_H]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_DERICHE_H */
