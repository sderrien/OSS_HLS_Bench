/*
 * MachSuite-style FFT benchmark adapter for OpenHLS.
 */
#ifndef MACHSUITE_FFT_H
#define MACHSUITE_FFT_H

#define FFT_N 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float re;
    float im;
} fft_complex_t;

void fft8(fft_complex_t data[FFT_N]);

#ifdef __cplusplus
}
#endif

#endif /* MACHSUITE_FFT_H */
