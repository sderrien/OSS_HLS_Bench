#ifndef KALMAN_5X5_ACCEL_H
#define KALMAN_5X5_ACCEL_H

#include <ap_fixed.h>

/*
 * Shared constants and packet types for the 5-state fixed-point Kalman
 * acceleration tracker.
 *
 * Role in the flow:
 * Keep the kernel and host reference on one deterministic 2D tracking problem so
 * OpenHLS can compare exact integer outputs across software execution, OHLS
 * scheduling, CycleSim, and RTL simulation.  The top-level ABI is a persistent
 * single-sample step: each call consumes one control-tick worth of acceleration
 * and position measurements, updates internal estimator state, and returns the
 * corresponding state estimate.  The reset input initializes that persistent
 * state deterministically before a new trajectory.
 *
 * Model:
 * The state vector is [px, py, vx, vy, accel_bias].  Inputs provide measured
 * acceleration in x/y and measured position in x/y.  All estimator data uses
 * `ap_fixed<8,8>`, an 8-bit signed fixed-point type with no fractional bits, so
 * the benchmark exercises HLS fixed-point frontend support while keeping exact
 * integer-valued comparison in the host reference.  The covariance is a full 5x5
 * matrix updated with a small scalar correction for each position measurement.
 */

#define KALMAN_5X5_STEPS 16
#define KALMAN_5X5_STATE 5
#define KALMAN_5X5_MEAS 2
#define KALMAN_5X5_SCALE 1
#define KALMAN_5X5_DT 1
#define KALMAN_5X5_HALF_DT2 0
#define KALMAN_5X5_PROCESS_NOISE 5
#define KALMAN_5X5_MEASUREMENT_NOISE 97

typedef ap_fixed<8, 8> kalman_5x5_data_t;

/*
 * One estimator input sample.
 *
 * Role:
 * Represent the data naturally available at one real-time control tick.  The
 * acceleration terms drive prediction, while measured x/y position drive the two
 * scalar correction steps.  In a real streaming integration this packet would map
 * to the payload of a valid/ready stream beat.
 */
typedef struct {
  kalman_5x5_data_t accel[KALMAN_5X5_MEAS];
  kalman_5x5_data_t measurement[KALMAN_5X5_MEAS];
} kalman_5x5_sample_t;

/*
 * One estimator output sample.
 *
 * Role:
 * Carry the complete state estimate produced after consuming one input sample.
 * The array order matches the model state [px, py, vx, vy, accel_bias].
 */
typedef struct {
  kalman_5x5_data_t state[KALMAN_5X5_STATE];
} kalman_5x5_estimate_t;

void kalman_5x5_accel(const kalman_5x5_sample_t *sample, int reset,
                      kalman_5x5_estimate_t *estimate);

#endif
