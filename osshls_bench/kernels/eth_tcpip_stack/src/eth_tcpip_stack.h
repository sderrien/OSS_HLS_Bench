#ifndef ETH_TCPIP_STACK_H
#define ETH_TCPIP_STACK_H

/*
 * Role:
 * Declare a compact stateful Ethernet-to-TCP/IP benchmark kernel.
 *
 * Goal:
 * Expose a self-contained packet-processing top function that exercises
 * Ethernet demux, ARP reply generation, IPv4/TCP handshake state, and payload
 * echo behavior while keeping the benchmark deterministic and easy to verify.
 */

#include <stdint.h>

enum {
    ETH_TCPIP_ETH_ADDR_BYTES = 6,
    ETH_TCPIP_IP_ADDR_BYTES = 4,
    ETH_TCPIP_FRAME_BYTES = 64,
    ETH_TCPIP_FRAME_COUNT = 4,
    ETH_TCPIP_TOTAL_BYTES = ETH_TCPIP_FRAME_BYTES * ETH_TCPIP_FRAME_COUNT,
    ETH_TCPIP_TOTAL_WORDS = ETH_TCPIP_TOTAL_BYTES / 4,
    ETH_TCPIP_PAYLOAD_BYTES = 4,
    ETH_TCPIP_STATE_WORDS = 16,
    ETH_TCPIP_ST_LOCAL_MAC0 = 0,
    ETH_TCPIP_ST_LOCAL_MAC1 = 1,
    ETH_TCPIP_ST_LOCAL_IP = 2,
    ETH_TCPIP_ST_LISTEN_PORT = 3,
    ETH_TCPIP_ST_SERVER_ISS = 4,
    ETH_TCPIP_ST_SERVER_SEQ = 5,
    ETH_TCPIP_ST_CLIENT_SEQ = 6,
    ETH_TCPIP_ST_CLIENT_PORT = 7,
    ETH_TCPIP_ST_CLIENT_MAC0 = 8,
    ETH_TCPIP_ST_CLIENT_MAC1 = 9,
    ETH_TCPIP_ST_CLIENT_IP = 10,
    ETH_TCPIP_ST_TCP_STATE = 11,
    ETH_TCPIP_ST_ARP_CACHED = 12,
};

typedef struct {
    uint32_t word[ETH_TCPIP_STATE_WORDS];
} eth_tcpip_state_t;

/*
 * Role:
 * Process a fixed batch of Ethernet frames through a tiny TCP/IP server state
 * machine.
 *
 * Goal:
 * Keep the kernel signature simple enough for the OpenHLS pipeline while still
 * carrying state across multiple packets in one invocation.
 */
__attribute__((annotate("hls.toplevel")))
void eth_tcpip_stack(const uint32_t rx[ETH_TCPIP_TOTAL_WORDS],
                     uint32_t tx[ETH_TCPIP_TOTAL_WORDS],
                     eth_tcpip_state_t state[1],
                     uint16_t tx_len[ETH_TCPIP_FRAME_COUNT]);

#endif
