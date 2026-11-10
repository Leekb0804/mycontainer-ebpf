/*
 * register_cgroup.c
 *
 * eBPF 감시 대상 cgroup 등록/해제 도구.
 *
 * mycontainer_run은 eBPF를 전혀 모른다 (컨테이너 런타임과 관측 도구 분리).
 * 대신 run_container.sh가 컨테이너 실행 전후로 이 도구를 호출해서,
 * kprobe 로더가 pin해둔 맵(/sys/fs/bpf/watched_cgroups)에 cgroup id를
 * 넣고 뺀다. 맵의 key는 cgroup v2 디렉토리의 inode 번호이며, 이는
 * BPF 쪽 bpf_get_current_cgroup_id()가 반환하는 값과 같다.
 *
 * 사용법:
 *   sudo ./register_cgroup add <cgroup 경로>
 *   sudo ./register_cgroup del <cgroup 경로>
 *
 * 반환값: 성공 0, 실패 1 (호출하는 쪽에서 실패를 무시하고 진행해도 됨)
 */

#include <bpf/bpf.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define PIN_PATH "/sys/fs/bpf/watched_cgroups"

int main(int argc, char *argv[]) {
    if (argc != 3 || (strcmp(argv[1], "add") != 0 && strcmp(argv[1], "del") != 0)) {
        fprintf(stderr, "사용법: %s add|del <cgroup 경로>\n", argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *cgroup_path = argv[2];

    /* ---------- cgroup id 확보 ---------- */
    struct stat st;
    if (stat(cgroup_path, &st) != 0) {
        fprintf(stderr, "[!] stat(%s) 실패: %s\n", cgroup_path, strerror(errno));
        return 1;
    }
    __u64 cgroup_id = st.st_ino;

    /* ---------- pin된 맵 열기 (kprobe 로더가 안 떠 있으면 여기서 실패) ---------- */
    int bpf_fd = bpf_obj_get(PIN_PATH);
    if (bpf_fd < 0) {
        fprintf(stderr, "[!] %s 열기 실패: %s (kprobe 로더가 실행 중인지 확인)\n",
                PIN_PATH, strerror(errno));
        return 1;
    }

    int ret;
    if (strcmp(mode, "add") == 0) {
        __u8 dummy = 1;
        ret = bpf_map_update_elem(bpf_fd, &cgroup_id, &dummy, BPF_ANY);
    } else {
        ret = bpf_map_delete_elem(bpf_fd, &cgroup_id);
    }

    if (ret != 0) {
        fprintf(stderr, "[!] 맵 %s 실패 (cgroup_id=%llu): %s\n",
                mode, (unsigned long long)cgroup_id, strerror(errno));
        close(bpf_fd);
        return 1;
    }

    printf("[*] eBPF 감시 %s 완료: cgroup_id=%llu\n", mode, (unsigned long long)cgroup_id);
    close(bpf_fd);
    return 0;
}
