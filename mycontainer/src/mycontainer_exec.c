/*
 * mycontainer_exec.c
 *
 * 6주차: 실행 중인 컨테이너에 재진입(docker exec와 동일한 원리)
 *
 * 원리:
 *   - target PID가 속한 mnt/pid/net/uts namespace로 setns()
 *   - PID namespace는 setns() 즉시 적용되지 않고, 이후 fork()한
 *     자식부터 적용됨 -> 그래서 PID namespace만 fork() 이전에 미리
 *     setns()로 "예약"해두고, 그 다음 fork() 해야 함
 *   - mnt/net/uts는 즉시 적용되므로 자식 프로세스 안에서 setns()해도 무방
 *   - cgroup은 namespace 재진입(setns)이 아니라 "가입" 개념 - state
 *     파일에 저장된 CGROUP_PATH의 cgroup.procs에 자식 PID를 직접 씀
 *     (mycontainer_run.c의 join_cgroup()과 동일한 방식). 이렇게 해야
 *     exec로 들어간 프로세스도 컨테이너와 같은 memory/cpu/pids 제한을
 *     받는다 - 안 하면 무제한 리소스를 쓰는 "무늬만 컨테이너 안"인
 *     상태가 됨.
 *
 * 스코프 아웃(의도적 생략): user namespace 재진입은 하지 않음.
 *   이유: setns(CLONE_NEWUSER) 이후 setuid(0)/setgid(0) 처리가 추가로
 *   필요해 복잡도가 크게 늘어나는데, exec의 학습 목표(setns 기반 재진입
 *   원리 이해 + 최소 동작 데모)에는 필수가 아니라고 판단함. 실제
 *   nsenter/docker exec도 상황에 따라 user namespace 재진입을 생략하는
 *   경우가 흔함. 필요 시 향후 개선 대상으로 남겨둠.
 *
 * 사용법:
 *   sudo ./mycontainer_exec <컨테이너 작업디렉토리> <실행할 프로그램> [인자...]
 *
 * 예:
 *   sudo ./mycontainer_exec /var/lib/mycontainer/containers/mycontainer-1234 bash
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/sched.h>

static void die(const char *msg) {
    fprintf(stderr, "[!] %s: %s\n", msg, strerror(errno));
    exit(1);
}

/* glibc setns() 래퍼가 없는 환경 대비 syscall 직접 호출 버전 */
static int do_setns(int fd, int nstype) {
    return syscall(SYS_setns, fd, nstype);
}

/*
 * state 파일(container_dir/state)에서 "PID=1234", "CGROUP_PATH=..." 줄을
 * 찾아 각각 out_pid/out_cgroup_path에 채워준다.
 * 형식은 mycontainer_run.c의 write_state_file()과 일치해야 함.
 */
static void read_state(const char *container_dir, pid_t *out_pid,
                        char *out_cgroup_path, size_t cgroup_path_size) {
    char state_path[PATH_MAX];
    snprintf(state_path, sizeof(state_path), "%s/state", container_dir);

    FILE *f = fopen(state_path, "r");
    if (!f) die("state 파일 열기 실패 (컨테이너가 실행 중인지 확인)");

    char line[256];
    pid_t pid = -1;
    out_cgroup_path[0] = '\0';
    while (fgets(line, sizeof(line), f) != NULL) {
        int val;
        char path_buf[PATH_MAX];

        if (sscanf(line, "PID=%d", &val) == 1) {
            pid = (pid_t)val;
        } else if (sscanf(line, "CGROUP_PATH=%s", path_buf) == 1) {
            snprintf(out_cgroup_path, cgroup_path_size, "%s", path_buf);
        }
    }
    fclose(f);

    if (pid <= 0) die("state 파일에서 PID를 찾지 못함");
    if (out_cgroup_path[0] == '\0') die("state 파일에서 CGROUP_PATH를 찾지 못함");

    *out_pid = pid;
}

/* /proc/[pid]/ns/<name> 을 open()해서 fd 반환 */
static int open_ns_fd(pid_t pid, const char *ns_name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, ns_name);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[!] ns fd open 실패: %s\n", path);
        die("open_ns_fd");
    }
    return fd;
}

/*
 * cgroup은 setns() 대상이 아니라 "가입"의 개념이다 (CLONE_NEWCGROUP이라는
 * 별도 namespace가 있긴 하지만 mycontainer_run.c가 그걸 쓰지 않으므로
 * 여기서도 다루지 않는다). mycontainer_run.c의 join_cgroup()과 완전히
 * 동일한 방식: cgroup.procs 파일에 내 PID를 쓰면 그 cgroup의 제한을
 * 받게 된다. 이 함수는 자식 프로세스 안에서, 자기 자신의 PID를 쓰기
 * 위해 호출한다.
 */
