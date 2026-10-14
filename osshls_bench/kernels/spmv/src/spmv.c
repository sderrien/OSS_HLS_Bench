#include <stddef.h>
#include <stdint.h>
#if TRACE            
#include <ohls_trace.h>
#endif

#define N 256
#define NNZ 6554

__attribute__((annotate("hls.toplevel")))
void spmv(const float val[NNZ], const unsigned char col[NNZ],
          const unsigned short row_ptr[N + 1], const float x[N], float y[N]) {
    for (size_t i = 0; i < N; ++i) {
        float acc = 0.0f;
        unsigned short start = row_ptr[i];
        unsigned short end = row_ptr[i + 1];
        #pragma HLS pipeline
        for (unsigned short idx = start; idx < end; ++idx) {
            unsigned char c = col[idx];
            float prod = val[idx] * x[c];
            acc += prod;
        }
        y[i] = acc;
    }
}
