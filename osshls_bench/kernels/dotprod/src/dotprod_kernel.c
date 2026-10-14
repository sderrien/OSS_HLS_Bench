#include "dotprod.h"
#include <stdio.h>
#pragma HLS top
#pragma HLS resource variable=A core=ram_1p latency=1 num_read_outstanding=2 num_write_outstanding=2
#pragma HLS resource variable=B core=ram_1p latency=1 num_read_outstanding=2 num_write_outstanding=2
int dotprod(const int A[N], const int B[N])
{
  int sum = 0;
L_1_0:
#pragma HLS pipeline
  for (int i = 0; i < N; ++i)
  {
    sum += A[i] * B[i];
    printf("dotprod[%d]: A=%d B=%d term=%d sum=%d\n", i, A[i], B[i], A[i] * B[i], sum);

  }
  return sum;
}
