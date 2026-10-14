#include <stdint.h>
#include <stdio.h>

#include "kalman_5x5_accel.h"

/*
 * Host verification driver for the 5x5 Kalman acceleration tracker benchmark.
 *
 * Role in the flow:
 * Build a deterministic stream of per-tick estimator samples, call the persistent
 * HLS kernel once per sample, run a source-level reference implementation, then
 * emit exact checksums and a match line for OpenHLS benchmark verification.
 */

static kalman_5x5_data_t mul_q8_ref(kalman_5x5_data_t a,
                                    kalman_5x5_data_t b) {
  return a * b;
}

/*
 * Populate the synthetic real-time sample stream used by this benchmark.
 *
 * Role:
 * Create one packet per control tick containing both prediction input
 * acceleration and correction input position measurements.  Keeping these fields
 * in one packet makes the source-level ABI resemble a hardware stream payload
 * without requiring benchmark-specific stream primitives in the C testbench.
 */
static void init_inputs(kalman_5x5_sample_t samples[KALMAN_5X5_STEPS]) {
  kalman_5x5_data_t px = 2 * KALMAN_5X5_SCALE;
  kalman_5x5_data_t py = -1 * KALMAN_5X5_SCALE;
  kalman_5x5_data_t vx = 14;
  kalman_5x5_data_t vy = -9;
  uint32_t seed = 0x3456789au;

  for (int t = 0; t < KALMAN_5X5_STEPS; ++t) {
    seed = seed * 1664525u + 1013904223u;
    kalman_5x5_data_t ax = ((int32_t)((seed >> 24) & 0x0f) - 7) * 3;
    seed = seed * 1664525u + 1013904223u;
    kalman_5x5_data_t ay = ((int32_t)((seed >> 24) & 0x0f) - 8) * 2;
    vx += ax;
    vy += ay;
    px += vx;
    py += vy;
    samples[t].accel[0] = ax + kalman_5x5_data_t(5);
    samples[t].accel[1] = ay - kalman_5x5_data_t(3);
    samples[t].measurement[0] =
        px + kalman_5x5_data_t(((t % 5) - 2) * 11);
    samples[t].measurement[1] =
        py + kalman_5x5_data_t(((t % 7) - 3) * 9);
  }
}

/*
 * Reference model for the benchmark kernel.
 *
 * Role:
 * Mirror the kernel recurrence exactly, including integer division order and the
 * covariance update order, so mismatches indicate frontend/scheduler/backend
 * behavior rather than numerical tolerance differences.
 */
