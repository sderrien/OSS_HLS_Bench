#include <stdint.h>
#include <stdio.h>

#include "ekf2_px4.h"

/*
 * Host verification driver for the PX4 EKF2-inspired benchmark.
 *
 * Role in the flow: generate a deterministic IMU/GPS replay, run the HLS kernel
 * and an independent plain-C reference implementation, and compare final state
 * plus covariance checksums. The deterministic replay gives stable regression
 * signatures while OHLS targets check that optimized/lowered/scheduled forms
 * preserve the same integer Kalman behavior.
 */

static int32_t ref_abs_i32(int32_t value) { return value < 0 ? -value : value; }

static void init_sensor_replay(int32_t accel_x[EKF2_PX4_N],
                               int32_t accel_y[EKF2_PX4_N],
                               int32_t gyro_z[EKF2_PX4_N],
                               int32_t gps_pos_n[EKF2_PX4_N],
                               int32_t gps_pos_e[EKF2_PX4_N],
                               int32_t gps_vel_n[EKF2_PX4_N],
                               int32_t gps_vel_e[EKF2_PX4_N]) {
  int32_t pos_n = 12 * EKF2_PX4_SCALE;
  int32_t pos_e = -7 * EKF2_PX4_SCALE;
  int32_t vel_n = 2 * EKF2_PX4_SCALE;
  int32_t vel_e = -1 * EKF2_PX4_SCALE;
  uint32_t seed = 0x4b1d2a39u;

  for (int t = 0; t < EKF2_PX4_N; ++t) {
    seed = seed * 1664525u + 1013904223u;
    int32_t ax = 18 + (int32_t)(t % 4) * 3;
    int32_t ay = -9 + (int32_t)(t % 3) * 2;
    int32_t gz = 7 + (int32_t)(t % 5);
    vel_n += (ax * EKF2_PX4_DT) / EKF2_PX4_SCALE;
    vel_e += (ay * EKF2_PX4_DT) / EKF2_PX4_SCALE;
    pos_n += (vel_n * EKF2_PX4_DT) / EKF2_PX4_SCALE;
    pos_e += (vel_e * EKF2_PX4_DT) / EKF2_PX4_SCALE;

    accel_x[t] = ax + 5;
    accel_y[t] = ay - 4;
    gyro_z[t] = gz + 3;
    gps_pos_n[t] = pos_n + (int32_t)((seed >> 16) & 0x1f) - 16;
    gps_pos_e[t] = pos_e - (int32_t)((seed >> 21) & 0x1f) + 15;
    gps_vel_n[t] = vel_n + (int32_t)((seed >> 11) & 0x0f) - 7;
    gps_vel_e[t] = vel_e - (int32_t)((seed >> 7) & 0x0f) + 8;
  }
}

static void ref_predict_state(int32_t state[EKF2_PX4_STATE], int32_t ax,
                              int32_t ay, int32_t gz) {
  int32_t unbiased_ax = ax - state[EKF2_PX4_ACCEL_BIAS_X];
  int32_t unbiased_ay = ay - state[EKF2_PX4_ACCEL_BIAS_Y];
  int32_t unbiased_gz = gz - state[EKF2_PX4_GYRO_BIAS_Z];

  state[EKF2_PX4_YAW] += (unbiased_gz * EKF2_PX4_DT) / EKF2_PX4_SCALE;
  state[EKF2_PX4_VEL_N] += (unbiased_ax * EKF2_PX4_DT) / EKF2_PX4_SCALE;
  state[EKF2_PX4_VEL_E] += (unbiased_ay * EKF2_PX4_DT) / EKF2_PX4_SCALE;
  state[EKF2_PX4_POS_N] +=
      (state[EKF2_PX4_VEL_N] * EKF2_PX4_DT) / EKF2_PX4_SCALE;
  state[EKF2_PX4_POS_E] +=
      (state[EKF2_PX4_VEL_E] * EKF2_PX4_DT) / EKF2_PX4_SCALE;
}

