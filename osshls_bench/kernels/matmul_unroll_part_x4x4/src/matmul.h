#ifndef MATMUL_H
#define MATMUL_H

//#define N 256
#define N 64
#define T 16

#ifdef __cplusplus
extern "C" {
#endif

void matmul_unroll_part_x4(const int A[N][N], const int B[N][N], int C[N][N]);

#ifdef __cplusplus
}
#endif

#endif
