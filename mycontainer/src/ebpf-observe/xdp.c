// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "xdp.skel.h"
#include <errno.h>
#include <net/if.h>

struct event {
    unsigned int pkt_len;
};

static int handle_event(void *ctx, void *data, size_t data_sz) {
    const struct event *e = data;
    
    printf("[XDP] pkt_len=%u\n", e->pkt_len);
    
    return 0;
}


int main(int argc, char* argv[]) {
    
    struct xdp_bpf *skel;
    struct bpf_link *link;
    int err = 0;
    struct ring_buffer *rb = NULL;

    unsigned int ifindex = if_nametoindex(argv[1]);
       
    if (ifindex == 0) {
        fprintf(stderr, "실패");
        return 1;
    }

    skel = xdp_bpf__open_and_load();
    if (!skel) {
		fprintf(stderr, "skeleton open/load 실패\n");
		return 1;
	}

    link = bpf_program__attach_xdp(skel->progs.xdp_practice, ifindex);
    if (!link) {
		fprintf(stderr, "attach 실패\n");
		err = -1;
        goto cleanup;
	}

    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "ring buffer 생성 실패\n");
		err = -1;
		goto cleanup;
    }

    while(1) {
        err = ring_buffer__poll(rb, 100 /* ms 단위 timeout */);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "poll 에러: %d\n", err);
			break;
		}
    }

cleanup:
    ring_buffer__free(rb);
	xdp_bpf__destroy(skel);
	return err < 0 ? -err : 0;
}
