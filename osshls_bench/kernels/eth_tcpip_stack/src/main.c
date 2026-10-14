/*
 * Role:
 * Provide a deterministic, self-checking host harness for the network-stack
 * benchmark.
 *
 * Goal:
 * Feed a scripted Ethernet/ARP/TCP session into the kernel, verify each
 * emitted response against an independent software reference, and print a
 * compact PASS/FAIL summary that works across the OpenHLS verification flow.
 */

#include "eth_tcpip_stack.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" void eth_tcpip_stack(const uint32_t rx[ETH_TCPIP_TOTAL_WORDS],
                                  uint32_t tx[ETH_TCPIP_TOTAL_WORDS],
                                  eth_tcpip_state_t state[1],
                                  uint16_t tx_len[ETH_TCPIP_FRAME_COUNT]);
#endif

static inline __attribute__((always_inline)) uint16_t read_be16(const uint8_t *p) {
    return __builtin_bswap16(*(const uint16_t *)p);
}

static inline __attribute__((always_inline)) uint32_t read_be32(const uint8_t *p) {
    return __builtin_bswap32(*(const uint32_t *)p);
}

static inline __attribute__((always_inline)) void write_be16(uint8_t *p, uint16_t v) {
    *(uint16_t *)p = __builtin_bswap16(v);
}

static inline __attribute__((always_inline)) void write_be32(uint8_t *p, uint32_t v) {
    *(uint32_t *)p = __builtin_bswap32(v);
}

static inline __attribute__((always_inline)) void copy_bytes(uint8_t *dst, const uint8_t *src,
                                                             int count) {
    for (int i = 0; i < count; ++i) {
        dst[i] = src[i];
    }
}

static inline __attribute__((always_inline)) void zero_frame(uint8_t *frame) {
    for (int i = 0; i < ETH_TCPIP_FRAME_BYTES; ++i) {
        frame[i] = 0;
    }
}

static inline __attribute__((always_inline)) void pack_packet_words(const uint8_t bytes[ETH_TCPIP_TOTAL_BYTES],
                                                                    uint32_t words[ETH_TCPIP_TOTAL_WORDS]) {
    for (int i = 0; i < ETH_TCPIP_TOTAL_WORDS; ++i) {
        const int off = i * 4;
        words[i] = ((uint32_t)bytes[off + 0] << 24) | ((uint32_t)bytes[off + 1] << 16) |
                   ((uint32_t)bytes[off + 2] << 8) | (uint32_t)bytes[off + 3];
    }
}

static inline __attribute__((always_inline)) void unpack_packet_words(const uint32_t words[ETH_TCPIP_TOTAL_WORDS],
                                                                      uint8_t bytes[ETH_TCPIP_TOTAL_BYTES]) {
    for (int i = 0; i < ETH_TCPIP_TOTAL_WORDS; ++i) {
        const int off = i * 4;
        const uint32_t w = words[i];
        bytes[off + 0] = (uint8_t)(w >> 24);
        bytes[off + 1] = (uint8_t)(w >> 16);
        bytes[off + 2] = (uint8_t)(w >> 8);
        bytes[off + 3] = (uint8_t)w;
    }
}

static inline __attribute__((always_inline)) void load_mac_word_pair(const uint32_t *state,
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

static inline __attribute__((always_inline)) void store_mac_word_pair(uint32_t *state,
                                                                      int base,
                                                                      const uint8_t in[6]) {
    state[base + 0] = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                      ((uint32_t)in[2] << 8) | (uint32_t)in[3];
    state[base + 1] = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16);
}

static inline __attribute__((always_inline)) void load_ip_word(const uint32_t *state,
                                                               int base,
                                                               uint8_t out[4]) {
    const uint32_t w = state[base];
    out[0] = (uint8_t)(w >> 24);
    out[1] = (uint8_t)(w >> 16);
    out[2] = (uint8_t)(w >> 8);
    out[3] = (uint8_t)w;
}