static void ref_predict_covariance(
    int32_t covariance[EKF2_PX4_STATE][EKF2_PX4_STATE]) {
  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    for (int j = 0; j < EKF2_PX4_STATE; ++j) {
      int32_t coupling = 0;
      if ((i == EKF2_PX4_POS_N && j == EKF2_PX4_VEL_N) ||
          (i == EKF2_PX4_VEL_N && j == EKF2_PX4_POS_N) ||
          (i == EKF2_PX4_POS_E && j == EKF2_PX4_VEL_E) ||
          (i == EKF2_PX4_VEL_E && j == EKF2_PX4_POS_E)) {
        coupling = EKF2_PX4_DT / 2;
      }

      int32_t diagonal_noise = 0;
      if (i == j) {
        diagonal_noise = 2 + i;
        if (i == EKF2_PX4_POS_N || i == EKF2_PX4_POS_E) {
          diagonal_noise += 4;
        }
        if (i == EKF2_PX4_VEL_N || i == EKF2_PX4_VEL_E) {
          diagonal_noise += 6;
        }
      }

      covariance[i][j] += coupling + diagonal_noise;
    }
  }
}

static void ref_fuse_measurement(
    int32_t state[EKF2_PX4_STATE],
    int32_t covariance[EKF2_PX4_STATE][EKF2_PX4_STATE], int meas_index,
    int32_t measurement, int32_t measurement_noise) {
  int32_t innovation = measurement - state[meas_index];
  int32_t innovation_cov = covariance[meas_index][meas_index] + measurement_noise;
  int32_t gain[EKF2_PX4_STATE];
  int32_t row_snapshot[EKF2_PX4_STATE];

  if (innovation_cov == 0) {
    innovation_cov = 1;
  }

  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    gain[i] = (covariance[i][meas_index] * EKF2_PX4_SCALE) / innovation_cov;
    row_snapshot[i] = covariance[meas_index][i];
  }

  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    state[i] += (gain[i] * innovation) / EKF2_PX4_SCALE;
  }

  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    for (int j = 0; j < EKF2_PX4_STATE; ++j) {
      covariance[i][j] -= (gain[i] * row_snapshot[j]) / EKF2_PX4_SCALE;
      if (i == j && covariance[i][j] < 1) {
        covariance[i][j] = 1;
      }
    }
  }
}

static void ref_limit_bias_states(int32_t state[EKF2_PX4_STATE]) {
  if (ref_abs_i32(state[EKF2_PX4_GYRO_BIAS_Z]) > 32) {
    state[EKF2_PX4_GYRO_BIAS_Z] = state[EKF2_PX4_GYRO_BIAS_Z] < 0 ? -32 : 32;
  }
  if (ref_abs_i32(state[EKF2_PX4_ACCEL_BIAS_X]) > 48) {
    state[EKF2_PX4_ACCEL_BIAS_X] = state[EKF2_PX4_ACCEL_BIAS_X] < 0 ? -48 : 48;
  }
  if (ref_abs_i32(state[EKF2_PX4_ACCEL_BIAS_Y]) > 48) {
    state[EKF2_PX4_ACCEL_BIAS_Y] = state[EKF2_PX4_ACCEL_BIAS_Y] < 0 ? -48 : 48;
  }
}

static void reference_ekf2_px4(
    const int32_t accel_x[EKF2_PX4_N], const int32_t accel_y[EKF2_PX4_N],
    const int32_t gyro_z[EKF2_PX4_N], const int32_t gps_pos_n[EKF2_PX4_N],
    const int32_t gps_pos_e[EKF2_PX4_N], const int32_t gps_vel_n[EKF2_PX4_N],
    const int32_t gps_vel_e[EKF2_PX4_N], int32_t state[EKF2_PX4_STATE],
    int32_t covariance[EKF2_PX4_STATE][EKF2_PX4_STATE]) {
  int32_t initial_state[EKF2_PX4_STATE] = {0, 0, 0, 0, 0, 3, -5, 4};
  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    state[i] = initial_state[i];
    for (int j = 0; j < EKF2_PX4_STATE; ++j) {
      covariance[i][j] = (i == j) ? (96 + 8 * i) : 0;
    }
  }

  for (int t = 0; t < EKF2_PX4_N; ++t) {
    ref_predict_state(state, accel_x[t], accel_y[t], gyro_z[t]);
    ref_predict_covariance(covariance);
    ref_fuse_measurement(state, covariance, EKF2_PX4_POS_N, gps_pos_n[t], 44);
    ref_fuse_measurement(state, covariance, EKF2_PX4_POS_E, gps_pos_e[t], 47);
    ref_fuse_measurement(state, covariance, EKF2_PX4_VEL_N, gps_vel_n[t], 30);
    ref_fuse_measurement(state, covariance, EKF2_PX4_VEL_E, gps_vel_e[t], 33);
    ref_limit_bias_states(state);
  }
}

