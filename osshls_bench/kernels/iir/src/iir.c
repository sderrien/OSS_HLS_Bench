
#include "iir.h"

int iir(in_int_t y[N], in_int_t x[N], in_int_t a, in_int_t b) {
  int tmp = y[0];
  for (unsigned i = 1; i < N; i++) {
    tmp = a * tmp + b * x[i];
    y[i] = tmp;
  }
  return tmp;
}
