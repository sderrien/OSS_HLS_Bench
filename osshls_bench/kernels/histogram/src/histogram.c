#include <stddef.h>
#include <stdint.h>

#define HIST_N 64
#define BINS 8

#pragma HLS toplevel
#pragma HLS resource variable=bins core=ram_1p latency=4 num_read_outstanding=1 num_write_outstanding=1
void histogram(const uint8_t in[HIST_N], uint16_t bins[BINS]) {
    #pragma HLS pipeline
    for (size_t i = 0; i < BINS; ++i) {
        bins[i] = 0;
    }
    #pragma HLS pipeline
    for (size_t i = 0; i < HIST_N; ++i) {
        uint8_t v = in[i] & (BINS - 1);
        bins[v] += 1;
    }
}
