#include <stdint.h>
#include <stdio.h>

#include "earlyexit.h"

int hb_cfg_switch_early_exit(const int a[EARLYEXIT_N], int key);

static int hb_cfg_switch_early_exit_ref(const int a[EARLYEXIT_N], int key) {
  printf("[earlyexit-ref-fn] begin key=%d\n", key);
  int i = 0;
  int acc = 0;
  int exit_tag = 0;

  while (i < EARLYEXIT_N) {
    const int v = a[i];
    if (i < 3) {
      printf("[earlyexit-ref] i=%d v=%d acc=%d key=%d\n", i, v, acc, key);
    }

    switch (v & 3) {
      case 0:
        acc += v;
        break;

      case 1:
        if (v == key) {
          exit_tag = 11;
        } else {
          acc -= v;
        }
        break;

      case 2:
        i++;
        continue;

      default:
        if (v < 0) {
          exit_tag = 22;
        } else {
          acc ^= (v + 1);
        }
        break;
    }

    if (exit_tag != 0) {
      break;
    }
    i++;
  }

  const int final = acc + i + exit_tag;
  printf("[earlyexit-ref-fn] end key=%d result=%d\n", key, final);
  return final;
}

static void fill_input(int data[EARLYEXIT_N], uint32_t seed) {
  for (int i = 0; i < EARLYEXIT_N; ++i) {
    seed = seed * 1664525u + 1013904223u;
    data[i] = (int)((seed >> 16) & 63) - 24;
  }
}

int main(int argc, char **argv) {
  if (argc > 1) {
    if (freopen(argv[1], "w", stdout) == NULL) {
      perror("freopen");
      return 1;
    }
  }

  FILE *out = stdout;

  int failures = 0;
  int data[EARLYEXIT_N];
  fill_input(data, 1u);
  fprintf(out, "[earlyexit-test] case=random-1 key=9 begin\n");
  int got = hb_cfg_switch_early_exit(data, 9);
  fprintf(out, "[earlyexit-test] case=random-1 kernel_done got=%d\n", got);
  int ref = hb_cfg_switch_early_exit_ref(data, 9);
  fprintf(out, "[earlyexit-test] case=random-1 ref_done ref=%d\n", ref);
  fprintf(out, "random-1 got=%d ref=%d match=%s\n", got, ref,
          got == ref ? "yes" : "no");
  failures += (got == ref) ? 0 : 1;

  fill_input(data, 2u);
  data[5] = 9;
  fprintf(out, "[earlyexit-test] case=break-key key=9 begin\n");
  got = hb_cfg_switch_early_exit(data, 9);
  fprintf(out, "[earlyexit-test] case=break-key kernel_done got=%d\n", got);
  ref = hb_cfg_switch_early_exit_ref(data, 9);
  fprintf(out, "[earlyexit-test] case=break-key ref_done ref=%d\n", ref);
  fprintf(out, "break-key got=%d ref=%d match=%s\n", got, ref,
          got == ref ? "yes" : "no");
  failures += (got == ref) ? 0 : 1;

  fill_input(data, 3u);
  data[4] = -13;
  fprintf(out, "[earlyexit-test] case=break-negative key=9 begin\n");
  got = hb_cfg_switch_early_exit(data, 9);
  fprintf(out, "[earlyexit-test] case=break-negative kernel_done got=%d\n", got);
  ref = hb_cfg_switch_early_exit_ref(data, 9);
  fprintf(out, "[earlyexit-test] case=break-negative ref_done ref=%d\n", ref);
  fprintf(out, "break-negative got=%d ref=%d match=%s\n", got, ref,
          got == ref ? "yes" : "no");
  failures += (got == ref) ? 0 : 1;

  fill_input(data, 4u);
  data[2] = 12;
  data[3] = 14;
  fprintf(out, "[earlyexit-test] case=continue-heavy key=31 begin\n");
  got = hb_cfg_switch_early_exit(data, 31);
  fprintf(out, "[earlyexit-test] case=continue-heavy kernel_done got=%d\n", got);
  ref = hb_cfg_switch_early_exit_ref(data, 31);
  fprintf(out, "[earlyexit-test] case=continue-heavy ref_done ref=%d\n", ref);
  fprintf(out, "continue-heavy got=%d ref=%d match=%s\n", got, ref,
          got == ref ? "yes" : "no");
  failures += (got == ref) ? 0 : 1;

  fill_input(data, 5u);
  data[0] = 17;
  data[1] = 6;
  data[2] = 5;
  fprintf(out, "[earlyexit-test] case=early-return-candidate key=99 begin\n");
  got = hb_cfg_switch_early_exit(data, 99);
  fprintf(out,
          "[earlyexit-test] case=early-return-candidate kernel_done got=%d\n",
          got);
  ref = hb_cfg_switch_early_exit_ref(data, 99);
  fprintf(out, "[earlyexit-test] case=early-return-candidate ref_done ref=%d\n",
          ref);
  fprintf(out, "early-return-candidate got=%d ref=%d match=%s\n", got,
          ref, got == ref ? "yes" : "no");
  failures += (got == ref) ? 0 : 1;

  fprintf(out, "overall=%s\n", failures == 0 ? "pass" : "fail");

  if (out != stdout)
    fclose(out);
  return failures == 0 ? 0 : 2;
}
