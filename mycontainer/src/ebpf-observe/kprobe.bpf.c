// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

/* 유저스페이스로 넘길 이벤트 하나의 모양.
 * bpf_printk처럼 텍스트로 풀어쓰지 않고, 이 구조체 그대로
 * 바이너리로 ring buffer에 실어 보낸다. */
struct event {
	__u32 host_pid;      /* 호스트(root ns) 기준 PID */
	__u32 pid_ns_inum;   /* 이 프로세스가 속한 PID 네임스페이스 식별자 */
	char comm[16];       /* 프로세스 이름 */
    __u32 container_pid;
};

/* ring buffer map 선언. 256KB짜리 공유 원형 버퍼 하나. */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} events SEC(".maps");

/* hash map 선언*/
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 32);
    __type(key, __u64);
    __type(value, __u8);
} watched_cgroups SEC(".maps");

/* kprobe가 아니라 tracepoint를 후킹한다.
 * sched_process_exec는 execve 계열이 성공적으로 끝난 직후,
 * "새 프로그램으로 교체된 그 프로세스" 컨텍스트에서 실행된다. */
SEC("tracepoint/sched/sched_process_exec")
int handle_exec(void *ctx)
{
    __u64 cgroup_id = bpf_get_current_cgroup_id();
    
    void *watched = bpf_map_lookup_elem(&watched_cgroups, &cgroup_id);
    if (!watched)
        return 0;   // 감시 대상이 아니면 그냥 버림

	struct task_struct *task = (struct task_struct *)bpf_get_current_task();
	struct event *e;

	/* ring buffer에 이 이벤트 하나를 쓸 공간을 예약한다.
	 * 공간이 없으면(consumer가 못 따라오면) NULL이 반환된다. */
	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e)
		return 0;

	e->host_pid = bpf_get_current_pid_tgid() >> 32;

    struct pid *spid = BPF_CORE_READ(task, thread_pid);
    int level = BPF_CORE_READ(spid, level);
    e->container_pid = BPF_CORE_READ(task, thread_pid, numbers[level].nr);

	/* task->nsproxy->pid_ns_for_children->ns.inum
	 * CO-RE relocation 덕분에 커널 버전이 달라도 이 체인이 안전하게 읽힌다. */
	e->pid_ns_inum = BPF_CORE_READ(task, nsproxy, pid_ns_for_children, ns.inum);

	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	/* 다 채웠으니 확정 — 이제 유저스페이스가 읽을 수 있다. */
	bpf_ringbuf_submit(e, 0);
	return 0;
}
