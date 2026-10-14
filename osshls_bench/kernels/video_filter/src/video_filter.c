//===- video_filter.c - RGB color transformations -----------------*- C -*-===//
//
// Implements the video_filter kernel.
//
//===----------------------------------------------------------------------===//

#include "video_filter.h"

void video_filter(in_int_t offset, in_int_t scale, inout_int_t pixelR[N][N],
                  inout_int_t pixelG[N][N], inout_int_t pixelB[N][N]) {
  for (unsigned row = 0; row < N; row++) {
    for (unsigned col = 0; col < N; col++) {
      pixelR[row][col] = ((pixelR[row][col] - offset) * scale) >> 4;
      pixelB[row][col] = ((pixelB[row][col] - offset) * scale) >> 4;
      pixelG[row][col] = ((pixelG[row][col] - offset) * scale) >> 4;
    }
  }
}
