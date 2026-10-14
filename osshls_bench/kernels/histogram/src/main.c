#include <stdint.h>
#include <stdio.h>

#define HIST_N 64
#define BINS 8

void histogram(const uint8_t in[HIST_N], uint16_t bins[BINS]);

static unsigned int lcg_state = 47;
static uint8_t rand_byte(void) {
    lcg_state = 1664525u * lcg_state + 1013904223u;
    return (uint8_t)(lcg_state & 0xFF);
}

static void histogram_ref(const uint8_t in[HIST_N], uint16_t bins[BINS]) {
    for (size_t i = 0; i < BINS; ++i) {
        bins[i] = 0;
    }
    for (size_t i = 0; i < HIST_N; ++i) {
        uint8_t v = in[i] & (BINS - 1);
        bins[v] += 1;
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (freopen(argv[1], "w", stdout) == NULL) {
            perror("freopen");
            return 1;
        }
    }

    FILE *out = stdout;

    uint8_t in[HIST_N];
    uint16_t bins[BINS];
    uint16_t ref[BINS];

    for (size_t i = 0; i < HIST_N; ++i) {
        in[i] = rand_byte();
    }

    histogram(in, bins);

    histogram_ref(in, ref);

    int failures = 0;
    for (size_t i = 0; i < BINS; ++i) {
        if (bins[i] != ref[i]) {
            fprintf(out, "FAIL histogram bin %zu\n", i);
            failures = 1;
            break;
        }
    }

    if (!failures)
        fprintf(out, "PASS histogram\n");

    return failures ? 1 : 0;
}
