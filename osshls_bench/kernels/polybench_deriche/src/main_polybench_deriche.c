/*
 * Host verification driver for the OHLS PolyBench Deriche benchmark.
 *
 * Role in the flow: initialize a deterministic grayscale image, run the HLS top
 * and a plain C reference, and report checksum/match status for ohls-cli.
 */
#include <math.h>
#include <stdio.h>

#include "polybench_deriche.h"

static const float kTolerance = 1e-3f;

/* Initialize the input image with the PolyBench Deriche formula. */
static void init_image(float img_in[DERICHE_W][DERICHE_H]) {
  for (int i = 0; i < DERICHE_W; ++i) {
    for (int j = 0; j < DERICHE_H; ++j) {
      img_in[i][j] = (float)((313 * i + 991 * j) % 65536) / 65535.0f;
    }
  }
}

/* Golden Deriche implementation without HLS annotations. */
static void reference_deriche(float alpha, const float img_in[DERICHE_W][DERICHE_H],
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
    for (int j = DERICHE_H - 1; j >= 0; --j) {
      y2[i][j] = a3 * xp1 + a4 * xp2 + b1 * yp1 + b2 * yp2;
      xp2 = xp1;
      xp1 = img_in[i][j];
      yp2 = yp1;
      yp1 = y2[i][j];
    }
  }
  for (int i = 0; i < DERICHE_W; ++i) {
    for (int j = 0; j < DERICHE_H; ++j) {
      img_out[i][j] = y1[i][j] + y2[i][j];
    }
  }
  for (int j = 0; j < DERICHE_H; ++j) {
    float tm1 = 0.0f;
    float ym1 = 0.0f;
    float ym2 = 0.0f;
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
    for (int i = DERICHE_W - 1; i >= 0; --i) {
      y2[i][j] = a7 * tp1 + a8 * tp2 + b1 * yp1 + b2 * yp2;
      tp2 = tp1;
      tp1 = img_out[i][j];
      yp2 = yp1;
      yp1 = y2[i][j];
    }
  }
  for (int i = 0; i < DERICHE_W; ++i) {
    for (int j = 0; j < DERICHE_H; ++j) {
      img_out[i][j] = y1[i][j] + y2[i][j];
    }
  }
}

/* Compute a deterministic signature of the filtered image. */
static float checksum_image(const float img_out[DERICHE_W][DERICHE_H]) {
  float sum = 0.0f;
  for (int i = 0; i < DERICHE_W; ++i) {
    for (int j = 0; j < DERICHE_H; ++j) {
      sum += img_out[i][j] * (float)(1 + i + j);
    }
  }
  return sum;
}

/* Compare two filtered images and return the first mismatching pixel. */
static int compare_images(const float lhs[DERICHE_W][DERICHE_H],
                          const float rhs[DERICHE_W][DERICHE_H], int *bad_i,
                          int *bad_j) {
  for (int i = 0; i < DERICHE_W; ++i) {
    for (int j = 0; j < DERICHE_H; ++j) {
      if (fabsf(lhs[i][j] - rhs[i][j]) > kTolerance) {
        *bad_i = i;
        *bad_j = j;
        return 0;
      }
    }
  }
  *bad_i = -1;
  *bad_j = -1;
  return 1;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL polybench_deriche: cannot open output file\n");
      return 2;
    }
  }

  static float img_in[DERICHE_W][DERICHE_H];
  static float img_kernel[DERICHE_W][DERICHE_H];
  static float img_reference[DERICHE_W][DERICHE_H];
  static float y1_kernel[DERICHE_W][DERICHE_H];
  static float y2_kernel[DERICHE_W][DERICHE_H];
  static float y1_reference[DERICHE_W][DERICHE_H];
  static float y2_reference[DERICHE_W][DERICHE_H];
  const float alpha = 0.25f;

  init_image(img_in);
  polybench_deriche(alpha, img_in, img_kernel, y1_kernel, y2_kernel);
  reference_deriche(alpha, img_in, img_reference, y1_reference, y2_reference);

  int bad_i = -1;
  int bad_j = -1;
  int match = compare_images(img_kernel, img_reference, &bad_i, &bad_j);
  fprintf(out, "kernel_checksum=%f\n", checksum_image(img_kernel));
  fprintf(out, "reference_checksum=%f\n", checksum_image(img_reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_i, bad_j);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
