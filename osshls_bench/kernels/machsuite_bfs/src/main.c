/*
 * Self-checking host harness for the MachSuite-style BFS benchmark.
 */

#include "bfs.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" void bfs(const uint8_t graph[BFS_NODE_COUNT][BFS_NODE_COUNT],
                     int32_t dist[BFS_NODE_COUNT]);
#endif

static void init_graph(uint8_t graph[BFS_NODE_COUNT][BFS_NODE_COUNT]) {
    static const uint8_t kGraph[BFS_NODE_COUNT][BFS_NODE_COUNT] = {
        {0,1,1,0,0,0,0,0},
        {1,0,1,1,0,0,0,0},
        {1,1,0,0,1,0,0,0},
        {0,1,0,0,1,1,0,0},
        {0,0,1,1,0,1,1,0},
        {0,0,0,1,1,0,0,1},
        {0,0,0,0,1,0,0,1},
        {0,0,0,0,0,1,1,0},
    };

    for (int i = 0; i < BFS_NODE_COUNT; ++i) {
        for (int j = 0; j < BFS_NODE_COUNT; ++j) {
            graph[i][j] = kGraph[i][j];
        }
    }
}

static void reference_bfs(const uint8_t graph[BFS_NODE_COUNT][BFS_NODE_COUNT],
                          int32_t dist[BFS_NODE_COUNT]) {
    int32_t queue[BFS_NODE_COUNT];
    uint8_t visited[BFS_NODE_COUNT];
    int32_t head = 0;
    int32_t tail = 0;

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
        for (int32_t v = 0; v < BFS_NODE_COUNT; ++v) {
            if (graph[u][v] && !visited[v]) {
                visited[v] = 1;
                dist[v] = next_dist;
                queue[tail++] = v;
            }
        }
    }
}

static int32_t checksum_dist(const int32_t dist[BFS_NODE_COUNT]) {
    int32_t sum = 0;
    for (int i = 0; i < BFS_NODE_COUNT; ++i) {
        sum = sum * 17 + dist[i];
    }
    return sum;
}

static int compare_dist(const int32_t lhs[BFS_NODE_COUNT],
                        const int32_t rhs[BFS_NODE_COUNT], int *bad_idx) {
    for (int i = 0; i < BFS_NODE_COUNT; ++i) {
        if (lhs[i] != rhs[i]) {
            *bad_idx = i;
            return 0;
        }
    }
    *bad_idx = -1;
    return 1;
}

int main(int argc, char **argv) {
    FILE *out = stdout;
    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            fprintf(stderr, "FAIL machsuite_bfs: cannot open output file\n");
            return 2;
        }
    }

    uint8_t graph[BFS_NODE_COUNT][BFS_NODE_COUNT];
    int32_t kernel[BFS_NODE_COUNT];
    int32_t reference[BFS_NODE_COUNT];

    init_graph(graph);
    bfs(graph, kernel);
    reference_bfs(graph, reference);

    int bad_idx = -1;
    int match = compare_dist(kernel, reference, &bad_idx);
    fprintf(out, "kernel_checksum=%d\n", checksum_dist(kernel));
    fprintf(out, "reference_checksum=%d\n", checksum_dist(reference));
    fprintf(out, "match=%s\n", match ? "yes" : "no");
    if (!match) {
        fprintf(out, "first_mismatch=%d\n", bad_idx);
    }

    if (out != stdout) {
        fclose(out);
    }
    return match ? 0 : 2;
}
