/*
 * Role:
 * Implement the stateful Ethernet-to-TCP/IP packet processor used by the
 * benchmark.
 *
 * Goal:
 * Model a compact datapath that decodes Ethernet frames, answers ARP, completes
 * a TCP handshake, and echoes one short payload while keeping the kernel friendly
 * to the OpenHLS frontend. Packet data is accessed through statically sized
 * arrays plus integer byte offsets, avoiding derived packet pointers in the
 * synthesized code.
 */

#include "eth_tcpip_stack.h"

static inline __attribute__((always_inline)) uint8_t pkt_get(const uint32_t buf[ETH_TCPIP_TOTAL_WORDS],
                                                             int off) {
    const uint32_t word = buf[off >> 2];
    const int shift = (3 - (off & 3)) << 3;
    return (uint8_t)(word >> shift);
}

static inline __attribute__((always_inline)) void pkt_set(uint32_t buf[ETH_TCPIP_TOTAL_WORDS],
                                                          int off,
                                                          uint8_t value) {
    const int word_idx = off >> 2;
    const int shift = (3 - (off & 3)) << 3;
    const uint32_t mask = 0xffu << shift;
    buf[word_idx] = (buf[word_idx] & ~mask) | ((uint32_t)value << shift);
}

static inline __attribute__((always_inline)) uint16_t read_be16_at(const uint32_t buf[ETH_TCPIP_TOTAL_WORDS],
                                                                   int off) {
    return (uint16_t)(((uint16_t)pkt_get(buf, off) << 8) | (uint16_t)pkt_get(buf, off + 1));
}

static inline __attribute__((always_inline)) uint32_t read_be32_at(const uint32_t buf[ETH_TCPIP_TOTAL_WORDS],
                                                                   int off) {
    return ((uint32_t)pkt_get(buf, off) << 24) | ((uint32_t)pkt_get(buf, off + 1) << 16) |
           ((uint32_t)pkt_get(buf, off + 2) << 8) | (uint32_t)pkt_get(buf, off + 3);
}

static inline __attribute__((always_inline)) void write_be16_at(uint32_t buf[ETH_TCPIP_TOTAL_WORDS],
                                                                int off,
                                                                uint16_t value) {
    pkt_set(buf, off, (uint8_t)(value >> 8));
    pkt_set(buf, off + 1, (uint8_t)(value & 0xffu));
}

static inline __attribute__((always_inline)) void write_be32_at(uint32_t buf[ETH_TCPIP_TOTAL_WORDS],
                                                                int off,
                                                                uint32_t value) {
    pkt_set(buf, off, (uint8_t)(value >> 24));
    pkt_set(buf, off + 1, (uint8_t)((value >> 16) & 0xffu));
    pkt_set(buf, off + 2, (uint8_t)((value >> 8) & 0xffu));
    pkt_set(buf, off + 3, (uint8_t)(value & 0xffu));
}

static inline __attribute__((always_inline)) void copy_bytes_at(uint32_t dst[ETH_TCPIP_TOTAL_WORDS],
                                                                int dst_off,
                                                                const uint32_t src[ETH_TCPIP_TOTAL_WORDS],
                                                                int src_off,
                                                                int count) {
    for (int i = 0; i < count; ++i) {
        pkt_set(dst, dst_off + i, pkt_get(src, src_off + i));
    }
}

static inline __attribute__((always_inline)) void copy_local_to_packet(uint32_t dst[ETH_TCPIP_TOTAL_WORDS],
                                                                       int dst_off,
                                                                       const uint8_t src[6],
                                                                       int count) {
    for (int i = 0; i < count; ++i) {
        pkt_set(dst, dst_off + i, src[i]);
    }
}

static inline __attribute__((always_inline)) void copy_packet_to_local(const uint32_t src[ETH_TCPIP_TOTAL_WORDS],
                                                                       int src_off,
                                                                       uint8_t dst[6],
                                                                       int count) {
    for (int i = 0; i < count; ++i) {
        dst[i] = pkt_get(src, src_off + i);
    }
}

