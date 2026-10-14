#include <stddef.h>

#include "lbm.h"


/**
 * Top-level HLS kernel that performs one in-place collision iteration.
 *
 * The function is marked as the HLS entry point and adds pipelining directives
 * to every innermost loop to ensure efficient scheduling.
 */
__attribute__((annotate("hls.toplevel")))
void lbm(float f[NX][NY][Q], float omega) {
  const int cx[Q] = {0, 1, 0, -1, 0, 1, -1, -1, 1};
  const int cy[Q] = {0, 0, 1, 0, -1, 1, 1, -1, -1};
  const float w[Q] = {4.0f / 9.0f,
                      1.0f / 9.0f,
                      1.0f / 9.0f,
                      1.0f / 9.0f,
                      1.0f / 9.0f,
                      1.0f / 36.0f,
                      1.0f / 36.0f,
                      1.0f / 36.0f,
                      1.0f / 36.0f};
lbm_collide_x:
  for (size_t x = 0; x < NX; ++x) {
lbm_collide_y:
    for (size_t y = 0; y < NY; ++y) {
      float rho = 0.0f;
      float ux = 0.0f;
      float uy = 0.0f;
lbm_macroscopic_q:
#pragma HLS pipeline
      for (size_t q = 0; q < Q; ++q) {
        float fq = f[x][y][q];
        rho += fq;
        ux += fq * cx[q];
        uy += fq * cy[q];
      }
      ux /= rho;
      uy /= rho;
lbm_stream_q:
#pragma HLS pipeline
      for (size_t q = 0; q < Q; ++q) {
        float cu = 3.0f * (cx[q] * ux + cy[q] * uy);
        float feq = w[q] * rho * (1.0f + cu + 0.5f * cu * cu - 1.5f * (ux * ux + uy * uy));
        float f_post = f[x][y][q] + omega * (feq - f[x][y][q]);
        // Update only this cell so all q values use the pre-collision cell state.
        f[x][y][q] = f_post;
      }
    }
  }
}
