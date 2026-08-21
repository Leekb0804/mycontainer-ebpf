/*
 * mycontainer_stop.c
 *
 * 6주차: 컨테이너 graceful shutdown
 *
 * 설계(직접 정한 것):
 *   1. state 파일에서 target PID를 읽는다 (exec.c의 read_state()와 유사)
 *   2. SIGTERM을 딱 한 번 보낸다
 *   3. 일정 시간(TIMEOUT_SECONDS) 동안, 1초 간격으로 kill(pid, 0)을 반복
 *      호출해서 프로세스가 아직 존재하는지 확인한다
 *      - 존재하지 않게 되면(정상 종료) -> 반복 중단, 여기서 끝
 *   4. 시간 다 지나도 여전히 존재하면 -> SIGKILL로 강제 종료
 *
 * 참고(4주차에서 겪었던 이슈 반영):
 *   run_container.sh에서 cgroup rmdir/netns 정리가 set -e 때문에 한쪽이
 *   실패하면 나머지 정리 단계가 연쇄적으로 스킵되는 문제가 있었음. 여기서는
 *   각 정리 단계가 실패해도 나머지 단계는 계속 진행되도록 만들어야 함
 *   (TODO 5 참고).
 *
 * 사용법:
 *   sudo ./mycontainer_stop <컨테이너 작업디렉토리>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <sys/types.h>

#define TIMEOUT_SECONDS 10

/*
 * state 파일에서 PID를 읽어온다.
 * (mycontainer_exec.c의 read_state()와 거의 동일한 구조 - PID만 필요하니
 *  그 함수보다 더 단순한 버전. 실패 시 -1 대신 여기서는 target이 없으면
 *  진짜 문제(사용자가 잘못된 이름을 줬거나 이미 정리된 컨테이너)이므로
 *  exec.c처럼 에러 출력 후 종료해도 됨 - ps.c와는 성격이 다름을 기억할 것)
 *
 * 반환값: 성공 시 pid, 실패 시 -1
 */
static pid_t read_target_pid(const char *container_dir) {
    char state_path[PATH_MAX];
    snprintf(state_path, sizeof(state_path), "%s/state", container_dir);

    FILE *f = fopen(state_path, "r");
    if (!f) {
        fprintf(stderr, "[!] state 파일을 찾을 수 없음: %s\n", state_path);
        return -1;
    }

    char line[256];
    pid_t pid = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        int val;
        if (sscanf(line, "PID=%d", &val) == 1) {
            pid = (pid_t)val;
        }
    }
    fclose(f);

    return pid;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "사용법: %s <컨테이너 작업디렉토리>\n", argv[0]);
        return 1;
    }

    const char *container_dir = argv[1];

    pid_t target_pid = read_target_pid(container_dir);
    if (target_pid <= 0) {
        fprintf(stderr, "[!] target PID를 찾을 수 없음\n");
        return 1;
    }
    printf("[*] target PID = %d\n", target_pid);

    /* SIGTERM을 딱 한 번만 전송 - graceful shutdown 요청 */
    printf("[*] SIGTERM 전송\n");
    if (kill(target_pid, SIGTERM) != 0) {
        fprintf(stderr, "[!] SIGTERM 전송 실패: %s\n", strerror(errno));
        /* 실패해도 계속 진행 - 이미 죽어있었을 수 있으니 아래 폴링에서 확인 */
    }

    /* TIMEOUT_SECONDS 동안, 1초 간격으로 존재 여부 폴링 */
    for (int i = 0; i < TIMEOUT_SECONDS; i++) {
        if (kill(target_pid, 0) == -1) {
            printf("[*] 정상 종료됨 (SIGTERM만으로 종료)\n");
            return 0;
        }
        sleep(1);
    }

    /* 타임아웃까지 살아있었다는 뜻 - 강제 종료 */
    printf("[*] 타임아웃(%d초) - 강제 종료(SIGKILL) 전송\n", TIMEOUT_SECONDS);
    kill(target_pid, SIGKILL);

    return 0;
}
