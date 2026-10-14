#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// --- CORDIC exp(x) en Q1.15 ---
extern int16_t cordic_exp_q15(int16_t x) ;

// Conversion float -> Q1.15 with saturation to avoid overflow
int16_t float_to_q15(float x) {
    float scaled = x * 32768.0f;
    int32_t rounded = (int32_t)lroundf(scaled);
    if (rounded > INT16_MAX) {
        rounded = INT16_MAX;
    } else if (rounded < INT16_MIN) {
        rounded = INT16_MIN;
    }
    return (int16_t)rounded;
}
// Conversion Q1.15 -> float
float q15_to_float(int16_t x) {
    return ((float)x) / 32768.0f;
}

int main(int argc, char **argv) {
    FILE *out = stdout;
    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            perror("fopen");
            return 1;
        }
    }

    // Test sur plusieurs valeurs
    float test_vals[] = { -2.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 2.0f };
    int n = sizeof(test_vals)/sizeof(test_vals[0]);
    for (int i = 0; i < n; ++i) {
        float x = test_vals[i];
        int16_t x_q15 = float_to_q15(x);
        int16_t y_q15 = cordic_exp_q15(x_q15);
        float y_cordic = q15_to_float(y_q15);
        float y_ref = expf(x);
        fprintf(out, "x=%.4f\texp_cordic=%.6f\texp_ref=%.6f\terror=%.6f\n", x, y_cordic, y_ref, y_cordic - y_ref);
    }

    if (out != stdout) {
        fclose(out);
    }
    return 0;
}
