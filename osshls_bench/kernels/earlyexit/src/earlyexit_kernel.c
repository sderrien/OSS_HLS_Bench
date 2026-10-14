#include <stdio.h>

#include "earlyexit.h"

#pragma HLS top
int hb_cfg_switch_early_exit(const int a[EARLYEXIT_N], int key) {
  int i = 0;
  int acc = 0;
  int exit_tag = 0;
  int cpt =0;
  #pragma HLS pipeline
  while (i < EARLYEXIT_N) {
    const int v = a[i];
    printf("[earlyexit-kernel] i=%d v=%d acc=%d key=%d\n", i, v, acc, key);

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

  return acc + i + exit_tag;
}
