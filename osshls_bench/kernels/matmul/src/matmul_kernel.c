#include <stdio.h>
#include "matmul.h"

#pragma HLS top
void matmul(const int A[N][N], const int B[N][N], int C[N][N]) {
  for (int i = 0; i < N; i += 1) {
    for (int j = 0; j < N; j += 1) {
      int sum =0;
      #pragma HLS pipeline
      for (int k = 0; k < N; k += 1) {
        sum += A[i][k] * B[k][j];
      }
      C[i][j] = sum;
    }
  }
}
