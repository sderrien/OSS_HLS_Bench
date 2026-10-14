#include <stdio.h>

int gcd(int a, int b);

int ref_gcd(int a, int b) {
  if (a == 0) return b;
  if (b == 0) return a;

  while (a != b) {
    if (a > b) {
      a = a - b;
    } else {
      b = b - a;
    }
  }
  return a;
}

int main(int argc, char **argv) {
  FILE *out = stdout;
  if (argc > 1) out = fopen(argv[1], "w");

  // Match the shared runner input: this pair exercises a long subtraction-GCD loop.
  int test_cases[] = {488732, 182};
  int num_tests = 1;

  int passed = 1;
  for (int i = 0; i < num_tests; ++i) {
    int a = test_cases[2*i];
    int b = test_cases[2*i + 1];
    int result = gcd(a, b);
    int expected = ref_gcd(a,b);
    int ok = (result == expected);
    fprintf(out, "gcd(%d, %d) = %d (expected %d) %s\n", 
            a, b, result, expected, ok ? "OK" : "FAIL");
    if (!ok) passed = 0;
  }

  fprintf(out, "result=%s\n", passed ? "PASS" : "FAIL");
  if (out != stdout) fclose(out);
  return passed ? 0 : 1;
}
