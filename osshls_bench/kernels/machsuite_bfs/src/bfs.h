/*
 * MachSuite-style BFS benchmark adapter for OpenHLS.
 *
 * Role in the flow: define the fixed graph size and expose the top-level BFS
 * function used by the kernel and host harness.
 */
#ifndef MACHSUITE_BFS_H
#define MACHSUITE_BFS_H

#include <stdint.h>

#define BFS_NODE_COUNT 8

#ifdef __cplusplus
extern "C" {
#endif

void bfs(const uint8_t graph[BFS_NODE_COUNT][BFS_NODE_COUNT],
         int32_t dist[BFS_NODE_COUNT]);

#ifdef __cplusplus
}
#endif

#endif /* MACHSUITE_BFS_H */
