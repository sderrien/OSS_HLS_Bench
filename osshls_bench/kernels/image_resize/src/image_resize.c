#include "image_resize.h"

void image_resize(inout_int_t a[N][N], in_int_t c) {
  for (unsigned i = 0; i < N; i++) {
    for (unsigned j = 0; j < N; j++) {
      a[i][j] = c - a[i][j];
    }
  }
}
