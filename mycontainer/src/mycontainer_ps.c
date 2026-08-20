/*
 * mycontainer_ps.c
 *
 * 6주차: 실행 중인(running) 컨테이너 목록 조회
 *
 * 설계(직접 정한 것):
 *   1. CONTAINERS_ROOT 아래 모든 하위 디렉토리를 훑는다
 *   2. 각 디렉토리에 state 파일이 있는지 확인한다
 *      - 있으면 -> running -> 이름/PID/NETNS_NAME 출력
 *      - 없으면 -> 이미 종료됨 -> 건너뜀
 *   3. 출력 항목은 "자주 보는 요약 정보"만: 컨테이너 이름, PID, NETNS_NAME
 *      (CGROUP_PATH, MERGEDDIR은 필요할 때만 보면 되는 상세 정보라 생략)
 *
 * 사용법:
 *   sudo ./mycontainer_ps
 */
 
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
 
#define CONTAINERS_ROOT "/var/lib/mycontainer/containers"
 
/*
 * state 파일에서 PID, NETNS_NAME을 읽어온다.
 * (읽는 로직 자체는 mycontainer_exec.c의 read_state()와 거의 동일하니
 *  그걸 참고해서 채워보기 - 다만 여기서는 CGROUP_PATH 대신
 *  NETNS_NAME을 파싱해야 함)
 *
 * 반환값: 성공하면 0, state 파일이 없거나 파싱 실패하면 -1
 */

static int read_container_state(const char *container_dir,
                                 pid_t *out_pid,
                                 char *out_netns_name, size_t netns_name_size) {
    char state_path[PATH_MAX];
    snprintf(state_path, sizeof(state_path), "%s/state", container_dir);

    FILE *f = fopen(state_path, "r");
    if (!f) {
        return -1;
    }

    char line[256];
    pid_t pid = -1;
    out_netns_name[0] = '\0';

    while (fgets(line, sizeof(line), f) != NULL) {
        int val;
        char path_buf[PATH_MAX];

        if (sscanf(line, "PID=%d", &val) == 1) {
            pid = (pid_t)val;
        } else if (sscanf(line, "NETNS_NAME=%s", path_buf) == 1) {
            snprintf(out_netns_name, netns_name_size, "%s", path_buf);
        }
    }
    fclose(f);

    if (pid <= 0) return -1;

    *out_pid = pid;
    return 0;
}

int main(void) {
    DIR *dir = opendir(CONTAINERS_ROOT);
    if (dir == NULL) {
        fprintf(stderr, "[!] opendir(%s) 실패: %s\n", CONTAINERS_ROOT, strerror(errno));
        return 1;
    }

    printf("%-20s %-10s %-20s\n", "NAME", "PID", "NETNS");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char container_dir[PATH_MAX];
        snprintf(container_dir, sizeof(container_dir), "%s/%s",
                 CONTAINERS_ROOT, entry->d_name);

        pid_t pid;
        char netns_name[256];
        if (read_container_state(container_dir, &pid, netns_name, sizeof(netns_name)) == 0) {
            printf("%-20s %-10d %-20s\n", entry->d_name, pid, netns_name);
        }
    }

    closedir(dir);
    return 0;
}
