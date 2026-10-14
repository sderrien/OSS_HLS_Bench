/*
 * MachSuite-style BFS kernel adapted for OpenHLS.
 *
 * Why: breadth-first search stresses queue management, frontier expansion, and
 * loop-carried control. What: compute shortest-path distances from node 0 over a
 * tiny deterministic graph. How: use a bounded queue and explicit visited state.
 */

#include "bfs.h"

__attribute__((annotate("hls.toplevel")))
void bfs(const uint8_t graph[BFS_NODE_COUNT][BFS_NODE_COUNT],
         int32_t dist[BFS_NODE_COUNT]) {
    int32_t queue[BFS_NODE_COUNT];
    uint8_t visited[BFS_NODE_COUNT];
    int32_t head = 0;
    int32_t tail = 0;

    #pragma HLS pipeline
    for (int i = 0; i < BFS_NODE_COUNT; ++i) {
        dist[i] = -1;
        visited[i] = 0;
    }

    dist[0] = 0;
    visited[0] = 1;
    queue[tail++] = 0;

    while (head < tail) {
        int32_t u = queue[head++];
        int32_t next_dist = dist[u] + 1;
        #pragma HLS pipeline
        for (int32_t v = 0; v < BFS_NODE_COUNT; ++v) {
            if (graph[u][v] && !visited[v]) {
                visited[v] = 1;
                dist[v] = next_dist;
                queue[tail++] = v;
            }
        }
    }
}
