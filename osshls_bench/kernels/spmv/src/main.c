#include <math.h>
#include <stdio.h>

#define N 256
#define NNZ 6554

/* Export the RTL kernel with C linkage so the RTL runtime can link to it. */
#ifdef __cplusplus
extern "C" void spmv(const float val[NNZ], const unsigned char col[NNZ],
                      const unsigned short row_ptr[N + 1], const float x[N], float y[N]);
#else
void spmv(const float val[NNZ], const unsigned char col[NNZ],
          const unsigned short row_ptr[N + 1], const float x[N], float y[N]);
#endif

static void spmv_ref(const float val[NNZ], const unsigned char col[NNZ],
                     const unsigned short row_ptr[N + 1], const float x[N], float y[N]) {
    for (size_t i = 0; i < N; ++i) {
        float acc = 0.0f;
        for (unsigned short idx = row_ptr[i]; idx < row_ptr[i + 1]; ++idx) {
            acc += val[idx] * x[col[idx]];
            printf(
                "spmv row=%zu idx=%u val=%.6f x=%.6f prod=%.6f acc=%.6f",
                i,
                (unsigned)idx,
                val[idx],
                x[col[idx]],
                val[idx] * x[col[idx]],
                acc); 
        }
        y[i] = acc;
    }
}

static unsigned int fixture_mix(unsigned int seed, unsigned int salt) {
    return seed * 1664525u + 1013904223u + salt * 97u;
}

static void init_csr(float val[NNZ], unsigned char col[NNZ],
                     unsigned short row_ptr[N + 1]) {
    unsigned char selected[N][N] = {{0}};
    unsigned int state = 0xC0FFEE11u;
    size_t selected_count = 0;
    size_t index = 0;

    // Sample fixed CSR coordinates without replacement; the row scan below
    // produces sorted columns and intentionally variable row lengths.
    while (selected_count < NNZ) {
        state = 1664525u * state + 1013904223u;
        size_t cell = (size_t)state % (N * N);
        size_t row = cell / N;
        size_t column = cell % N;
        if (!selected[row][column]) {
            selected[row][column] = 1;
            ++selected_count;
        }
    }
    for (size_t row = 0; row < N; ++row) {
        row_ptr[row] = (unsigned short)index;
        for (size_t column = 0; column < N; ++column) {
            if (!selected[row][column])
                continue;
            val[index] = ((float)(fixture_mix(1u, (unsigned int)index + 1u) % 200u) - 100.0f) / 128.0f;
            col[index] = (unsigned char)column;
            ++index;
        }
    }
    row_ptr[N] = (unsigned short)index;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (freopen(argv[1], "w", stdout) == NULL) {
            perror("freopen");
            return 1;
        }
    }
    FILE *out = stdout;
    float val[NNZ];
    unsigned char col[NNZ];
    unsigned short row_ptr[N + 1];
    float x[N];
    float y[N];
    float ref[N];

    init_csr(val, col, row_ptr);
    for (size_t i = 0; i < N; ++i) {
        x[i] = ((float)(fixture_mix(4u, (unsigned int)i + 5u) % 200u) - 100.0f) / 256.0f;
    }

    spmv(val, col, row_ptr, x, y);

    spmv_ref(val, col, row_ptr, x, ref);

    int failures = 0;
    for (size_t i = 0; i < N; ++i) {
        fprintf(out, "spmv compare idx=%zu y=%.6f ref=%.6f\n", i, y[i], ref[i]);
        if (fabsf(y[i] - ref[i]) > 1e-4f) {
            fprintf(out, "FAIL spmv idx %zu\n", i);
            failures = 1;
            break;
        }
    }

    if (!failures)
        fprintf(out, "PASS spmv\n");

    return failures != 0 ? 1 : 0;
}
