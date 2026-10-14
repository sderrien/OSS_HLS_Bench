#include <stdint.h>
#include <stdio.h>

#define N 12 // Nombre d'itérations (précision)
#define Q 15 // Nombre de bits de fraction

// Table atanh(2^-i) en Q1.15 (pré-calculée)
static const int16_t atanh_table[N] = {
    17900, // atanh(2^-0)  ≈ 0.5493 * 32768
    8370,  // atanh(2^-1)  ≈ 0.2554 * 32768
    4117,  // atanh(2^-2)  ≈ 0.1257 * 32768
    2048,  // atanh(2^-3)  ≈ 0.0625 * 32768
    1024,  // atanh(2^-4)  ≈ 0.03125 * 32768
    512,   // etc.
    256,
    128,
    64,
    32,
    16,
    8
};

#pragma HLS top
int16_t cordic_exp_q15(int16_t x) {
    int32_t X = 32768; // 1.0 en Q1.15
    int32_t Y = 0;
    int32_t Z = x;
#pragma HLS pipeline
    for (int i = 0; i < N; ++i) {
        int32_t d = (Z >= 0) ? 1 : -1;
        // Décalages arithmétiques pour fixed-point
        int32_t x_new = X + d * (Y >> i);
        int32_t y_new = Y + d * (X >> i);
        int32_t z_new = Z - d * atanh_table[i];
        printf(
            "cordic iter=%d X=%d Y=%d Z=%d d=%d", i, X, Y, Z, d);
        X = x_new;
        Y = y_new;
        Z = z_new;
    }
    // exp(x) ≈ X + Y (toujours en Q1.15)
    int32_t result = X + Y;
    // Saturation sur 16 bits
    if (result > 32767) result = 32767;
    if (result < -32768) result = -32768;
    return (int16_t)result;
}
