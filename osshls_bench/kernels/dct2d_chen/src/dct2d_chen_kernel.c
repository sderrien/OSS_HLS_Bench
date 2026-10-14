#include <ohls_trace.h>
#include "dct2d_chen.h"

/*
 * HLS kernel for the 8x8 Chen DCT benchmark.
 *
 * Role in the flow: run one row transform per image row, transpose via an
 * intermediate block, then run the same 1D primitive across columns. The
 * implementation is side-effect free apart from writing the output coefficients.
 */

#pragma HLS top
void dct2d_chen(const uint8_t image[DCT2D_CHEN_N][DCT2D_CHEN_N],
                int32_t coeffs[DCT2D_CHEN_N][DCT2D_CHEN_N]) {
  int32_t row_stage[DCT2D_CHEN_N][DCT2D_CHEN_N];
  int32_t col_in[DCT2D_CHEN_N];
  int32_t col_out[DCT2D_CHEN_N];

  for (int r = 0; r < DCT2D_CHEN_N; ++r) {
    int32_t row_in[DCT2D_CHEN_N];
    #pragma HLS pipeline
    for (int c = 0; c < DCT2D_CHEN_N; ++c) {
      row_in[c] = (int32_t)image[r][c];
    }
    CHEN_DCT8_1D(row_in, row_stage[r]);
  }

  for (int c = 0; c < DCT2D_CHEN_N; ++c) {
    #pragma HLS pipeline
    for (int r = 0; r < DCT2D_CHEN_N; ++r) {
      col_in[r] = row_stage[r][c];
    }
    CHEN_DCT8_1D(col_in, col_out);
    #pragma HLS pipeline
    for (int r = 0; r < DCT2D_CHEN_N; ++r) {
      coeffs[r][c] = col_out[r];
    }
  }
}
