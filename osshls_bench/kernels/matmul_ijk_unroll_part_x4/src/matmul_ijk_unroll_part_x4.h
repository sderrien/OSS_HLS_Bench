#ifndef MATMUL_IJK_UNROLL_PART_X4_H
#define MATMUL_IJK_UNROLL_PART_X4_H

//#define N 256
#define N 64
#define T 16

#ifdef __cplusplus
extern "C" {
#endif

void matmul_ijk_unroll_part_x4(const int A[N][N], const int B[N][N], int C[N][N]);

#ifdef __cplusplus
}
#endif

#endif