static uint64_t checksum_state(const int32_t state[EKF2_PX4_STATE]) {
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    h ^= (uint64_t)(uint32_t)state[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t checksum_covariance(
    const int32_t covariance[EKF2_PX4_STATE][EKF2_PX4_STATE]) {
  uint64_t h = 1099511628211ULL;
  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    for (int j = 0; j < EKF2_PX4_STATE; ++j) {
      h ^= (uint64_t)(uint32_t)covariance[i][j];
      h *= 1469598103934665603ULL;
    }
  }
  return h;
}

static int compare_outputs(
    const int32_t lhs_state[EKF2_PX4_STATE],
    const int32_t lhs_covariance[EKF2_PX4_STATE][EKF2_PX4_STATE],
    const int32_t rhs_state[EKF2_PX4_STATE],
    const int32_t rhs_covariance[EKF2_PX4_STATE][EKF2_PX4_STATE], int *bad_i,
    int *bad_j) {
  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    if (lhs_state[i] != rhs_state[i]) {
      *bad_i = i;
      *bad_j = -1;
      return 0;
    }
  }
  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    for (int j = 0; j < EKF2_PX4_STATE; ++j) {
      if (lhs_covariance[i][j] != rhs_covariance[i][j]) {
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
      fprintf(stderr, "FAIL ekf2_px4: cannot open output file\n");
      return 2;
    }
  }

  int32_t accel_x[EKF2_PX4_N];
  int32_t accel_y[EKF2_PX4_N];
  int32_t gyro_z[EKF2_PX4_N];
  int32_t gps_pos_n[EKF2_PX4_N];
  int32_t gps_pos_e[EKF2_PX4_N];
  int32_t gps_vel_n[EKF2_PX4_N];
  int32_t gps_vel_e[EKF2_PX4_N];
  int32_t kernel_state[EKF2_PX4_STATE];
  int32_t reference_state[EKF2_PX4_STATE];
  int32_t kernel_covariance[EKF2_PX4_STATE][EKF2_PX4_STATE];
  int32_t reference_covariance[EKF2_PX4_STATE][EKF2_PX4_STATE];

  init_sensor_replay(accel_x, accel_y, gyro_z, gps_pos_n, gps_pos_e, gps_vel_n,
                     gps_vel_e);
  ekf2_px4(accel_x, accel_y, gyro_z, gps_pos_n, gps_pos_e, gps_vel_n, gps_vel_e,
           kernel_state, kernel_covariance);
  reference_ekf2_px4(accel_x, accel_y, gyro_z, gps_pos_n, gps_pos_e, gps_vel_n,
                     gps_vel_e, reference_state, reference_covariance);

  int bad_i = -1;
  int bad_j = -1;
  int match = compare_outputs(kernel_state, kernel_covariance, reference_state,
                              reference_covariance, &bad_i, &bad_j);
  fprintf(out, "kernel_state_checksum=%llu\n",
          (unsigned long long)checksum_state(kernel_state));
  fprintf(out, "reference_state_checksum=%llu\n",
          (unsigned long long)checksum_state(reference_state));
  fprintf(out, "kernel_covariance_checksum=%llu\n",
          (unsigned long long)checksum_covariance(kernel_covariance));
  fprintf(out, "reference_covariance_checksum=%llu\n",
          (unsigned long long)checksum_covariance(reference_covariance));
  fprintf(out, "state0=%d\n", kernel_state[0]);
  fprintf(out, "state3=%d\n", kernel_state[3]);
  fprintf(out, "match=%s\n", match ? "yes" : "no");
  if (!match) {
    fprintf(out, "first_mismatch=%d,%d\n", bad_i, bad_j);
  }

  if (out != stdout) {
    fclose(out);
  }
  return match ? 0 : 2;
}
