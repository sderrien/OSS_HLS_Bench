/*
 * OHLS benchmark adapter for an 8x8 Chen-style 2D DCT image tile.
 *
 * Role in the flow: define the fixed benchmark shape and expose the shared 1D
 * Chen factorization used by both the HLS kernel and the software reference.
 *
 * Why: a tiny grayscale image transform is a compact way to exercise row/column
 * decomposition, staged temporary storage, and trace-friendly integer datapaths.
 * What: apply an 8-point Chen DCT twice, once across rows and once across
 * columns, to produce one 2D coefficient block.
 * How: keep the 1D transform in a single inline helper so the kernel and
 * reference agree bit-for-bit while the kernel adds trace probes around the
 * shared arithmetic.
 */
#ifndef DCT2D_CHEN_H
#define DCT2D_CHEN_H

#include <stdint.h>

#define DCT2D_CHEN_N 8

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared Chen-style 1D DCT helper.
 *
 * Role: transform one 8-sample vector into 8 frequency-domain coefficients.
 * Goal: provide the row/column primitive for the 2D benchmark without pulling
 * in libm or floating-point trigonometric calls. The helper is a macro instead
 * of a function so the frontend does not need to lower an intra-benchmark call.
 */
#define CHEN_DCT8_1D(IN, OUT)                                                    \
  do {                                                                           \
    enum {                                                                       \
      FXP_C0 = 46341,                                                            \
      FXP_C1 = 32138,                                                            \
      FXP_C2 = 30274,                                                            \
      FXP_C3 = 27245,                                                            \
      FXP_C4 = 23170,                                                            \
      FXP_C5 = 18205,                                                            \
      FXP_C6 = 12540,                                                            \
      FXP_C7 = 6393,                                                             \
    };                                                                            \
    int32_t chen_tmp[8];                                                         \
    int32_t chen_tmp2[8];                                                        \
                                                                                 \
    for (int chen_i = 0; chen_i < 4; ++chen_i) {                                 \
      chen_tmp[chen_i] = (IN)[chen_i] + (IN)[7 - chen_i];                        \
    }                                                                             \
    for (int chen_i = 4; chen_i < 8; ++chen_i) {                                 \
      chen_tmp[chen_i] = -(IN)[chen_i] + (IN)[7 - chen_i];                       \
    }                                                                             \
                                                                                 \
    chen_tmp2[0] = chen_tmp[0] + chen_tmp[3];                                    \
    chen_tmp2[1] = chen_tmp[1] + chen_tmp[2];                                    \
    chen_tmp2[2] = chen_tmp[1] - chen_tmp[2];                                    \
    chen_tmp2[3] = chen_tmp[0] - chen_tmp[3];                                    \
    chen_tmp2[4] = chen_tmp[4];                                                  \
    chen_tmp2[5] = (FXP_C4 * (chen_tmp[6] - chen_tmp[5])) >> 15;                 \
    chen_tmp2[6] = (FXP_C4 * (chen_tmp[5] + chen_tmp[5])) >> 15;                 \
    chen_tmp2[7] = chen_tmp[7];                                                  \
                                                                                 \
    chen_tmp[0] = (FXP_C4 * (chen_tmp2[0] + chen_tmp2[1])) >> 15;                \
    chen_tmp[1] = (FXP_C4 * (chen_tmp2[0] - chen_tmp2[1])) >> 15;                \
    chen_tmp[2] = ((FXP_C6 * chen_tmp2[2]) >> 15) +                              \
                  ((FXP_C2 * chen_tmp2[3]) >> 15);                               \
    chen_tmp[3] = ((FXP_C6 * chen_tmp2[3]) >> 15) -                              \
                  ((FXP_C2 * chen_tmp2[2]) >> 15);                               \
    chen_tmp[4] = chen_tmp2[4] + chen_tmp2[5];                                    \
    chen_tmp[5] = chen_tmp2[4] - chen_tmp2[5];                                    \
    chen_tmp[6] = chen_tmp2[7] - chen_tmp2[6];                                    \
    chen_tmp[7] = chen_tmp2[6] + chen_tmp2[7];                                    \
                                                                                 \
    (OUT)[0] = chen_tmp[0] >> 1;                                                 \
    (OUT)[4] = chen_tmp[1] >> 1;                                                 \
    (OUT)[2] = chen_tmp[2] >> 1;                                                 \
    (OUT)[6] = chen_tmp[3] >> 1;                                                 \
    (OUT)[1] = (((FXP_C7 * chen_tmp[4]) >> 15) +                                 \
                ((FXP_C1 * chen_tmp[7]) >> 15)) >> 1;                            \
    (OUT)[5] = (((FXP_C3 * chen_tmp[5]) >> 15) +                                 \
                ((FXP_C5 * chen_tmp[6]) >> 15)) >> 1;                            \
    (OUT)[3] = (((FXP_C3 * chen_tmp[6]) >> 15) -                                 \
                ((FXP_C5 * chen_tmp[5]) >> 15)) >> 1;                            \
    (OUT)[7] = (((FXP_C7 * chen_tmp[7]) >> 15) -                                 \
                ((FXP_C1 * chen_tmp[4]) >> 15)) >> 1;                            \
  } while (0)

/*
 * HLS top-level kernel.
 *
 * Role: compute the 2D DCT coefficient block for an 8x8 grayscale tile.
 * Goal: keep the kernel small, deterministic, and traceable while still
 * exercising a real row/column transform pipeline.
 */
void dct2d_chen(const uint8_t image[DCT2D_CHEN_N][DCT2D_CHEN_N],
                int32_t coeffs[DCT2D_CHEN_N][DCT2D_CHEN_N]);

#ifdef __cplusplus
}
#endif

#endif /* DCT2D_CHEN_H */
