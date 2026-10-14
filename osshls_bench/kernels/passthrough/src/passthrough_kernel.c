/*
 * Passthrough benchmark kernel.
 *
 * Role in the benchmark flow:
 * Provide a memory-to-memory copy kernel with one input array and one output
 * array.  The kernel is intentionally simple so each HLS tool can expose its
 * baseline array interface, loop scheduling, and memory bandwidth behavior on a
 * common 1024-sample integer workload.
 *
 * How it works:
 * The top-level function reads each element from data_in and writes the same
 * value to data_out.  HLS pragmas identify the top function and request simple
 * single-port RAM-style array interfaces, matching the style used by the small
 * existing kernels in this benchmark set.
 */

#include "passthrough.h"

#pragma HLS top
/*
 * Copy one full input array into the output array.
 *
 * Role:
 * Serve as the HLS top function measured by every benchmark tool.
 *
 * Goal:
 * Exercise array reads and writes with no arithmetic side effects, making it easy
 * to compare baseline memory-interface and loop-control overheads across tools.
 */
void passthrough(const int data_in[PASSTHROUGH_N], int data_out[PASSTHROUGH_N]) {
Loop_copy:
    #pragma HLS pipeline 
  for (int i = 0; i < PASSTHROUGH_N; ++i) {
    data_out[i] = data_in[i];
  }
}