static inline __attribute__((always_inline)) void store_ip_word(uint32_t *state,
                                                                int base,
                                                                const uint8_t in[4]) {
    state[base] = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                  ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static inline __attribute__((always_inline)) void store_be16_aligned(uint8_t *ptr,
                                                                      uint16_t value) {
    write_be16(ptr, value);
}

static inline __attribute__((always_inline)) uint16_t checksum16(const uint8_t *data,
                                                                 int length) {
    uint32_t sum = 0;
    for (int i = 0; i < length; i += 2) {
        uint16_t hi = (uint16_t)data[i] << 8;
        uint16_t lo = 0;
        if (i + 1 < length) {
            lo = data[i + 1];
        }
        sum += (uint32_t)(hi | lo);
        while (sum >> 16) {
            sum = (sum & 0xffffu) + (sum >> 16);
        }
    }
    return (uint16_t)(~sum & 0xffffu);
}

static inline __attribute__((always_inline)) uint16_t ipv4_header_checksum(const uint8_t *ip) {
    return checksum16(ip, 20);
}

static inline __attribute__((always_inline)) uint16_t tcp_checksum(const uint8_t *ip,
                                                                   const uint8_t *tcp,
                                                                   int tcp_len) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; ++i) {
        sum += (uint32_t)ip[12 + i];
        sum += (uint32_t)ip[16 + i];
    }
    sum += 6u;
    sum += (uint32_t)tcp_len;
    for (int i = 0; i < tcp_len; i += 2) {
        uint16_t hi = (uint16_t)tcp[i] << 8;
        uint16_t lo = 0;
        if (i + 1 < tcp_len) {
            lo = tcp[i + 1];
        }
        sum += (uint32_t)(hi | lo);
        while (sum >> 16) {
            sum = (sum & 0xffffu) + (sum >> 16);
        }
    }
    return (uint16_t)(~sum & 0xffffu);
}

