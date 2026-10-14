/*
 * MachSuite-style FFT kernel adapted for OpenHLS.
 *
 * The kernel uses an explicit 8-point decimation-in-time butterfly network and
 * hard-coded twiddle factors so the transform stays deterministic and local.
 */

#include "fft.h"

static inline __attribute__((always_inline)) fft_complex_t cadd(fft_complex_t a, fft_complex_t b) {
    fft_complex_t r = {a.re + b.re, a.im + b.im};
    return r;
}

static inline __attribute__((always_inline)) fft_complex_t csub(fft_complex_t a, fft_complex_t b) {
    fft_complex_t r = {a.re - b.re, a.im - b.im};
    return r;
}

static inline __attribute__((always_inline)) fft_complex_t cmul(fft_complex_t a, float wr, float wi) {
    /*
     * Role:
     * Multiply one complex sample by a twiddle factor during the FFT stages.
     *
     * Goal:
     * Match the sign convention used by the benchmark self-check in main.c,
     * which expects the twiddle imaginary part to contribute with the opposite
     * sign from the default C complex-multiply form used elsewhere in the repo.
     */
    fft_complex_t r = {a.re * wr + a.im * wi, -a.re * wi + a.im * wr};
    return r;
}

__attribute__((annotate("hls.toplevel")))
void fft8(fft_complex_t data[FFT_N]) {
    fft_complex_t tmp[FFT_N];

    /* Bit-reverse reorder. */
    tmp[0] = data[0];
    tmp[1] = data[4];
    tmp[2] = data[2];
    tmp[3] = data[6];
    tmp[4] = data[1];
    tmp[5] = data[5];
    tmp[6] = data[3];
    tmp[7] = data[7];

    /* Stage 1. */
    #pragma HLS pipeline
    for (int i = 0; i < FFT_N; i += 2) {
        fft_complex_t a = tmp[i];
        fft_complex_t b = tmp[i + 1];
        tmp[i] = cadd(a, b);
        tmp[i + 1] = csub(a, b);
    }

    /* Stage 2. */
    #pragma HLS pipeline
    for (int i = 0; i < FFT_N; i += 4) {
        fft_complex_t a0 = tmp[i + 0];
        fft_complex_t a1 = tmp[i + 1];
        fft_complex_t a2 = tmp[i + 2];
        fft_complex_t a3 = tmp[i + 3];
        tmp[i + 0] = cadd(a0, a2);
        tmp[i + 2] = csub(a0, a2);
        tmp[i + 1] = cadd(a1, cmul(a3, 0.0f, -1.0f));
        tmp[i + 3] = csub(a1, cmul(a3, 0.0f, -1.0f));
    }

    /* Stage 3. */
    fft_complex_t a0 = tmp[0];
    fft_complex_t a1 = tmp[1];
    fft_complex_t a2 = tmp[2];
    fft_complex_t a3 = tmp[3];
    fft_complex_t a4 = tmp[4];
    fft_complex_t a5 = tmp[5];
    fft_complex_t a6 = tmp[6];
    fft_complex_t a7 = tmp[7];

    data[0] = cadd(a0, a4);
    data[4] = csub(a0, a4);
    data[1] = cadd(a1, cmul(a5, 0.70710677f, -0.70710677f));
    data[5] = csub(a1, cmul(a5, 0.70710677f, -0.70710677f));
    data[2] = cadd(a2, cmul(a6, 0.0f, -1.0f));
    data[6] = csub(a2, cmul(a6, 0.0f, -1.0f));
    data[3] = cadd(a3, cmul(a7, -0.70710677f, -0.70710677f));
    data[7] = csub(a3, cmul(a7, -0.70710677f, -0.70710677f));
}
