/*
 * Self-checking host harness for the MachSuite-style FFT benchmark.
 */

#include "fft.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" void fft8(fft_complex_t data[FFT_N]);
#endif

static void init_signal(fft_complex_t data[FFT_N]) {
    static const fft_complex_t kSignal[FFT_N] = {
        {1.0f, 0.0f}, {2.0f, -1.0f}, {3.0f, 0.5f}, {4.0f, -0.5f},
        {5.0f, 1.0f}, {6.0f, -1.5f}, {7.0f, 0.25f}, {8.0f, -0.25f},
    };
    for (int i = 0; i < FFT_N; ++i) {
        data[i] = kSignal[i];
    }
}

static void reference_fft8(fft_complex_t data[FFT_N]) {
    fft_complex_t out[FFT_N];
    static const float cos_tbl[FFT_N][FFT_N] = {
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 0.70710677f, 0.0f, -0.70710677f, -1.0f, -0.70710677f, 0.0f, 0.70710677f},
        {1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f},
        {1.0f, -0.70710677f, 0.0f, 0.70710677f, -1.0f, 0.70710677f, 0.0f, -0.70710677f},
        {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f},
        {1.0f, -0.70710677f, 0.0f, 0.70710677f, -1.0f, 0.70710677f, 0.0f, -0.70710677f},
        {1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f},
        {1.0f, 0.70710677f, 0.0f, -0.70710677f, -1.0f, -0.70710677f, 0.0f, 0.70710677f},
    };
    static const float sin_tbl[FFT_N][FFT_N] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, -0.70710677f, -1.0f, -0.70710677f, 0.0f, 0.70710677f, 1.0f, 0.70710677f},
        {0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f},
        {0.0f, -0.70710677f, 1.0f, -0.70710677f, 0.0f, 0.70710677f, -1.0f, 0.70710677f},
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.70710677f, -1.0f, 0.70710677f, 0.0f, -0.70710677f, 1.0f, -0.70710677f},
        {0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f},
        {0.0f, 0.70710677f, 1.0f, 0.70710677f, 0.0f, -0.70710677f, -1.0f, -0.70710677f},
    };

    for (int k = 0; k < FFT_N; ++k) {
        float re = 0.0f;
        float im = 0.0f;
        for (int n = 0; n < FFT_N; ++n) {
            float xre = data[n].re;
            float xim = data[n].im;
            float wr = cos_tbl[k][n];
            float wi = -sin_tbl[k][n];
            re += xre * wr - xim * wi;
            im += xre * wi + xim * wr;
        }
        out[k].re = re;
        out[k].im = im;
    }

    for (int i = 0; i < FFT_N; ++i) {
        data[i] = out[i];
    }
}

static float checksum_signal(const fft_complex_t data[FFT_N]) {
    float sum = 0.0f;
    for (int i = 0; i < FFT_N; ++i) {
        sum += data[i].re * (float)(i + 1) + data[i].im * (float)(i + 3);
    }
    return sum;
}

static int compare_signal(const fft_complex_t lhs[FFT_N],
                          const fft_complex_t rhs[FFT_N], int *bad_idx) {
    const float kTolerance = 1e-4f;
    for (int i = 0; i < FFT_N; ++i) {
        float re_diff = lhs[i].re - rhs[i].re;
        float im_diff = lhs[i].im - rhs[i].im;
        if (re_diff < 0.0f) re_diff = -re_diff;
        if (im_diff < 0.0f) im_diff = -im_diff;
        if (re_diff > kTolerance || im_diff > kTolerance) {
            *bad_idx = i;
            return 0;
        }
    }
    *bad_idx = -1;
    return 1;
}

int main(int argc, char **argv) {
    FILE *out = stdout;
    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            fprintf(stderr, "FAIL machsuite_fft: cannot open output file\n");
            return 2;
        }
    }

    fft_complex_t kernel[FFT_N];
    fft_complex_t reference[FFT_N];

    init_signal(kernel);
    for (int i = 0; i < FFT_N; ++i) {
        reference[i] = kernel[i];
    }

    fft8(kernel);
    reference_fft8(reference);

    int bad_idx = -1;
    int match = compare_signal(kernel, reference, &bad_idx);
    fprintf(out, "kernel_checksum=%f\n", checksum_signal(kernel));
    fprintf(out, "reference_checksum=%f\n", checksum_signal(reference));
    fprintf(out, "match=%s\n", match ? "yes" : "no");
    if (!match) {
        fprintf(out, "first_mismatch=%d\n", bad_idx);
    }

    if (out != stdout) {
        fclose(out);
    }
    return match ? 0 : 2;
}
