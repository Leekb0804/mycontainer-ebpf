// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "xdp.skel.h"
#include <errno.h>
#include <net/if.h>
#include <bpf/bpf.h>
#include <time.h>

#define PROTO_TCP   0
#define PROTO_UDP   1
#define PROTO_ICMP  2
#define PROTO_OTHER 3
#define PROTO_COUNT 4

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
    
    unsigned int cpu_core_cnt = libbpf_num_possible_cpus();
    __u64 values[cpu_core_cnt];     // bpf_map_lookup_elem 으로 percpu map의 value            값을 담을 공간.
  
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

    int percpu_fd = bpf_map__fd(skel->maps.percpu_arr_map);
    const char *names[PROTO_COUNT] = {"TCP", "UDP", "ICMP", "OTHER"};
    time_t last_print = time(NULL);

    while(1) {
        err = ring_buffer__poll(rb, 100 /* ms 단위 timeout */);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "poll 에러: %d\n", err);
			break;
		}

        time_t now = time(NULL);
        if (now - last_print >= 1) {

            for( __u32 key = 0; key < PROTO_COUNT; key++) {
                __u64  sum = 0;

                int percpu_err = bpf_map_lookup_elem(percpu_fd, &key, &values[0]);
                if (percpu_err != 0) {
                    fprintf(stderr, "percpu_arr_map lookup 실패 (key=%u): %d\n", key, percpu_err);
                    continue;
                }

                for(int i = 0; i < cpu_core_cnt; i++) {
                    sum += values[i];
                }
            
                printf("%s : %llu\n", names[key], sum);
            }
            
            last_print = now;
        }
    }

cleanup:
    ring_buffer__free(rb);
	xdp_bpf__destroy(skel);
	return err < 0 ? -err : 0;
}
