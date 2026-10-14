#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dotprod.h"


int dotprod(const int A[N], const int B[N]);

static void init_vector(int v[N], uint32_t seed) {
  for (int i = 0; i < N; ++i) {
    seed = seed * 1664525u + 1013904223u;
    v[i] = (int)((seed >> 8) % 17u) - 8;
  }
}

static int dotprod_reference(const int A[N], const int B[N]) {
  int sum = 0;
  for (int i = 0; i < N; ++i) {
    sum += A[i] * B[i];
  }
  return sum;
}

static int run_dotprod(const int A[N], const int B[N]) { return dotprod(A, B); }

static int run_dotprod_reference(const int A[N], const int B[N]) {
  return dotprod_reference(A, B);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    if (freopen(argv[1], "w", stdout) == NULL) {
      perror("freopen");
      return 2;
    }
  }

  static int A[N];
  static int B[N];

  init_vector(A, 1u);
  init_vector(B, 2u);

  int kernel = run_dotprod(A, B);

  int ref = run_dotprod_reference(A, B);

  fprintf(stdout, "kernel=%d\n", kernel);
  fprintf(stdout, "reference=%d\n", ref);
  fprintf(stdout, "match=%s\n", kernel == ref ? "yes" : "no");

  return kernel == ref ? 0 : 2;
}
