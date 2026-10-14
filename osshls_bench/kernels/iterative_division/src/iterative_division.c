#include "iterative_division.h"

// Maybe we can create a stdlib for dynamatic? Creating a select for this is a
// overkill...
static inline int abs_int(int x) {
  return (x >= 0) ? x : -x;
}

int iterative_division(in_int_t dividend, in_int_t divisor) {
  if (divisor == 0)
    return -1;
  int quotient = 0;
  int sign = ((dividend < 0) ^ (divisor < 0)) ? -1 : 1;

  dividend = abs_int(dividend);
  divisor = abs_int(divisor);

  while (dividend >= divisor) {
    dividend -= divisor;
    quotient++;
  }
  return (sign == -1) ? -quotient : quotient;
}
