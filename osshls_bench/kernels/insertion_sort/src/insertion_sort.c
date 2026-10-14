#include "insertion_sort.h"

void insertion_sort(inout_int_t A[N], in_int_t n) {
  for (int i = 1; i < n; ++i) {
    int x = A[i];
    int j = i;
    while (j > 0) {
      if (A[j - 1] <= x)
        break;
      A[j] = A[j - 1];
      --j;
    }
    A[j] = x;
  }
}
