#include <stdio.h>

/*
 * Compute the greatest common divisor with Euclid's subtraction algorithm.
 *
 * The zero-operand guards are part of the mathematical definition used by the
 * benchmark harness: gcd(0, b) is b and gcd(a, 0) is a.  They also prevent RTL
 * cycle simulation from entering the subtraction loop with a zero operand, where
 * subtracting zero would never make progress toward a == b.
 */
#pragma HLS top
int gcd(int a, int b) {
  if (a == 0) return b;
  if (b == 0) return a;

  #pragma HLS pipeline
  while (a != b) {
    printf("[gcd] a=%d b=%d\n", a, b);
    if (a > b) {
      printf("  %d > %d, a = %d - %d\n", a, b, a, b);
      a = a - b;
    } else {
      printf("  %d <= %d, b = %d - %d\n", a, b, b, a);
      b = b - a;
    }
  }
  return a;
}
