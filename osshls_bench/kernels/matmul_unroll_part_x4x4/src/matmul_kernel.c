#include "matmul.h"

#pragma HLS top

#pragma HLS array_partition variable=A type=cyclic factor=2 dim=0
#pragma HLS array_partition variable=B type=cyclic factor=2 dim=0
#pragma HLS array_partition variable=C type=cyclic factor=2 dim=0

#pragma HLS resource variable=A core=ram_2p read_ports=2 write_ports=2
#pragma HLS resource variable=B core=ram_2p read_ports=2 write_ports=2
#pragma HLS resource variable=C core=ram_2p read_ports=2 write_ports=2
void matmul_unroll_part_x4(const int A[N][N], const int B[N][N], int C[N][N]) {
  for (int ii = 0; ii < N; ii += T) {
    for (int jj = 0; jj < N; jj += T) {
      for (int kk = 0; kk < N; kk += T) {
        for (int i = 0; i < T; ++i) {
          for (int j = 0; j < T; ++j) {
            int sum = C[ii + i][jj + j];
            #pragma HLS unroll 4
            #pragma HLS pipeline
            for (int k = 0; k < T; ++k) {
              sum += A[ii + i][kk + k] * B[kk + k][jj + j];
            }
            C[ii + i][jj + j] = sum;
          }
        }
      }
    }
  }
}
