#include <stdio.h>
#include <stdint.h>

#include "dct2d_chen.h"

/*
 * Host verification driver for the 8x8 Chen DCT benchmark.
 *
 * Role in the flow: build a deterministic grayscale image block, run the HLS
 * kernel and a plain C reference, and report a compact checksum plus match
 * status so ohls-cli can verify the benchmark end-to-end.
 */

static void init_image(uint8_t image[DCT2D_CHEN_N][DCT2D_CHEN_N]) {
  static const uint8_t kImage[DCT2D_CHEN_N][DCT2D_CHEN_N] = {
      {139, 144, 149, 153, 155, 155, 155, 155},
      {144, 151, 153, 156, 159, 156, 156, 156},
      {150, 155, 160, 163, 158, 156, 156, 156},
      {159, 161, 162, 160, 160, 159, 159, 159},
      {159, 160, 161, 162, 162, 155, 155, 155},
      {161, 161, 161, 161, 160, 157, 157, 157},
      {162, 162, 161, 163, 162, 157, 157, 157},
      {162, 162, 161, 161, 163, 158, 158, 158},
  };

  for (int r = 0; r < DCT2D_CHEN_N; ++r) {
    for (int c = 0; c < DCT2D_CHEN_N; ++c) {
      image[r][c] = kImage[r][c];
    }
  }
}

/*
 * Plain C reference implementation.
 *
 * Role: mirror the kernel's row/column decomposition without any trace calls.
 * Goal: provide a stable oracle for ohls-cli verification.
 */
static void reference_dct2d_chen(const uint8_t image[DCT2D_CHEN_N][DCT2D_CHEN_N],
                                 int32_t coeffs[DCT2D_CHEN_N][DCT2D_CHEN_N]) {
  int32_t row_stage[DCT2D_CHEN_N][DCT2D_CHEN_N];
  int32_t col_in[DCT2D_CHEN_N];
  int32_t col_out[DCT2D_CHEN_N];

  for (int r = 0; r < DCT2D_CHEN_N; ++r) {
    int32_t row_in[DCT2D_CHEN_N];
    for (int c = 0; c < DCT2D_CHEN_N; ++c) {
      row_in[c] = (int32_t)image[r][c];
    }
    CHEN_DCT8_1D(row_in, row_stage[r]);
  }

  for (int c = 0; c < DCT2D_CHEN_N; ++c) {
    for (int r = 0; r < DCT2D_CHEN_N; ++r) {
      col_in[r] = row_stage[r][c];
    }
    CHEN_DCT8_1D(col_in, col_out);
    for (int r = 0; r < DCT2D_CHEN_N; ++r) {
      coeffs[r][c] = col_out[r];
    }
  }
}

/* Produce a compact checksum for the 2D coefficient block. */
static uint64_t checksum_coeffs(const int32_t coeffs[DCT2D_CHEN_N][DCT2D_CHEN_N]) {
  uint64_t h = 1469598103934665603ULL;
  for (int r = 0; r < DCT2D_CHEN_N; ++r) {
    for (int c = 0; c < DCT2D_CHEN_N; ++c) {
      h = (h * 1099511628211ULL) ^ (uint64_t)(uint32_t)coeffs[r][c];
    }
  }
  return h;
}

/* Compare all coefficients and report the first mismatch location. */
static int compare_coeffs(const int32_t lhs[DCT2D_CHEN_N][DCT2D_CHEN_N],
                          const int32_t rhs[DCT2D_CHEN_N][DCT2D_CHEN_N],
                          int *bad_r, int *bad_c) {
  for (int r = 0; r < DCT2D_CHEN_N; ++r) {
    for (int c = 0; c < DCT2D_CHEN_N; ++c) {
      if (lhs[r][c] != rhs[r][c]) {
        *bad_r = r;
        *bad_c = c;
        return 0;
      }
    }
  }
  *bad_r = -1;
  *bad_c = -1;
  return 1;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL dct2d_chen: cannot open output file\n");
      return 2;
    }
  }

  uint8_t image[DCT2D_CHEN_N][DCT2D_CHEN_N];
  int32_t kernel[DCT2D_CHEN_N][DCT2D_CHEN_N];
  int32_t reference[DCT2D_CHEN_N][DCT2D_CHEN_N];

  init_image(image);
  dct2d_chen(image, kernel);
  reference_dct2d_chen(image, reference);

  int bad_r = -1;
  int bad_c = -1;
  int match = compare_coeffs(kernel, reference, &bad_r, &bad_c);
  fprintf(out, "kernel_checksum=%llu\n", (unsigned long long)checksum_coeffs(kernel));
  fprintf(out, "reference_checksum=%llu\n",
          (unsigned long long)checksum_coeffs(reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_r, bad_c);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
