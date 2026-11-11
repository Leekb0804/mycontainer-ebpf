/*
 * mycontainer_stop.c
 *
 * 6주차: 컨테이너 graceful shutdown
 *
 * 설계(직접 정한 것):
 *   1. state 파일에서 target PID를 읽는다 (exec.c의 read_state()와 유사)
 *   2. SIGTERM을 딱 한 번 보낸다
 *   3. 일정 시간(TIMEOUT_SECONDS) 동안 프로세스가 끝나기를 기다린다
 *      - 끝나면(정상 종료) -> 여기서 끝
 *   4. 시간 다 지나도 여전히 존재하면 -> SIGKILL로 강제 종료
 *
 * 수정: PID 재사용 대응 (deepdive-doc/mycontainer.md §4-1)
 *   기존에는 kill(pid, ...)로 "숫자 PID"에 신호를 보냈다. state 파일이
 *   남아 있고 그 PID가 다른 프로세스에 재사용됐다면, 상관없는 호스트
 *   프로세스를 죽일 수 있었다.
 *   -> pidfd(프로세스를 가리키는 fd)를 쓴다.
 *      a. pidfd_open(pid): 지금 그 PID를 가진 프로세스를 fd로 "고정"한다
 *      b. 그 다음 STARTTIME을 검증한다. 순서가 중요하다 - 열고 나서
 *         검증해야, 검증을 통과한 pidfd가 가리키는 게 확실히 컨테이너
 *         프로세스다. (검증 먼저 하고 열면, 그 사이에 PID가 바뀔 틈이 있다)
 *      c. 신호는 pidfd_send_signal()로 보낸다 - 숫자 PID를 다시 해석하지
 *         않으므로, 이후 PID가 재사용돼도 엉뚱한 프로세스에 가지 않는다
 *      d. 종료 대기는 kill(pid, 0) 폴링 대신 poll(pidfd)로 한다. pidfd는
 *         프로세스가 끝나면 읽기 가능 상태가 된다. (kill(pid, 0)은 좀비나
 *         재사용된 PID에도 성공해서 "끝났는지"를 정확히 알 수 없다)
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
#include <poll.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include "mycontainer_state.h"

#define TIMEOUT_SECONDS 10
#define KILL_CONFIRM_SECONDS 5

/* glibc 래퍼 유무와 상관없이 쓰려고 syscall()로 직접 호출 (pivot_root, setns와 같은 방식) */
static int my_pidfd_open(pid_t pid) {
    return (int)syscall(SYS_pidfd_open, pid, 0);
}

static int my_pidfd_send_signal(int pidfd, int sig) {
    return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
}

/* pidfd가 가리키는 프로세스가 timeout_sec 안에 끝나면 1, 아니면 0 */
static int wait_exit(int pidfd, int timeout_sec) {
    struct pollfd pfd = { .fd = pidfd, .events = POLLIN };
    int r;
    do {
        r = poll(&pfd, 1, timeout_sec * 1000);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}

/*
 * state 파일에서 PID와 STARTTIME을 읽어온다.
 * (target이 없으면 진짜 문제(잘못된 이름이거나 이미 정리된 컨테이너)이므로
 *  에러 출력 후 종료 - ps.c와는 성격이 다름을 기억할 것)
 *
 * 반환값: 성공 시 0, 실패 시 -1. STARTTIME이 없는 옛 파일이면 *out_starttime = 0.
 */
static int read_target(const char *container_dir, pid_t *out_pid,
                       unsigned long long *out_starttime) {
    char state_path[PATH_MAX];
    snprintf(state_path, sizeof(state_path), "%s/state", container_dir);

    FILE *f = fopen(state_path, "r");
    if (!f) {
        fprintf(stderr, "[!] state 파일을 찾을 수 없음: %s\n", state_path);
        return -1;
    }

    char line[256];
    pid_t pid = -1;
    unsigned long long starttime = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        int val;
        unsigned long long ull;
        if (sscanf(line, "PID=%d", &val) == 1) {
            pid = (pid_t)val;
        } else if (sscanf(line, "STARTTIME=%llu", &ull) == 1) {
            starttime = ull;
        }
    }
    fclose(f);

    if (pid <= 0) return -1;
    *out_pid = pid;
    *out_starttime = starttime;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "사용법: %s <컨테이너 작업디렉토리>\n", argv[0]);
        return 1;
    }

    const char *container_dir = argv[1];

    pid_t target_pid;
    unsigned long long starttime;
    if (read_target(container_dir, &target_pid, &starttime) != 0) {
        fprintf(stderr, "[!] target PID를 찾을 수 없음\n");
        return 1;
    }
    printf("[*] target PID = %d, STARTTIME = %llu\n", target_pid, starttime);

    /* a. 프로세스를 fd로 고정 */
    int pidfd = my_pidfd_open(target_pid);
    if (pidfd < 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "[!] PID %d 프로세스가 없음 - 이미 종료된 컨테이너 "
                            "(state 파일만 남은 유령)\n", target_pid);
        } else {
            fprintf(stderr, "[!] pidfd_open 실패: %s\n", strerror(errno));
        }
        return 1;
    }

    /* b. 고정한 다음에 "같은 프로세스인지" 검증 */
    if (!is_same_process(target_pid, starttime)) {
        fprintf(stderr, "[!] PID %d는 이 컨테이너 프로세스가 아님 (%s) - 신호를 보내지 않음\n",
                target_pid,
                starttime == 0 ? "state에 STARTTIME 없음: 옛 형식이라 검증 불가"
                               : "STARTTIME 불일치: PID가 다른 프로세스에 재사용됨");
        close(pidfd);
        return 1;
    }

    /* c. SIGTERM을 딱 한 번만 전송 - graceful shutdown 요청 */
    printf("[*] SIGTERM 전송\n");
    if (my_pidfd_send_signal(pidfd, SIGTERM) != 0) {
        fprintf(stderr, "[!] SIGTERM 전송 실패: %s\n", strerror(errno));
        /* 실패해도 계속 진행 - 그 사이 이미 죽었을 수 있으니 아래 대기에서 확인 */
    }

    /* d. 종료 대기 */
    if (wait_exit(pidfd, TIMEOUT_SECONDS)) {
        printf("[*] 정상 종료됨 (SIGTERM만으로 종료)\n");
        close(pidfd);
        return 0;
    }

    /* 타임아웃까지 살아있었다는 뜻 - 강제 종료 */
    printf("[*] 타임아웃(%d초) - 강제 종료(SIGKILL) 전송\n", TIMEOUT_SECONDS);
    if (my_pidfd_send_signal(pidfd, SIGKILL) != 0 && errno != ESRCH) {
        fprintf(stderr, "[!] SIGKILL 전송 실패: %s\n", strerror(errno));
        close(pidfd);
        return 1;
    }

    if (!wait_exit(pidfd, KILL_CONFIRM_SECONDS)) {
        fprintf(stderr, "[!] SIGKILL 후 %d초가 지나도 종료되지 않음\n", KILL_CONFIRM_SECONDS);
        close(pidfd);
        return 1;
    }
    printf("[*] 강제 종료 확인\n");

    close(pidfd);
    return 0;
}
