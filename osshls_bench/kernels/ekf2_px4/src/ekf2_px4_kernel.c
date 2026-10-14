#include "ekf2_px4.h"

/*
 * HLS kernel for a compact PX4 EKF2-inspired fixed-point Kalman filter.
 *
 * Role in the flow: model the core EKF2 computation pattern used in PX4 flight
 * estimation: IMU prediction updates navigation state and covariance every
 * timestep, then GPS position/velocity innovations are fused through sequential
 * scalar Kalman updates. The benchmark intentionally keeps integer divisions in
 * Kalman gain computation so frontend/lowering/scheduling support is exercised
 * rather than avoided.
 *
 * The state is [pos_n, pos_e, vel_n, vel_e, yaw, gyro_bias_z, accel_bias_x,
 * accel_bias_y]. This is not a line-by-line PX4 port; it is a self-contained
 * kernel that preserves the predict/update structure and loop-carried matrix
 * dependencies that make EKF2 useful as an HLS scheduling benchmark.
 */

static int32_t ekf2_abs_i32(int32_t value) { return value < 0 ? -value : value; }

static void ekf2_predict_state(int32_t state[EKF2_PX4_STATE], int32_t ax,
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

static void ekf2_predict_covariance(
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

static void ekf2_fuse_measurement(
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
    state[i] += (gain[i] * innovation) / EKF2_PX4_SCALE;
  }


  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
  }

  for (int j = 0; j < EKF2_PX4_STATE; ++j) {

  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
      covariance[i][j] -= (gain[i] * row_snapshot[j]) / EKF2_PX4_SCALE;
      if (i == j && covariance[i][j] < 1) {
        covariance[i][j] = 1;
      }
    }
  }
}

static void ekf2_limit_bias_states(int32_t state[EKF2_PX4_STATE]) {
  if (ekf2_abs_i32(state[EKF2_PX4_GYRO_BIAS_Z]) > 32) {
    state[EKF2_PX4_GYRO_BIAS_Z] = state[EKF2_PX4_GYRO_BIAS_Z] < 0 ? -32 : 32;
  }
  if (ekf2_abs_i32(state[EKF2_PX4_ACCEL_BIAS_X]) > 48) {
    state[EKF2_PX4_ACCEL_BIAS_X] = state[EKF2_PX4_ACCEL_BIAS_X] < 0 ? -48 : 48;
  }
  if (ekf2_abs_i32(state[EKF2_PX4_ACCEL_BIAS_Y]) > 48) {
    state[EKF2_PX4_ACCEL_BIAS_Y] = state[EKF2_PX4_ACCEL_BIAS_Y] < 0 ? -48 : 48;
  }
}

#pragma HLS top
void ekf2_px4(const int32_t accel_x[EKF2_PX4_N],
              const int32_t accel_y[EKF2_PX4_N],
              const int32_t gyro_z[EKF2_PX4_N],
              const int32_t gps_pos_n[EKF2_PX4_N],
              const int32_t gps_pos_e[EKF2_PX4_N],
              const int32_t gps_vel_n[EKF2_PX4_N],
              const int32_t gps_vel_e[EKF2_PX4_N],
              int32_t state_out[EKF2_PX4_STATE],
              int32_t covariance_out[EKF2_PX4_STATE][EKF2_PX4_STATE]) {
  int32_t state[EKF2_PX4_STATE] = {0, 0, 0, 0, 0, 3, -5, 4};
  int32_t covariance[EKF2_PX4_STATE][EKF2_PX4_STATE];

  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    for (int j = 0; j < EKF2_PX4_STATE; ++j) {
      covariance[i][j] = (i == j) ? (96 + 8 * i) : 0;
    }
  }

  for (int t = 0; t < EKF2_PX4_N; ++t) {
    ekf2_predict_state(state, accel_x[t], accel_y[t], gyro_z[t]);
    ekf2_predict_covariance(covariance);
    ekf2_fuse_measurement(state, covariance, EKF2_PX4_POS_N, gps_pos_n[t], 44);
    ekf2_fuse_measurement(state, covariance, EKF2_PX4_POS_E, gps_pos_e[t], 47);
    ekf2_fuse_measurement(state, covariance, EKF2_PX4_VEL_N, gps_vel_n[t], 30);
    ekf2_fuse_measurement(state, covariance, EKF2_PX4_VEL_E, gps_vel_e[t], 33);
    ekf2_limit_bias_states(state);
  }

  for (int i = 0; i < EKF2_PX4_STATE; ++i) {
    state_out[i] = state[i];
    for (int j = 0; j < EKF2_PX4_STATE; ++j) {
      covariance_out[i][j] = covariance[i][j];
    }
  }
}