static inline __attribute__((always_inline)) void zero_frame_at(uint32_t frame[ETH_TCPIP_TOTAL_WORDS],
                                                                int base) {
    for (int i = 0; i < ETH_TCPIP_FRAME_BYTES / 4; ++i) {
        frame[(base >> 2) + i] = 0u;
    }
}

static inline __attribute__((always_inline)) void load_mac_word_pair(const uint32_t state[ETH_TCPIP_STATE_WORDS],
                                                                     int base,
                                                                     uint8_t out[6]) {
    const uint32_t w0 = state[base + 0];
    const uint32_t w1 = state[base + 1];
    out[0] = (uint8_t)(w0 >> 24);
    out[1] = (uint8_t)(w0 >> 16);
    out[2] = (uint8_t)(w0 >> 8);
    out[3] = (uint8_t)w0;
    out[4] = (uint8_t)(w1 >> 24);
    out[5] = (uint8_t)(w1 >> 16);
}

static inline __attribute__((always_inline)) void store_mac_word_pair(uint32_t state[ETH_TCPIP_STATE_WORDS],
                                                                      int base,
                                                                      const uint8_t in[6]) {
    state[base + 0] = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                      ((uint32_t)in[2] << 8) | (uint32_t)in[3];
    state[base + 1] = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16);
}

static inline __attribute__((always_inline)) void load_ip_word(const uint32_t state[ETH_TCPIP_STATE_WORDS],
                                                               int base,
                                                               uint8_t out[4]) {
    const uint32_t w = state[base];
    out[0] = (uint8_t)(w >> 24);
    out[1] = (uint8_t)(w >> 16);
    out[2] = (uint8_t)(w >> 8);
    out[3] = (uint8_t)w;
}

static inline __attribute__((always_inline)) void store_ip_word(uint32_t state[ETH_TCPIP_STATE_WORDS],
                                                                int base,
                                                                const uint8_t in[4]) {
    state[base] = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                  ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static inline __attribute__((always_inline)) void copy_ip_state_to_packet(uint32_t dst[ETH_TCPIP_TOTAL_WORDS],
                                                                          int dst_off,
                                                                          const uint32_t state[ETH_TCPIP_STATE_WORDS],
                                                                          int state_base) {
    const uint32_t w = state[state_base];
    pkt_set(dst, dst_off + 0, (uint8_t)(w >> 24));
    pkt_set(dst, dst_off + 1, (uint8_t)(w >> 16));
    pkt_set(dst, dst_off + 2, (uint8_t)(w >> 8));
    pkt_set(dst, dst_off + 3, (uint8_t)w);
}

static inline __attribute__((always_inline)) uint16_t checksum16_at(const uint32_t data[ETH_TCPIP_TOTAL_WORDS],
                                                                    int base,
                                                                    int length) {
    uint32_t sum = 0;
    for (int i = 0; i < length; i += 2) {
        uint16_t hi = (uint16_t)pkt_get(data, base + i) << 8;
        uint16_t lo = 0u;
        if (i + 1 < length) {
            lo = pkt_get(data, base + i + 1);
        }
        sum += (uint32_t)(hi | lo);
        while (sum >> 16) {
            sum = (sum & 0xffffu) + (sum >> 16);
        }
    }
    return (uint16_t)(~sum & 0xffffu);
}

static inline __attribute__((always_inline)) uint16_t ipv4_header_checksum_at(const uint32_t packet[ETH_TCPIP_TOTAL_WORDS],
                                                                              int ip_base) {
    return checksum16_at(packet, ip_base, 20);
}

static inline __attribute__((always_inline)) uint16_t tcp_checksum_at(const uint32_t packet[ETH_TCPIP_TOTAL_WORDS],
                                                                      int ip_base,
                                                                      int tcp_base,
                                                                      int tcp_len) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; ++i) {
        sum += (uint32_t)pkt_get(packet, ip_base + 12 + i);
        sum += (uint32_t)pkt_get(packet, ip_base + 16 + i);
    }
    sum += 6u;
    sum += (uint32_t)tcp_len;

    for (int i = 0; i < tcp_len; i += 2) {
        uint16_t hi = (uint16_t)pkt_get(packet, tcp_base + i) << 8;
        uint16_t lo = 0u;
        if (i + 1 < tcp_len) {
            lo = pkt_get(packet, tcp_base + i + 1);
        }
        sum += (uint32_t)(hi | lo);
        while (sum >> 16) {
            sum = (sum & 0xffffu) + (sum >> 16);
        }
    }
    return (uint16_t)(~sum & 0xffffu);
}

