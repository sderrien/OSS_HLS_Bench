#include "kalman_5x5_accel.h"

/*
 * 5x5 fixed-point Kalman-style tracker kernel.
 *
 * Role in the flow:
 * Exercise a compact but non-trivial matrix recurrence: a 5-element state vector,
 * a full 5x5 covariance matrix, two sequential measurement corrections,
 * fixed-point division, and persistent state across calls.  The top-level
 * interface uses one input packet and one output packet per control tick, which
 * mirrors how a real estimator IP would sit between sensor/control streams and
 * downstream control logic.  A reset input starts a new trajectory by
 * reinitializing the retained state vector and covariance matrix before
 * processing the current sample.
 *
 * Algorithm:
 * Each step predicts position/velocity using 2D acceleration inputs and a shared
 * acceleration-bias state.  It then applies two scalar measurement updates for
 * x and y position.  The covariance update intentionally stays integer-only and
 * deterministic so the host reference can compare exact bit patterns.  All
 * algorithmic steps are written directly in the top function so the benchmark
 * exposes one flat kernel body to the frontend and scheduler.
 */

#pragma HLS top
void kalman_5x5_accel(const kalman_5x5_sample_t *sample, int reset,
                      kalman_5x5_estimate_t *estimate) {
  static kalman_5x5_data_t x[KALMAN_5X5_STATE];
  static kalman_5x5_data_t p[KALMAN_5X5_STATE][KALMAN_5X5_STATE];
  const kalman_5x5_data_t dt = KALMAN_5X5_DT;
  const kalman_5x5_data_t half_dt2 = KALMAN_5X5_HALF_DT2;
  const kalman_5x5_data_t process_noise = KALMAN_5X5_PROCESS_NOISE;
  const kalman_5x5_data_t measurement_noise = KALMAN_5X5_MEASUREMENT_NOISE;
  const kalman_5x5_data_t zero = 0;

  /* Reset makes the persistent estimator deterministic for a new trajectory. */
  if (reset) {
#pragma HLS pipeline
    for (int r = 0; r < KALMAN_5X5_STATE; ++r) {
      x[r] = 0;
      for (int c = 0; c < KALMAN_5X5_STATE; ++c) {
        p[r][c] = (r == c) ? kalman_5x5_data_t(KALMAN_5X5_SCALE * 3 + r * 17)
                            : kalman_5x5_data_t(0);
      }
    }
  }

  const kalman_5x5_data_t ax = sample->accel[0] - x[4];
  const kalman_5x5_data_t ay = sample->accel[1] - x[4];

  /* Predict the persistent state vector from the newest acceleration sample. */
  x[0] = x[0] + (x[2] * dt) + (ax * half_dt2);
  x[1] = x[1] + (x[3] * dt) + (ay * half_dt2);
  x[2] = x[2] + (ax * dt);
  x[3] = x[3] + (ay * dt);

  /*
   * Snapshot covariance into scalars before any write to p.  This replaces the
   * former old[5][5] scratchpad and preserves the Kalman prediction contract
   * that every covariance cell reads the same pre-prediction matrix.
   */
  const kalman_5x5_data_t p00 = p[0][0];
  const kalman_5x5_data_t p01 = p[0][1];
  const kalman_5x5_data_t p02 = p[0][2];
  const kalman_5x5_data_t p03 = p[0][3];
  const kalman_5x5_data_t p04 = p[0][4];
  const kalman_5x5_data_t p10 = p[1][0];
  const kalman_5x5_data_t p11 = p[1][1];
  const kalman_5x5_data_t p12 = p[1][2];
  const kalman_5x5_data_t p13 = p[1][3];
  const kalman_5x5_data_t p14 = p[1][4];
  const kalman_5x5_data_t p20 = p[2][0];
  const kalman_5x5_data_t p21 = p[2][1];
  const kalman_5x5_data_t p22 = p[2][2];
  const kalman_5x5_data_t p23 = p[2][3];
  const kalman_5x5_data_t p24 = p[2][4];
  const kalman_5x5_data_t p30 = p[3][0];
  const kalman_5x5_data_t p31 = p[3][1];
  const kalman_5x5_data_t p32 = p[3][2];
  const kalman_5x5_data_t p33 = p[3][3];
  const kalman_5x5_data_t p34 = p[3][4];
  const kalman_5x5_data_t p40 = p[4][0];
  const kalman_5x5_data_t p41 = p[4][1];
  const kalman_5x5_data_t p42 = p[4][2];
  const kalman_5x5_data_t p43 = p[4][3];
  const kalman_5x5_data_t p44 = p[4][4];

#define KALMAN_PREDICT_COV_CELL(DST, BASE, ROW_ADV, COL_ADV, DIAG)            \
  do {                                                                         \
    kalman_5x5_data_t v = (BASE);                                              \
    v += (ROW_ADV) * dt;                                                       \
    v += (COL_ADV) * dt;                                                       \
    v += (DIAG);                                                               \
    (DST) = v;                                                                 \
  } while (0)

  KALMAN_PREDICT_COV_CELL(p[0][0], p00, p20, p02,
                          process_noise + kalman_5x5_data_t(0));
  KALMAN_PREDICT_COV_CELL(p[0][1], p01, p21, p03, zero);
  KALMAN_PREDICT_COV_CELL(p[0][2], p02, p22, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[0][3], p03, p23, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[0][4], p04, p24, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[1][0], p10, p30, p12, zero);
  KALMAN_PREDICT_COV_CELL(p[1][1], p11, p31, p13,
                          process_noise + kalman_5x5_data_t(1));
  KALMAN_PREDICT_COV_CELL(p[1][2], p12, p32, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[1][3], p13, p33, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[1][4], p14, p34, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[2][0], p20, zero, p22, zero);
  KALMAN_PREDICT_COV_CELL(p[2][1], p21, zero, p23, zero);
  KALMAN_PREDICT_COV_CELL(p[2][2], p22, zero, zero,
                          process_noise + kalman_5x5_data_t(2));
  KALMAN_PREDICT_COV_CELL(p[2][3], p23, zero, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[2][4], p24, zero, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[3][0], p30, zero, p32, zero);
  KALMAN_PREDICT_COV_CELL(p[3][1], p31, zero, p33, zero);
  KALMAN_PREDICT_COV_CELL(p[3][2], p32, zero, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[3][3], p33, zero, zero,
                          process_noise + kalman_5x5_data_t(3));
  KALMAN_PREDICT_COV_CELL(p[3][4], p34, zero, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[4][0], p40, zero, p42, zero);
  KALMAN_PREDICT_COV_CELL(p[4][1], p41, zero, p43, zero);
  KALMAN_PREDICT_COV_CELL(p[4][2], p42, zero, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[4][3], p43, zero, zero, zero);
  KALMAN_PREDICT_COV_CELL(p[4][4], p44, zero, zero,
                          process_noise + kalman_5x5_data_t(4));

#undef KALMAN_PREDICT_COV_CELL

#define KALMAN_UPDATE_AXIS(AXIS)                                               \
  do {                                                                         \
    const kalman_5x5_data_t innovation = sample->measurement[(AXIS)] - x[(AXIS)]; \
    const kalman_5x5_data_t denom = p[(AXIS)][(AXIS)] + measurement_noise;     \
    const kalman_5x5_data_t gain0 =                                            \
        kalman_5x5_data_t(p[0][(AXIS)].to_int() / denom.to_int());             \
    const kalman_5x5_data_t gain1 =                                            \
        kalman_5x5_data_t(p[1][(AXIS)].to_int() / denom.to_int());             \
    const kalman_5x5_data_t gain2 =                                            \
        kalman_5x5_data_t(p[2][(AXIS)].to_int() / denom.to_int());             \
    const kalman_5x5_data_t gain3 =                                            \
        kalman_5x5_data_t(p[3][(AXIS)].to_int() / denom.to_int());             \
    const kalman_5x5_data_t gain4 =                                            \
        kalman_5x5_data_t(p[4][(AXIS)].to_int() / denom.to_int());             \
    const kalman_5x5_data_t row0 = p[(AXIS)][0];                               \
    const kalman_5x5_data_t row1 = p[(AXIS)][1];                               \
    const kalman_5x5_data_t row2 = p[(AXIS)][2];                               \
    const kalman_5x5_data_t row3 = p[(AXIS)][3];                               \
    const kalman_5x5_data_t row4 = p[(AXIS)][4];                               \
    x[0] = x[0] + (gain0 * innovation);                                        \
    x[1] = x[1] + (gain1 * innovation);                                        \
    x[2] = x[2] + (gain2 * innovation);                                        \
    x[3] = x[3] + (gain3 * innovation);                                        \
    x[4] = x[4] + (gain4 * innovation);                                        \
    p[0][0] = p[0][0] - (gain0 * row0);                                        \
    p[0][1] = p[0][1] - (gain0 * row1);                                        \
    p[0][2] = p[0][2] - (gain0 * row2);                                        \
    p[0][3] = p[0][3] - (gain0 * row3);                                        \
    p[0][4] = p[0][4] - (gain0 * row4);                                        \
    p[1][0] = p[1][0] - (gain1 * row0);                                        \
    p[1][1] = p[1][1] - (gain1 * row1);                                        \
    p[1][2] = p[1][2] - (gain1 * row2);                                        \
    p[1][3] = p[1][3] - (gain1 * row3);                                        \
    p[1][4] = p[1][4] - (gain1 * row4);                                        \
    p[2][0] = p[2][0] - (gain2 * row0);                                        \
    p[2][1] = p[2][1] - (gain2 * row1);                                        \
    p[2][2] = p[2][2] - (gain2 * row2);                                        \
    p[2][3] = p[2][3] - (gain2 * row3);                                        \
    p[2][4] = p[2][4] - (gain2 * row4);                                        \
    p[3][0] = p[3][0] - (gain3 * row0);                                        \
    p[3][1] = p[3][1] - (gain3 * row1);                                        \
    p[3][2] = p[3][2] - (gain3 * row2);                                        \
    p[3][3] = p[3][3] - (gain3 * row3);                                        \
    p[3][4] = p[3][4] - (gain3 * row4);                                        \
    p[4][0] = p[4][0] - (gain4 * row0);                                        \
    p[4][1] = p[4][1] - (gain4 * row1);                                        \
    p[4][2] = p[4][2] - (gain4 * row2);                                        \
    p[4][3] = p[4][3] - (gain4 * row3);                                        \
    p[4][4] = p[4][4] - (gain4 * row4);                                        \
  } while (0)

  /* Apply x-position then y-position scalar measurement corrections. */
  KALMAN_UPDATE_AXIS(0);
  KALMAN_UPDATE_AXIS(1);

#undef KALMAN_UPDATE_AXIS

  estimate->state[0] = x[0];
  estimate->state[1] = x[1];
  estimate->state[2] = x[2];
  estimate->state[3] = x[3];
  estimate->state[4] = x[4];
}
