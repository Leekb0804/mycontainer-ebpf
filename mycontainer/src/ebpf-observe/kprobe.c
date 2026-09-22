// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "kprobe.skel.h"
#include <errno.h>

struct event {
	unsigned int host_pid;
	unsigned int pid_ns_inum;
	char comm[16];
    unsigned int container_pid;
};

/* ring buffer에 새 이벤트가 들어올 때마다 이 콜백이 호출된다. */
static int handle_event(void *ctx, void *data, size_t data_sz)
{
	const struct event *e = data;
    printf("[EXEC] host_pid=%-6u pid_ns_inum=%-12u container_pid=%-6u comm=%s\n",
	       e->host_pid, e->pid_ns_inum, e->container_pid, e->comm);
	return 0;
}

int main(void)
{
	struct kprobe_bpf *skel;
	struct ring_buffer *rb = NULL;
	int err;

	skel = kprobe_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "skeleton open/load 실패\n");
		return 1;
	}

    int pin_ret = bpf_map__pin(skel->maps.watched_cgroups, "/sys/fs/bpf/watched_cgroups");
    if (pin_ret != 0) {     // bpf_map__pin() 의 리턴값은 성공하면 0
        if (pin_ret == -EEXIST) {       // 만약 낡은 파일이 이미 있으면 리턴값은 -EEXIST
            unlink("/sys/fs/bpf/watched_cgroups");
            pin_ret = bpf_map__pin(skel->maps.watched_cgroups, "/sys/fs/bpf/watched_cgroups");        
        }

        if (pin_ret != 0) {     // 낡은파일 문제가 아닌경우
            fprintf(stderr, "맵 pin 실패: %d\n", pin_ret);
        }
    }

	err = kprobe_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "attach 실패: %d\n", err);
		goto cleanup;
	}

	rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "ring buffer 생성 실패\n");
		err = -1;
		goto cleanup;
	}

	printf("추적 시작... (Ctrl+C로 종료)\n");
	while (1) {
		err = ring_buffer__poll(rb, 100 /* ms 단위 timeout */);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "poll 에러: %d\n", err);
			break;
		}
	}

cleanup:
	ring_buffer__free(rb);
	kprobe_bpf__destroy(skel);
	return err < 0 ? -err : 0;
}
