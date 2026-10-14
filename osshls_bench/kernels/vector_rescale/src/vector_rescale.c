#include "vector_rescale.h"

void vector_rescale(inout_int_t a[N], in_int_t c) {
  for (unsigned i = 0; i < N; ++i)
    a[i] = a[i] * c;
}
