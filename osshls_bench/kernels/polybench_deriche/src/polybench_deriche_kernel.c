/*
 * HLS top for the PolyBench/C Deriche benchmark.
 *
 * Why: Deriche stresses recursive forward/backward image-filter passes with
 * loop-carried state. What: it filters a grayscale image using coefficient sets
 * derived from alpha. How: the PolyBench recurrence and pass order are retained
 * with pipeline annotations on the innermost scan loops.
 *
 * Adapted from PolyBench/C 4.2.1, Ohio State University Software Distribution
 * License.
 */
#include <math.h>

#include "polybench_deriche.h"

__attribute__((annotate("hls.toplevel")))
void polybench_deriche(float alpha, const float img_in[DERICHE_W][DERICHE_H],
                       float img_out[DERICHE_W][DERICHE_H],
                       float y1[DERICHE_W][DERICHE_H],
                       float y2[DERICHE_W][DERICHE_H]) {
  float k = (1.0f - expf(-alpha)) * (1.0f - expf(-alpha)) /
            (1.0f + 2.0f * alpha * expf(-alpha) - expf(2.0f * alpha));
  float a1 = k;
  float a2 = k * expf(-alpha) * (alpha - 1.0f);
  float a3 = k * expf(-alpha) * (alpha + 1.0f);
  float a4 = -k * expf(-2.0f * alpha);
  float a5 = k;
  float a6 = a2;
  float a7 = a3;
  float a8 = a4;
  float b1 = powf(2.0f, -alpha);
  float b2 = -expf(-2.0f * alpha);

  for (int i = 0; i < DERICHE_W; ++i) {
    float ym1 = 0.0f;
    float ym2 = 0.0f;
    float xm1 = 0.0f;
#pragma HLS pipeline
    for (int j = 0; j < DERICHE_H; ++j) {
      y1[i][j] = a1 * img_in[i][j] + a2 * xm1 + b1 * ym1 + b2 * ym2;
      xm1 = img_in[i][j];
      ym2 = ym1;
      ym1 = y1[i][j];
    }
  }

  for (int i = 0; i < DERICHE_W; ++i) {
    float yp1 = 0.0f;
    float yp2 = 0.0f;
    float xp1 = 0.0f;
    float xp2 = 0.0f;
#pragma HLS pipeline
    for (int j = DERICHE_H - 1; j >= 0; --j) {
      y2[i][j] = a3 * xp1 + a4 * xp2 + b1 * yp1 + b2 * yp2;
      xp2 = xp1;
      xp1 = img_in[i][j];
      yp2 = yp1;
      yp1 = y2[i][j];
    }
  }

  for (int i = 0; i < DERICHE_W; ++i) {
#pragma HLS pipeline
    for (int j = 0; j < DERICHE_H; ++j) {
      img_out[i][j] = y1[i][j] + y2[i][j];
    }
  }

  for (int j = 0; j < DERICHE_H; ++j) {
    float tm1 = 0.0f;
    float ym1 = 0.0f;
    float ym2 = 0.0f;
#pragma HLS pipeline
    for (int i = 0; i < DERICHE_W; ++i) {
      y1[i][j] = a5 * img_out[i][j] + a6 * tm1 + b1 * ym1 + b2 * ym2;
      tm1 = img_out[i][j];
      ym2 = ym1;
      ym1 = y1[i][j];
    }
  }

  for (int j = 0; j < DERICHE_H; ++j) {
    float tp1 = 0.0f;
    float tp2 = 0.0f;
    float yp1 = 0.0f;
    float yp2 = 0.0f;
#pragma HLS pipeline
    for (int i = DERICHE_W - 1; i >= 0; --i) {
      y2[i][j] = a7 * tp1 + a8 * tp2 + b1 * yp1 + b2 * yp2;
      tp2 = tp1;
      tp1 = img_out[i][j];
      yp2 = yp1;
      yp1 = y2[i][j];
    }
  }

  for (int i = 0; i < DERICHE_W; ++i) {
#pragma HLS pipeline
    for (int j = 0; j < DERICHE_H; ++j) {
      img_out[i][j] = y1[i][j] + y2[i][j];
    }
  }
}