static void reference_kalman_5x5(
    const kalman_5x5_sample_t samples[KALMAN_5X5_STEPS],
    kalman_5x5_estimate_t estimates[KALMAN_5X5_STEPS]) {
  kalman_5x5_data_t x[KALMAN_5X5_STATE] = {0, 0, 0, 0, 0};
  kalman_5x5_data_t p[KALMAN_5X5_STATE][KALMAN_5X5_STATE];

  for (int r = 0; r < KALMAN_5X5_STATE; ++r) {
    for (int c = 0; c < KALMAN_5X5_STATE; ++c) {
      p[r][c] = (r == c) ? (KALMAN_5X5_SCALE * 3 + r * 17) : 0;
    }
  }

  for (int t = 0; t < KALMAN_5X5_STEPS; ++t) {
    kalman_5x5_data_t ax = samples[t].accel[0] - x[4];
    kalman_5x5_data_t ay = samples[t].accel[1] - x[4];
    x[0] = x[0] + mul_q8_ref(x[2], KALMAN_5X5_DT) +
           mul_q8_ref(ax, KALMAN_5X5_HALF_DT2);
    x[1] = x[1] + mul_q8_ref(x[3], KALMAN_5X5_DT) +
           mul_q8_ref(ay, KALMAN_5X5_HALF_DT2);
    x[2] = x[2] + mul_q8_ref(ax, KALMAN_5X5_DT);
    x[3] = x[3] + mul_q8_ref(ay, KALMAN_5X5_DT);

    kalman_5x5_data_t old[KALMAN_5X5_STATE][KALMAN_5X5_STATE];
    for (int r = 0; r < KALMAN_5X5_STATE; ++r) {
      for (int c = 0; c < KALMAN_5X5_STATE; ++c) {
        old[r][c] = p[r][c];
      }
    }
    for (int r = 0; r < KALMAN_5X5_STATE; ++r) {
      for (int c = 0; c < KALMAN_5X5_STATE; ++c) {
        kalman_5x5_data_t v = old[r][c];
        if (r == 0) {
          v += mul_q8_ref(old[2][c], KALMAN_5X5_DT);
        }
        if (r == 1) {
          v += mul_q8_ref(old[3][c], KALMAN_5X5_DT);
        }
        if (c == 0) {
          v += mul_q8_ref(old[r][2], KALMAN_5X5_DT);
        }
        if (c == 1) {
          v += mul_q8_ref(old[r][3], KALMAN_5X5_DT);
        }
        if (r == c) {
          v += kalman_5x5_data_t(KALMAN_5X5_PROCESS_NOISE + r);
        }
        p[r][c] = v;
      }
    }

    for (int axis = 0; axis < KALMAN_5X5_MEAS; ++axis) {
      kalman_5x5_data_t gain[KALMAN_5X5_STATE];
      kalman_5x5_data_t row[KALMAN_5X5_STATE];
      kalman_5x5_data_t innovation = samples[t].measurement[axis] - x[axis];
      kalman_5x5_data_t denom =
          p[axis][axis] + kalman_5x5_data_t(KALMAN_5X5_MEASUREMENT_NOISE);
      for (int i = 0; i < KALMAN_5X5_STATE; ++i) {
        gain[i] = kalman_5x5_data_t(p[i][axis].to_int() / denom.to_int());
        row[i] = p[axis][i];
      }
      for (int i = 0; i < KALMAN_5X5_STATE; ++i) {
        x[i] = x[i] + mul_q8_ref(gain[i], innovation);
      }
      for (int r = 0; r < KALMAN_5X5_STATE; ++r) {
        for (int c = 0; c < KALMAN_5X5_STATE; ++c) {
          p[r][c] = p[r][c] - mul_q8_ref(gain[r], row[c]);
        }
      }
    }

    for (int i = 0; i < KALMAN_5X5_STATE; ++i) {
      estimates[t].state[i] = x[i];
    }
  }
}

static uint64_t checksum_estimates(
    const kalman_5x5_estimate_t estimates[KALMAN_5X5_STEPS]) {
  uint64_t h = 1469598103934665603ULL;
  for (int t = 0; t < KALMAN_5X5_STEPS; ++t) {
    for (int i = 0; i < KALMAN_5X5_STATE; ++i) {
      h ^= (uint64_t)(uint32_t)estimates[t].state[i].to_int();
      h *= 1099511628211ULL;
    }
  }
  return h;
}

static int compare_estimates(
    const kalman_5x5_estimate_t lhs[KALMAN_5X5_STEPS],
    const kalman_5x5_estimate_t rhs[KALMAN_5X5_STEPS], int *bad_t,
    int *bad_i) {
  for (int t = 0; t < KALMAN_5X5_STEPS; ++t) {
    for (int i = 0; i < KALMAN_5X5_STATE; ++i) {
      if (lhs[t].state[i] != rhs[t].state[i]) {
        *bad_t = t;
        *bad_i = i;
        return 0;
      }
    }
  }
  *bad_t = -1;
  *bad_i = -1;
  return 1;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) {
    out = fopen(argv[1], "w");
    if (!out) {
      fprintf(stderr, "FAIL kalman_5x5_accel: cannot open output file\n");
      return 2;
    }
  }

  kalman_5x5_sample_t samples[KALMAN_5X5_STEPS];
  kalman_5x5_estimate_t kernel[KALMAN_5X5_STEPS];
  kalman_5x5_estimate_t reference[KALMAN_5X5_STEPS];

  init_inputs(samples);
  for (int t = 0; t < KALMAN_5X5_STEPS; ++t) {
    kalman_5x5_accel(&samples[t], t == 0, &kernel[t]);
  }
  reference_kalman_5x5(samples, reference);

  int bad_t = -1;
  int bad_i = -1;
  int match = compare_estimates(kernel, reference, &bad_t, &bad_i);
  fprintf(out, "kernel_checksum=%llu\n",
          (unsigned long long)checksum_estimates(kernel));
  fprintf(out, "reference_checksum=%llu\n",
          (unsigned long long)checksum_estimates(reference));
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch_t=%d state=%d kernel=%d reference=%d\n", bad_t,
            bad_i, kernel[bad_t].state[bad_i].to_int(),
            reference[bad_t].state[bad_i].to_int());
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
