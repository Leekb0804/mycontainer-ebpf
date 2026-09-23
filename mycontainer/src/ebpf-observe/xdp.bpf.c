// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>

#define PROTO_TCP   0
#define PROTO_UDP   1
#define PROTO_ICMP  2
#define PROTO_OTHER 3
#define PROTO_COUNT 4

#define ETH_P_IP 0x0800

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct event {
        __u32 pkt_len;
};

struct {
        __uint(type, BPF_MAP_TYPE_RINGBUF);
        __uint(max_entries, 256 * 1024);
} events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, PROTO_COUNT);
    __type(key, __u32);
    __type(value, __u64);
} percpu_arr_map SEC(".maps");

SEC("xdp")
int xdp_practice(struct xdp_md *ctx) {
    // ctx 가 null인지 검증 필요한가?
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct event *e;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) {
        return XDP_PASS;
    }

    e->pkt_len = data_end - data;

    bpf_ringbuf_submit(e, 0);

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) {     // 이더넷 헤더 전체가 패킷 범위 안에 >    있는지
        return XDP_PASS;
    }

    if (eth->h_proto != bpf_htons(ETH_P_IP)) {
        return XDP_PASS;
    }

    struct iphdr *ip = (struct iphdr *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
        return XDP_PASS;
    }

    __u32 key;

    switch(ip->protocol) {
        case IPPROTO_TCP:
            key = PROTO_TCP;
            break;
        case IPPROTO_UDP:
            key = PROTO_UDP;
            break;
        case IPPROTO_ICMP:
            key = PROTO_ICMP;
            break;
        default:
            key = PROTO_OTHER;
            break;
    }

    __u64 *cnt = bpf_map_lookup_elem(&percpu_arr_map, &key);
    if(cnt) {
        (*cnt)++;
    }

    return XDP_PASS;
}