static inline __attribute__((always_inline)) void build_arp_request(uint8_t *frame) {
    zero_frame(frame);
    const uint8_t dst_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu};
    const uint8_t src_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u};
    const uint8_t src_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 2u};
    const uint8_t target_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 1u};

    copy_bytes(frame + 0, dst_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(frame + 6, src_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    store_be16_aligned(frame + 12, 0x0806u);
    write_be16(frame + 14, 1u);
    write_be16(frame + 16, 0x0800u);
    frame[18] = 6u;
    frame[19] = 4u;
    write_be16(frame + 20, 1u);
    copy_bytes(frame + 22, src_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(frame + 28, src_ip, ETH_TCPIP_IP_ADDR_BYTES);
    for (int i = 0; i < ETH_TCPIP_ETH_ADDR_BYTES; ++i) {
        frame[32 + i] = 0u;
    }
    copy_bytes(frame + 38, target_ip, ETH_TCPIP_IP_ADDR_BYTES);
}

static inline __attribute__((always_inline)) void build_tcp_syn(uint8_t *frame) {
    zero_frame(frame);
    const uint8_t dst_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};
    const uint8_t src_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u};
    const uint8_t src_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 2u};
    const uint8_t dst_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 1u};

    copy_bytes(frame + 0, dst_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(frame + 6, src_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    store_be16_aligned(frame + 12, 0x0800u);
    uint8_t *ip = frame + 14;
    ip[0] = 0x45u;
    ip[1] = 0u;
    write_be16(ip + 2, 40u);
    write_be16(ip + 4, 0x2222u);
    write_be16(ip + 6, 0x4000u);
    ip[8] = 64u;
    ip[9] = 6u;
    copy_bytes(ip + 12, src_ip, ETH_TCPIP_IP_ADDR_BYTES);
    copy_bytes(ip + 16, dst_ip, ETH_TCPIP_IP_ADDR_BYTES);
    write_be16(ip + 10, ipv4_header_checksum(ip));

    uint8_t *tcp = frame + 34;
    write_be16(tcp + 0, 40000u);
    write_be16(tcp + 2, 80u);
    write_be32(tcp + 4, 0x2000u);
    write_be32(tcp + 8, 0u);
    store_be16_aligned(tcp + 12, 0x5002u);
    write_be16(tcp + 14, 0x4000u);
    write_be16(tcp + 16, tcp_checksum(ip, tcp, 20));
}

static inline __attribute__((always_inline)) void build_tcp_ack(uint8_t *frame) {
    zero_frame(frame);
    const uint8_t dst_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};
    const uint8_t src_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u};
    const uint8_t src_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 2u};
    const uint8_t dst_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 1u};

    copy_bytes(frame + 0, dst_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(frame + 6, src_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    store_be16_aligned(frame + 12, 0x0800u);
    uint8_t *ip = frame + 14;
    ip[0] = 0x45u;
    ip[1] = 0u;
    write_be16(ip + 2, 40u);
    write_be16(ip + 4, 0x2223u);
    write_be16(ip + 6, 0x4000u);
    ip[8] = 64u;
    ip[9] = 6u;
    copy_bytes(ip + 12, src_ip, ETH_TCPIP_IP_ADDR_BYTES);
    copy_bytes(ip + 16, dst_ip, ETH_TCPIP_IP_ADDR_BYTES);
    write_be16(ip + 10, ipv4_header_checksum(ip));

    uint8_t *tcp = frame + 34;
    write_be16(tcp + 0, 40000u);
    write_be16(tcp + 2, 80u);
    write_be32(tcp + 4, 0x2001u);
    write_be32(tcp + 8, 0x1001u);
    store_be16_aligned(tcp + 12, 0x5010u);
    write_be16(tcp + 14, 0x4000u);
    write_be16(tcp + 16, tcp_checksum(ip, tcp, 20));
}

static inline __attribute__((always_inline)) void build_tcp_data(uint8_t *frame) {
    zero_frame(frame);
    const uint8_t dst_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};
    const uint8_t src_mac[ETH_TCPIP_ETH_ADDR_BYTES] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u};
    const uint8_t src_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 2u};
    const uint8_t dst_ip[ETH_TCPIP_IP_ADDR_BYTES] = {10u, 0u, 0u, 1u};
    const uint8_t payload[ETH_TCPIP_PAYLOAD_BYTES] = {'p', 'i', 'n', 'g'};

    copy_bytes(frame + 0, dst_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(frame + 6, src_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    store_be16_aligned(frame + 12, 0x0800u);
    uint8_t *ip = frame + 14;
    ip[0] = 0x45u;
    ip[1] = 0u;
    write_be16(ip + 2, 44u);
    write_be16(ip + 4, 0x2224u);
    write_be16(ip + 6, 0x4000u);
    ip[8] = 64u;
    ip[9] = 6u;
    copy_bytes(ip + 12, src_ip, ETH_TCPIP_IP_ADDR_BYTES);
    copy_bytes(ip + 16, dst_ip, ETH_TCPIP_IP_ADDR_BYTES);
    write_be16(ip + 10, ipv4_header_checksum(ip));

    uint8_t *tcp = frame + 34;
    write_be16(tcp + 0, 40000u);
    write_be16(tcp + 2, 80u);
    write_be32(tcp + 4, 0x2001u);
    write_be32(tcp + 8, 0x1001u);
    store_be16_aligned(tcp + 12, 0x5018u);
    write_be16(tcp + 14, 0x4000u);
    copy_bytes(tcp + 20, payload, ETH_TCPIP_PAYLOAD_BYTES);
    write_be16(tcp + 16, tcp_checksum(ip, tcp, 24));
}

static inline __attribute__((always_inline)) void reference_process_arp(const uint8_t *in,
                                                                        uint8_t *out,
                                                                        eth_tcpip_state_t *state) {
    uint8_t local_mac[6];
    uint8_t local_ip[4];
    uint8_t client_mac[6];
    uint8_t client_ip[4];

    load_mac_word_pair(state->word, ETH_TCPIP_ST_LOCAL_MAC0, local_mac);
    load_ip_word(state->word, ETH_TCPIP_ST_LOCAL_IP, local_ip);
    zero_frame(out);
    copy_bytes(out + 0, in + 6, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(out + 6, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    store_be16_aligned(out + 12, 0x0806u);
    write_be16(out + 14, 1u);
    write_be16(out + 16, 0x0800u);
    out[18] = 6u;
    out[19] = 4u;
    write_be16(out + 20, 2u);
    copy_bytes(out + 22, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(out + 28, local_ip, ETH_TCPIP_IP_ADDR_BYTES);
    copy_bytes(out + 32, in + 22, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(out + 38, in + 28, ETH_TCPIP_IP_ADDR_BYTES);
    copy_bytes(client_mac, in + 22, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(client_ip, in + 28, ETH_TCPIP_IP_ADDR_BYTES);
    store_mac_word_pair(state->word, ETH_TCPIP_ST_CLIENT_MAC0, client_mac);
    store_ip_word(state->word, ETH_TCPIP_ST_CLIENT_IP, client_ip);
    state->word[ETH_TCPIP_ST_ARP_CACHED] = 1u;
}

static inline __attribute__((always_inline)) void reference_process_tcp(const uint8_t *in,
                                                                        uint8_t *out,
                                                                        eth_tcpip_state_t *state) {
    const uint8_t *ip = in + 14;
    const uint8_t *tcp = in + 34;
    const uint16_t total_len = read_be16(ip + 2);
    const uint16_t payload_bytes = (uint16_t)(total_len - 40u);
    const uint32_t seq = read_be32(tcp + 4);
    const uint32_t ack = read_be32(tcp + 8);
    const uint8_t flags = (uint8_t)read_be16(tcp + 12);
    const uint8_t *payload = in + 54;
    uint8_t client_mac[6];
    uint8_t client_ip[4];
    uint8_t local_mac[6];

    load_mac_word_pair(state->word, ETH_TCPIP_ST_LOCAL_MAC0, local_mac);
    copy_bytes(client_mac, in + 6, ETH_TCPIP_ETH_ADDR_BYTES);
    copy_bytes(client_ip, ip + 12, ETH_TCPIP_IP_ADDR_BYTES);
    store_mac_word_pair(state->word, ETH_TCPIP_ST_CLIENT_MAC0, client_mac);
    store_ip_word(state->word, ETH_TCPIP_ST_CLIENT_IP, client_ip);
    state->word[ETH_TCPIP_ST_CLIENT_PORT] = (uint32_t)read_be16(tcp + 0);

    if ((flags & 0x02u) != 0u && state->word[ETH_TCPIP_ST_TCP_STATE] == 0u) {
        state->word[ETH_TCPIP_ST_CLIENT_SEQ] = seq;
        state->word[ETH_TCPIP_ST_TCP_STATE] = 1u;
        zero_frame(out);
        copy_bytes(out + 0, in + 6, ETH_TCPIP_ETH_ADDR_BYTES);
        copy_bytes(out + 6, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
        store_be16_aligned(out + 12, 0x0800u);
        uint8_t *rip = out + 14;
        rip[0] = 0x45u;
        rip[1] = 0u;
        write_be16(rip + 2, 40u);
        write_be16(rip + 4, 0x1111u);
        write_be16(rip + 6, 0x4000u);
        rip[8] = 64u;
        rip[9] = 6u;
        load_ip_word(state->word, ETH_TCPIP_ST_LOCAL_IP, rip + 12);
        copy_bytes(rip + 16, client_ip, ETH_TCPIP_IP_ADDR_BYTES);
        write_be16(rip + 10, ipv4_header_checksum(rip));
        uint8_t *rtcp = out + 34;
        write_be16(rtcp + 0, (uint16_t)state->word[ETH_TCPIP_ST_LISTEN_PORT]);
        write_be16(rtcp + 2, (uint16_t)state->word[ETH_TCPIP_ST_CLIENT_PORT]);
        write_be32(rtcp + 4, state->word[ETH_TCPIP_ST_SERVER_ISS]);
        write_be32(rtcp + 8, state->word[ETH_TCPIP_ST_CLIENT_SEQ] + 1u);
        store_be16_aligned(rtcp + 12, 0x5012u);
        write_be16(rtcp + 14, 0x4000u);
        write_be16(rtcp + 16, tcp_checksum(rip, rtcp, 20));
        state->word[ETH_TCPIP_ST_SERVER_SEQ] = state->word[ETH_TCPIP_ST_SERVER_ISS] + 1u;
        return;
    }

    if ((flags & 0x10u) != 0u && state->word[ETH_TCPIP_ST_TCP_STATE] == 1u &&
        ack == state->word[ETH_TCPIP_ST_SERVER_SEQ]) {
        state->word[ETH_TCPIP_ST_TCP_STATE] = 2u;
        zero_frame(out);
        copy_bytes(out + 0, in + 6, ETH_TCPIP_ETH_ADDR_BYTES);
        copy_bytes(out + 6, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
        store_be16_aligned(out + 12, 0x0800u);
        uint8_t *rip = out + 14;
        rip[0] = 0x45u;
        rip[1] = 0u;
        write_be16(rip + 2, 40u);
        write_be16(rip + 4, 0x1111u);
        write_be16(rip + 6, 0x4000u);
        rip[8] = 64u;
        rip[9] = 6u;
        load_ip_word(state->word, ETH_TCPIP_ST_LOCAL_IP, rip + 12);
        copy_bytes(rip + 16, client_ip, ETH_TCPIP_IP_ADDR_BYTES);
        write_be16(rip + 10, ipv4_header_checksum(rip));
        uint8_t *rtcp = out + 34;
        write_be16(rtcp + 0, (uint16_t)state->word[ETH_TCPIP_ST_LISTEN_PORT]);
        write_be16(rtcp + 2, (uint16_t)state->word[ETH_TCPIP_ST_CLIENT_PORT]);
        write_be32(rtcp + 4, state->word[ETH_TCPIP_ST_SERVER_SEQ]);
        write_be32(rtcp + 8, state->word[ETH_TCPIP_ST_CLIENT_SEQ] + 1u);
        store_be16_aligned(rtcp + 12, 0x5010u);
        write_be16(rtcp + 14, 0x4000u);
        write_be16(rtcp + 16, tcp_checksum(rip, rtcp, 20));
        return;
    }

    if (state->word[ETH_TCPIP_ST_TCP_STATE] == 2u && payload_bytes > 0u) {
        zero_frame(out);
        copy_bytes(out + 0, in + 6, ETH_TCPIP_ETH_ADDR_BYTES);
        copy_bytes(out + 6, local_mac, ETH_TCPIP_ETH_ADDR_BYTES);
        store_be16_aligned(out + 12, 0x0800u);
        uint8_t *rip = out + 14;
        rip[0] = 0x45u;
        rip[1] = 0u;
        write_be16(rip + 2, (uint16_t)(40u + payload_bytes));
        write_be16(rip + 4, 0x1111u);
        write_be16(rip + 6, 0x4000u);
        rip[8] = 64u;
        rip[9] = 6u;
        load_ip_word(state->word, ETH_TCPIP_ST_LOCAL_IP, rip + 12);
        copy_bytes(rip + 16, client_ip, ETH_TCPIP_IP_ADDR_BYTES);
        write_be16(rip + 10, ipv4_header_checksum(rip));
        uint8_t *rtcp = out + 34;
        write_be16(rtcp + 0, (uint16_t)state->word[ETH_TCPIP_ST_LISTEN_PORT]);
        write_be16(rtcp + 2, (uint16_t)state->word[ETH_TCPIP_ST_CLIENT_PORT]);
        write_be32(rtcp + 4, state->word[ETH_TCPIP_ST_SERVER_SEQ]);
        write_be32(rtcp + 8, seq + payload_bytes);
        store_be16_aligned(rtcp + 12, 0x5018u);
        write_be16(rtcp + 14, 0x4000u);
        copy_bytes(rtcp + 20, payload, ETH_TCPIP_PAYLOAD_BYTES);
        write_be16(rtcp + 16, tcp_checksum(rip, rtcp, 24));
        state->word[ETH_TCPIP_ST_SERVER_SEQ] += payload_bytes;
        return;
    }

    zero_frame(out);
}

static void reference_stack(const uint8_t rx[ETH_TCPIP_TOTAL_BYTES],
                            uint8_t tx[ETH_TCPIP_TOTAL_BYTES],
                            eth_tcpip_state_t state[1],
                            uint16_t tx_len[ETH_TCPIP_FRAME_COUNT]) {
    for (int pkt = 0; pkt < ETH_TCPIP_FRAME_COUNT; ++pkt) {
        const uint8_t *in = rx + pkt * ETH_TCPIP_FRAME_BYTES;
        uint8_t *out = tx + pkt * ETH_TCPIP_FRAME_BYTES;
        tx_len[pkt] = 0u;
        if (read_be16(in + 12) == 0x0806u) {
            reference_process_arp(in, out, &state[0]);
            tx_len[pkt] = 42u;
        } else if (read_be16(in + 12) == 0x0800u && in[23] == 6u) {
            reference_process_tcp(in, out, &state[0]);
            if (read_be16(out + 12) == 0x0800u) {
                tx_len[pkt] = (uint16_t)(14u + read_be16(out + 16));
            }
        } else {
            zero_frame(out);
        }
    }
}

static int compare_frames(const uint8_t *got, const uint8_t *exp, uint16_t got_len,
                          uint16_t exp_len) {
    if (got_len != exp_len) {
        return 1;
    }
    for (uint16_t i = 0; i < exp_len; ++i) {
        if (got[i] != exp[i]) {
            return 1;
        }
    }
    return 0;
}

static int first_diff_index(const uint8_t *got, const uint8_t *exp, uint16_t len) {
    for (uint16_t i = 0; i < len; ++i) {
        if (got[i] != exp[i]) {
            return (int)i;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (freopen(argv[1], "w", stdout) == NULL) {
            perror("freopen");
            return 1;
        }
    }

    uint8_t rx[ETH_TCPIP_TOTAL_BYTES];
    uint8_t tx[ETH_TCPIP_TOTAL_BYTES];
    uint8_t ref_tx[ETH_TCPIP_TOTAL_BYTES];
    uint32_t rx_word[ETH_TCPIP_TOTAL_WORDS];
    uint32_t tx_word[ETH_TCPIP_TOTAL_WORDS];
    uint16_t tx_len[ETH_TCPIP_FRAME_COUNT];
    uint16_t ref_len[ETH_TCPIP_FRAME_COUNT];

    for (int i = 0; i < ETH_TCPIP_TOTAL_BYTES; ++i) {
        rx[i] = 0u;
        tx[i] = 0u;
        ref_tx[i] = 0u;
    }
    for (int i = 0; i < ETH_TCPIP_TOTAL_WORDS; ++i) {
        rx_word[i] = 0u;
        tx_word[i] = 0u;
    }

    build_arp_request(rx + 0 * ETH_TCPIP_FRAME_BYTES);
    build_tcp_syn(rx + 1 * ETH_TCPIP_FRAME_BYTES);
    build_tcp_ack(rx + 2 * ETH_TCPIP_FRAME_BYTES);
    build_tcp_data(rx + 3 * ETH_TCPIP_FRAME_BYTES);

    eth_tcpip_state_t kernel_state[1] = {{
        {0x02000000u, 0x00010000u, 0x0a000001u, 80u, 0x1000u, 0x1000u, 0u, 0u,
         0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
    }};
    eth_tcpip_state_t ref_state[1] = {{
        {0x02000000u, 0x00010000u, 0x0a000001u, 80u, 0x1000u, 0x1000u, 0u, 0u,
         0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
    }};

    pack_packet_words(rx, rx_word);
    eth_tcpip_stack(rx_word, tx_word, kernel_state, tx_len);
    unpack_packet_words(tx_word, tx);
    reference_stack(rx, ref_tx, ref_state, ref_len);

    int failures = 0;
    for (int pkt = 0; pkt < ETH_TCPIP_FRAME_COUNT; ++pkt) {
        const uint8_t *got = tx + pkt * ETH_TCPIP_FRAME_BYTES;
        const uint8_t *exp = ref_tx + pkt * ETH_TCPIP_FRAME_BYTES;
        if (compare_frames(got, exp, tx_len[pkt], ref_len[pkt])) {
            int diff = first_diff_index(got, exp, ref_len[pkt]);
            fprintf(stdout, "FAIL eth_tcpip_stack frame=%d len=%u expected=%u\n", pkt,
                    (unsigned)tx_len[pkt], (unsigned)ref_len[pkt]);
            if (diff >= 0) {
                fprintf(stdout, "eth_tcpip_stack diff idx=%d got=%u expected=%u\n", diff,
                        (unsigned)got[diff], (unsigned)exp[diff]);
            }
            failures = 1;
            break;
        }
        fprintf(stdout, "eth_tcpip_stack frame=%d len=%u state=%u\n", pkt,
                (unsigned)tx_len[pkt],
                (unsigned)kernel_state[0].word[ETH_TCPIP_ST_TCP_STATE]);
    }

    if (kernel_state[0].word[ETH_TCPIP_ST_TCP_STATE] != 2u ||
        kernel_state[0].word[ETH_TCPIP_ST_ARP_CACHED] != 1u) {
        fprintf(stdout, "FAIL eth_tcpip_stack state tcp=%u arp=%u\n",
                (unsigned)kernel_state[0].word[ETH_TCPIP_ST_TCP_STATE],
                (unsigned)kernel_state[0].word[ETH_TCPIP_ST_ARP_CACHED]);
        failures = 1;
    }

    if (!failures) {
        fprintf(stdout, "PASS eth_tcpip_stack\n");
    }

    return failures ? 1 : 0;
}
