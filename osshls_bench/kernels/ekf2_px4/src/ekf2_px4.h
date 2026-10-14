#ifndef EKF2_PX4_H
#define EKF2_PX4_H

#include <stdint.h>

/*
 * Shared constants for the PX4 EKF2-inspired fixed-point benchmark.
 *
 * Role in the flow: define one compact, deterministic navigation problem that
 * exercises the same predict/fuse Kalman structure as PX4 EKF2 without pulling
 * in the full flight-stack dependency graph. Kernel and host reference include
 * this header so they agree on state dimensions, sample count, and fixed-point
 * scaling across C simulation, OHLS lowering, scheduling, and CycleSim.
 *
 * Arithmetic model: all state and covariance values are signed Q8 integers.
 * EKF2_PX4_DT is therefore 1/8 timestep in Q8 units, chosen to keep products
 * in 32-bit range while preserving non-trivial gain and covariance evolution.
 */

#define EKF2_PX4_N 16
#define EKF2_PX4_STATE 8
#define EKF2_PX4_MEAS 4
#define EKF2_PX4_SCALE 256
#define EKF2_PX4_DT 32

#define EKF2_PX4_POS_N 0
#define EKF2_PX4_POS_E 1
#define EKF2_PX4_VEL_N 2
#define EKF2_PX4_VEL_E 3
#define EKF2_PX4_YAW 4
#define EKF2_PX4_GYRO_BIAS_Z 5
#define EKF2_PX4_ACCEL_BIAS_X 6
#define EKF2_PX4_ACCEL_BIAS_Y 7

void ekf2_px4(const int32_t accel_x[EKF2_PX4_N],
              const int32_t accel_y[EKF2_PX4_N],
              const int32_t gyro_z[EKF2_PX4_N],
              const int32_t gps_pos_n[EKF2_PX4_N],
              const int32_t gps_pos_e[EKF2_PX4_N],
              const int32_t gps_vel_n[EKF2_PX4_N],
              const int32_t gps_vel_e[EKF2_PX4_N],
              int32_t state_out[EKF2_PX4_STATE],
              int32_t covariance_out[EKF2_PX4_STATE][EKF2_PX4_STATE]);

#endif
