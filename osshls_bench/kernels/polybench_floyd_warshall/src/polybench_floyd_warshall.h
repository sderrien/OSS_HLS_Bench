/*
 * OHLS benchmark adapter for PolyBench/C floyd-warshall.
 *
 * Role in the flow: define a static 32-node all-pairs-shortest-path workload
 * and declare the HLS top function adapted from PolyBench/C 4.2.1 medley.
 */
#ifndef POLYBENCH_FLOYD_WARSHALL_H
#define POLYBENCH_FLOYD_WARSHALL_H

#define FLOYD_WARSHALL_N 32

#ifdef __cplusplus
extern "C" {
#endif

/* Compute shortest paths in-place using the Floyd-Warshall recurrence. */
void polybench_floyd_warshall(int path[FLOYD_WARSHALL_N][FLOYD_WARSHALL_N]);

#ifdef __cplusplus
}
#endif

#endif /* POLYBENCH_FLOYD_WARSHALL_H */
