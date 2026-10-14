#include "matmul.h"
#include <stdio.h>
#pragma HLS top


#pragma HLS resource variable=A core=ram_2p read_ports=2 write_ports=2
#pragma HLS resource variable=B core=ram_2p read_ports=2 write_ports=2
#pragma HLS resource variable=C core=ram_2p read_ports=2 write_ports=2
void matmul_ikj_unroll_part_x4(const int A[N][N], const int B[N][N], int C[N][N]) {

#pragma HLS array_partition variable=lA type=cyclic factor=2 dim=0
#pragma HLS array_partition variable=lB type=cyclic factor=2 dim=0
#pragma HLS array_partition variable=lC type=cyclic factor=2 dim=0
  int lA[N][N],lB[N][N],lC[N][N];
  
  for (int i = 0; i < N; i ++) {
    #pragma HLS pipeline
    for (int j = 0; j < N; j ++) {
      lA[i][j] = A[i][j];
      lB[j][i] = B[i][j];
      lC[i][j] = 0;
    }
  }


   for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      int acc = 0;
      #pragma HLS pipeline
#pragma HLS unroll 4
      for (int k = 0; k < N; ++k) {
        acc += lA[i][k] * lB[j][k];
        printf("sum[%d,%d,%d]=%d\n",i,j,k,acc);
      }
      lC[i][j] = acc;
    }
  }

  for (int i = 0; i < N; i ++) {
    #pragma HLS pipeline
    for (int j = 0; j < N; j ++) {
      C[i][j] = lC[i][j];
    }
  }
  
}
