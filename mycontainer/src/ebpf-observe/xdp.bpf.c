// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define PROTO_TCP   0
#define PROTO_UDP   1
#define PROTO_ICMP  2
#define PROTO_OTHER 3
#define PROTO_COUNT 4

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

    struct event *e;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) {
        return XDP_PASS;
    }

    e->pkt_len = ctx->data_end - ctx->data;

    bpf_ringbuf_submit(e, 0);

    return XDP_PASS;
}