static inline __attribute__((always_inline)) void build_arp_reply(const uint32_t rx[ETH_TCPIP_TOTAL_WORDS],
                                                                  int in_base,
                                                                  uint32_t tx[ETH_TCPIP_TOTAL_WORDS],
                                                                  int out_base,
                                                                  eth_tcpip_state_t state[1]) {
    uint8_t local_mac[6];
    uint8_t local_ip[4];
    uint8_t client_mac[6];
    uint8_t client_ip[4];

    load_mac_word_pair(state[0].word, ETH_TCPIP_ST_LOCAL_MAC0, local_mac);
    load_ip_word(state[0].word, ETH_TCPIP_ST_LOCAL_IP, local_ip);
    zero_frame_at(tx, out_base);
    copy_bytes_at(tx, out_base + 0, rx, in_base + 6, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_local_to_packet(tx, out_base + 6, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    write_be16_at(tx, out_base + 12, 0x0806u);
    write_be16_at(tx, out_base + 14, 1u);
    write_be16_at(tx, out_base + 16, 0x0800u);
    pkt_set(tx, out_base + 18, 6u);
    pkt_set(tx, out_base + 19, 4u);
    write_be16_at(tx, out_base + 20, 2u);
    copy_local_to_packet(tx, out_base + 22, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_local_to_packet(tx, out_base + 28, local_ip, ETH_TCPIP_IP_ADDR_BYTES);
    copy_bytes_at(tx, out_base + 32, rx, in_base + 22, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes_at(tx, out_base + 38, rx, in_base + 28, ETH_TCPIP_IP_ADDR_BYTES);

    copy_packet_to_local(rx, in_base + 22, client_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_packet_to_local(rx, in_base + 28, client_ip, ETH_TCPIP_IP_ADDR_BYTES);
    store_mac_word_pair(state[0].word, ETH_TCPIP_ST_CLIENT_MAC0, client_mac);
    store_ip_word(state[0].word, ETH_TCPIP_ST_CLIENT_IP, client_ip);
    state[0].word[ETH_TCPIP_ST_ARP_CACHED] = 1u;
}

static inline __attribute__((always_inline)) void build_tcp_reply(const uint32_t rx[ETH_TCPIP_TOTAL_WORDS],
                                                                  int in_base,
                                                                  uint32_t tx[ETH_TCPIP_TOTAL_WORDS],
                                                                  int out_base,
                                                                  eth_tcpip_state_t state[1],
                                                                  uint8_t flags,
                                                                  int payload_base,
                                                                  uint16_t payload_bytes,
                                                                  uint32_t seq,
                                                                  uint32_t ack) {
    const uint16_t ip_len = (uint16_t)(20u + 20u + payload_bytes);
    const uint16_t tcp_len = (uint16_t)(20u + payload_bytes);
    const int ip_base = out_base + 14;
    const int tcp_base = out_base + 34;
    uint8_t local_mac[6];
    uint8_t client_ip[4];

    load_mac_word_pair(state[0].word, ETH_TCPIP_ST_LOCAL_MAC0, local_mac);
    load_ip_word(state[0].word, ETH_TCPIP_ST_CLIENT_IP, client_ip);

    zero_frame_at(tx, out_base);
    copy_bytes_at(tx, out_base + 0, rx, in_base + 6, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_local_to_packet(tx, out_base + 6, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    write_be16_at(tx, out_base + 12, 0x0800u);

    pkt_set(tx, ip_base + 0, 0x45u);
    pkt_set(tx, ip_base + 1, 0u);
    write_be16_at(tx, ip_base + 2, ip_len);
    write_be16_at(tx, ip_base + 4, 0x1111u);
    write_be16_at(tx, ip_base + 6, 0x4000u);
    pkt_set(tx, ip_base + 8, 64u);
    pkt_set(tx, ip_base + 9, 6u);
    pkt_set(tx, ip_base + 10, 0u);
    pkt_set(tx, ip_base + 11, 0u);
    copy_ip_state_to_packet(tx, ip_base + 12, state[0].word, ETH_TCPIP_ST_LOCAL_IP);
    copy_local_to_packet(tx, ip_base + 16, client_ip, ETH_TCPIP_IP_ADDR_BYTES);
    write_be16_at(tx, ip_base + 10, ipv4_header_checksum_at(tx, ip_base));

    write_be16_at(tx, tcp_base + 0, (uint16_t)state[0].word[ETH_TCPIP_ST_LISTEN_PORT]);
    write_be16_at(tx, tcp_base + 2, (uint16_t)state[0].word[ETH_TCPIP_ST_CLIENT_PORT]);
    write_be32_at(tx, tcp_base + 4, seq);
    write_be32_at(tx, tcp_base + 8, ack);
    write_be16_at(tx, tcp_base + 12, (uint16_t)(0x5000u | flags));
    write_be16_at(tx, tcp_base + 14, 0x4000u);
    pkt_set(tx, tcp_base + 16, 0u);
    pkt_set(tx, tcp_base + 17, 0u);
    pkt_set(tx, tcp_base + 18, 0u);
    pkt_set(tx, tcp_base + 19, 0u);
    for (uint16_t i = 0; i < payload_bytes; ++i) {
        pkt_set(tx, tcp_base + 20 + i, pkt_get(rx, payload_base + i));
    }
    write_be16_at(tx, tcp_base + 16, tcp_checksum_at(tx, ip_base, tcp_base, tcp_len));
}

static inline __attribute__((always_inline)) void process_frame(const uint32_t rx[ETH_TCPIP_TOTAL_WORDS],
                                                                int in_base,
                                                                uint32_t tx[ETH_TCPIP_TOTAL_WORDS],
                                                                int out_base,
                                                                eth_tcpip_state_t state[1]) {
    const uint16_t eth_type = read_be16_at(rx, in_base + 12);
    if (eth_type == 0x0806u) {
        const uint16_t arp_op = read_be16_at(rx, in_base + 20);
        uint8_t local_ip[4];
        load_ip_word(state[0].word, ETH_TCPIP_ST_LOCAL_IP, local_ip);
        if (arp_op == 1u && pkt_get(rx, in_base + 38) == local_ip[0] &&
            pkt_get(rx, in_base + 39) == local_ip[1] &&
            pkt_get(rx, in_base + 40) == local_ip[2] &&
            pkt_get(rx, in_base + 41) == local_ip[3]) {
            build_arp_reply(rx, in_base, tx, out_base, state);
        }
        return;
    }

    if (eth_type != 0x0800u) {
        return;
    }

    const int ip_base = in_base + 14;
    if (pkt_get(rx, ip_base + 9) != 6u) {
        return;
    }
    uint8_t local_ip[4];
    load_ip_word(state[0].word, ETH_TCPIP_ST_LOCAL_IP, local_ip);
    if (pkt_get(rx, ip_base + 16) != local_ip[0] || pkt_get(rx, ip_base + 17) != local_ip[1] ||
        pkt_get(rx, ip_base + 18) != local_ip[2] || pkt_get(rx, ip_base + 19) != local_ip[3]) {
        return;
    }

    const int tcp_base = in_base + 34;
    const uint16_t total_len = read_be16_at(rx, ip_base + 2);
    const uint16_t src_port = read_be16_at(rx, tcp_base + 0);
    const uint16_t dst_port = read_be16_at(rx, tcp_base + 2);
    if (dst_port != (uint16_t)state[0].word[ETH_TCPIP_ST_LISTEN_PORT]) {
        return;
    }

    const uint8_t flags = (uint8_t)read_be16_at(rx, tcp_base + 12);
    const uint32_t seq = read_be32_at(rx, tcp_base + 4);
    const uint32_t ack = read_be32_at(rx, tcp_base + 8);
    const uint16_t payload_bytes = (uint16_t)(total_len - 20u - 20u);
    const int payload_base = in_base + 54;
    uint8_t client_mac[6];
    uint8_t client_ip[4];

    copy_packet_to_local(rx, in_base + 6, client_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_packet_to_local(rx, ip_base + 12, client_ip, ETH_TCPIP_IP_ADDR_BYTES);
    store_mac_word_pair(state[0].word, ETH_TCPIP_ST_CLIENT_MAC0, client_mac);
    store_ip_word(state[0].word, ETH_TCPIP_ST_CLIENT_IP, client_ip);
    state[0].word[ETH_TCPIP_ST_CLIENT_PORT] = (uint32_t)src_port;

    if ((flags & 0x02u) != 0u && state[0].word[ETH_TCPIP_ST_TCP_STATE] == 0u) {
        state[0].word[ETH_TCPIP_ST_CLIENT_SEQ] = seq;
        state[0].word[ETH_TCPIP_ST_TCP_STATE] = 1u;
        build_tcp_reply(rx, in_base, tx, out_base, state, 0x12u, payload_base, 0u,
                        state[0].word[ETH_TCPIP_ST_SERVER_ISS],
                        state[0].word[ETH_TCPIP_ST_CLIENT_SEQ] + 1u);
        state[0].word[ETH_TCPIP_ST_SERVER_SEQ] = state[0].word[ETH_TCPIP_ST_SERVER_ISS] + 1u;
        return;
    }

    if ((flags & 0x10u) != 0u && state[0].word[ETH_TCPIP_ST_TCP_STATE] == 1u &&
        ack == state[0].word[ETH_TCPIP_ST_SERVER_SEQ]) {
        state[0].word[ETH_TCPIP_ST_TCP_STATE] = 2u;
        build_tcp_reply(rx, in_base, tx, out_base, state, 0x10u, payload_base, 0u,
                        state[0].word[ETH_TCPIP_ST_SERVER_SEQ],
                        state[0].word[ETH_TCPIP_ST_CLIENT_SEQ] + 1u);
        return;
    }

    if (state[0].word[ETH_TCPIP_ST_TCP_STATE] == 2u && payload_bytes > 0u) {
        build_tcp_reply(rx, in_base, tx, out_base, state, 0x18u, payload_base, payload_bytes,
                        state[0].word[ETH_TCPIP_ST_SERVER_SEQ], seq + payload_bytes);
        state[0].word[ETH_TCPIP_ST_SERVER_SEQ] += payload_bytes;
        return;
    }
}

void eth_tcpip_stack(const uint32_t rx[ETH_TCPIP_TOTAL_WORDS],
                     uint32_t tx[ETH_TCPIP_TOTAL_WORDS],
                     eth_tcpip_state_t state[1],
                     uint16_t tx_len[ETH_TCPIP_FRAME_COUNT]) {
    for (int pkt = 0; pkt < ETH_TCPIP_FRAME_COUNT; ++pkt) {
        const int in_base = pkt * ETH_TCPIP_FRAME_BYTES;
        const int out_base = pkt * ETH_TCPIP_FRAME_BYTES;
        tx_len[pkt] = 0u;
        zero_frame_at(tx, out_base);
        process_frame(rx, in_base, tx, out_base, state);

        if (read_be16_at(tx, out_base + 12) == 0x0806u) {
            tx_len[pkt] = 42u;
        } else if (read_be16_at(tx, out_base + 12) == 0x0800u) {
            tx_len[pkt] = (uint16_t)(14u + read_be16_at(tx, out_base + 16));
        }
    }
}