static void join_cgroup(const char *cgroup_path) {
    pid_t my_pid = getpid();
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", my_pid);

    char procs_file[PATH_MAX];
    snprintf(procs_file, sizeof(procs_file), "%s/cgroup.procs", cgroup_path);

    int fd = open(procs_file, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[!] cgroup.procs open 실패: %s\n", procs_file);
        die("join_cgroup");
    }

    ssize_t n = write(fd, pid_str, strlen(pid_str));
    if (n != (ssize_t)strlen(pid_str)) {
        close(fd);
        die("cgroup.procs write 실패");
    }

    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "사용법: %s <컨테이너 작업디렉토리> <실행할 프로그램> [인자...]\n",
                argv[0]);
        return 1;
    }

    const char *container_dir = argv[1];
    char **cmd_argv = &argv[2];

    pid_t target_pid;
    char cgroup_path[PATH_MAX];
    read_state(container_dir, &target_pid, cgroup_path, sizeof(cgroup_path));
    printf("[*] target PID = %d, CGROUP_PATH = %s\n", target_pid, cgroup_path);

    /* ---------- 1단계: 필요한 namespace fd를 미리 다 열어둔다 ---------- */
    /*
     * fork() 이후에는 자식이 target 프로세스가 이미 종료돼서
     * "/proc/[pid]/ns/" 아래 파일들이 사라져도 상관없도록, fd는
     * fork 전에 미리 확보해둔다 (fd는 열려있는 한 유효함).
     */
    int fd_pid = open_ns_fd(target_pid, "pid");
    int fd_mnt = open_ns_fd(target_pid, "mnt");
    int fd_net = open_ns_fd(target_pid, "net");
    int fd_uts = open_ns_fd(target_pid, "uts");

    /* ---------- 2단계: PID namespace는 fork() 전에 미리 setns ---------- */
    /*
     * 지난 논의 정리: setns(CLONE_NEWPID)는 호출한 프로세스 자신에게는
     * 적용되지 않고, "앞으로 이 프로세스가 fork()할 자식"부터 적용된다.
     * 그래서 반드시 fork()보다 먼저 호출해서 "예약"해둬야 한다.
     */
    printf("[*] setns(PID) - 이후 fork()할 자식부터 적용됨\n");
    if (do_setns(fd_pid, CLONE_NEWPID) != 0) die("setns(PID) 실패");
    close(fd_pid);

    /* ---------- 3단계: fork ---------- */
    pid_t child = fork();
    if (child < 0) die("fork 실패");

    if (child == 0) {
        /* ---------- 자식 프로세스: 새 PID namespace 소속으로 태어남 ---------- */

        /*
         * mnt/net/uts는 setns() 즉시 호출한 프로세스(이 자식)에
         * 적용되므로 여기서 해도 된다. fork 전(부모)에서 해도 동작은
         * 하지만, "이 자식만 재진입한다"는 의도를 코드로도 명확히
         * 하기 위해 자식 안에서 처리한다.
         */
        printf("[*] (child) setns(NET)\n");
        if (do_setns(fd_net, CLONE_NEWNET) != 0) die("setns(NET) 실패");
        close(fd_net);

        printf("[*] (child) setns(MNT)\n");
        if (do_setns(fd_mnt, CLONE_NEWNS) != 0) die("setns(MNT) 실패");
        close(fd_mnt);

        printf("[*] (child) setns(UTS)\n");
        if (do_setns(fd_uts, CLONE_NEWUTS) != 0) die("setns(UTS) 실패");
        close(fd_uts);

        /*
         * cgroup 가입은 namespace 재진입이 아니라 cgroup.procs에
         * 내 PID를 쓰는 것 - mnt/net/uts와 순서상 특별한 제약은 없지만,
         * "namespace 재진입을 다 마친 뒤 마지막으로 리소스 제한을
         * 건다"는 순서가 읽기에 자연스러워 execvp 직전에 둔다.
         */
        printf("[*] (child) cgroup 가입: %s\n", cgroup_path);
        join_cgroup(cgroup_path);

        printf("[*] (child) execvp 진입: %s\n", cmd_argv[0]);
        execvp(cmd_argv[0], cmd_argv);
        die("execvp 실패");
    }

    /* ---------- 부모: fd 정리 + 자식 종료 대기 ---------- */
    close(fd_net);
    close(fd_mnt);
    close(fd_uts);

    int status;
    waitpid(child, &status, 0);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
