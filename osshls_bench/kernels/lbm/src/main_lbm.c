#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "lbm.h"

/* Deterministic pseudo-random state used to seed the benchmark fields. */
static unsigned int lcg_state = 109u;

/* Tolerance used when comparing floating-point results between the kernels. */
static const float kGoldenTolerance = 1e-5f;

/**
 * Produce a reproducible pseudo-random floating point value in [0.0, 0.2).
 *
 * The generator mirrors the previous implementation so the two buffers start
 * from identical initial conditions.
 */
static float next_random_value(void) {
  lcg_state = 1664525u * lcg_state + 1013904223u;
  return (float)(lcg_state & 0xFFFFu) / 65535.0f;
}

/**
 * Compare two 3-D fields and report the first coordinates that differ.
 *
 * @return 1 when the fields match within the tolerance, 0 otherwise.
 */
static int compare_fields(const float lhs[NX][NY][Q],
                          const float rhs[NX][NY][Q],
                          int *mx,
                          int *my,
                          int *mq) {
  for (int x = 0; x < NX; ++x) {
    for (int y = 0; y < NY; ++y) {
      for (int q = 0; q < Q; ++q) {
        if (fabsf(lhs[x][y][q] - rhs[x][y][q]) > kGoldenTolerance) {
          *mx = x;
          *my = y;
          *mq = q;
          return 0;
        }
      }
    }
  }
  *mx = *my = *mq = -1;
  return 1;
}

/**
 * Compute a simple checksum across the updated distribution function array.
 *
 * This serves both as human-readable output and as a secondary sanity check.
 */
static float checksum_field(const float field[NX][NY][Q]) {
  float sum = 0.0f;
  for (int x = 0; x < NX; ++x) {
    for (int y = 0; y < NY; ++y) {
      for (int q = 0; q < Q; ++q) {
        sum += field[x][y][q];
      }
    }
  }
  return sum;
}

/**
 * Reference implementation of the in-place D2Q9 collision update that omits HLS directives.
 *
 * This routine is used to validate the collision logic produced by the hardware
 * kernel. It intentionally updates only the source cell, matching the kernel's
 * no-temporary-buffer storage model.
 */
void lbm_reference(float f[NX][NY][Q], float omega) {
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
  for (size_t x = 0; x < NX; ++x) {
    for (size_t y = 0; y < NY; ++y) {
      float rho = 0.0f;
      float ux = 0.0f;
      float uy = 0.0f;
      for (size_t q = 0; q < Q; ++q) {
        float fq = f[x][y][q];
        rho += fq;
        ux += fq * cx[q];
        uy += fq * cy[q];
      }
      ux /= rho;
      uy /= rho;
      for (size_t q = 0; q < Q; ++q) {
        float cu = 3.0f * (cx[q] * ux + cy[q] * uy);
        float feq = w[q] * rho * (1.0f + cu + 0.5f * cu * cu - 1.5f * (ux * ux + uy * uy));
        float f_post = f[x][y][q] + omega * (feq - f[x][y][q]);
        f[x][y][q] = f_post;
      }
    }
  }
}

/**
 * Entry point that drives the kernel/reference comparison and reports the match
 * status to stdout or an optional output file.
 */
int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL lbm: cannot open output file\n");
      return 2;
    }
  }

  static float kernel_field[NX][NY][Q];
  static float golden_field[NX][NY][Q];

  for (int x = 0; x < NX; ++x) {
    for (int y = 0; y < NY; ++y) {
      for (int q = 0; q < Q; ++q) {
        float value = 0.1f + next_random_value() * 0.1f;
        kernel_field[x][y][q] = value;
        golden_field[x][y][q] = value;
      }
    }
  }

  lbm(kernel_field, 1.0f);
  lbm_reference(golden_field, 1.0f);

  int mismatch_x, mismatch_y, mismatch_q;
  int match = compare_fields(kernel_field, golden_field, &mismatch_x, &mismatch_y, &mismatch_q);
  float kernel_checksum = checksum_field(kernel_field);
  float reference_checksum = checksum_field(golden_field);

  fprintf(out, "kernel=%f\n", kernel_checksum);
  fprintf(out, "reference=%f\n", reference_checksum);
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d,%d\n", mismatch_x, mismatch_y, mismatch_q);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
