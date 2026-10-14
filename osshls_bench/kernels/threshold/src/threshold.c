//===- threshold.c - Threshold on RGB channels --------------------*- C -*-===//
//
// Implements the threshold kernel.
//
//===----------------------------------------------------------------------===//

#include "threshold.h"

void threshold(in_int_t th, inout_int_t red[N], inout_int_t green[N],
               inout_int_t blue[N]) {
  for (unsigned i = 0; i < N; i++) {
    int sum = red[i] + green[i] + blue[i];
    if (sum <= th) {
      red[i] = 0;
      green[i] = 0;
      blue[i] = 0;
    }
  }
}
