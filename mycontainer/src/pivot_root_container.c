#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/sched.h>
#include <limits.h>
#include <sched.h>
#include <fcntl.h>

static void die(const char *msg) {
    fprintf(stderr, "[!] %s: %s\n", msg, strerror(errno));
    exit(1);
}

/* pivot_root는 glibc 래퍼가 없는 경우가 많아 syscall()로 직접 호출 */
static int pivot_root(const char *new_root, const char *put_old) {
    return syscall(SYS_pivot_root, new_root, put_old);
}

static void join_cgroup(const char *cgroup_path) {
    pid_t my_pid = getpid();        // 이 C 프로그램(pivot_root_container) 자신의 PID
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", my_pid);

    char procs_file[PATH_MAX];
    snprintf(procs_file, sizeof(procs_file), "%s/cgroup.procs", cgroup_path);

    int fd = open(procs_file, O_WRONLY);
    if (fd < 0) {
	    perror("open");
	    return;
    }
    
    int write_check = write(fd, pid_str, strlen(pid_str));
    if (write_check != strlen(pid_str)) {
    	perror("write");
	return;
    }

    int close_check = close(fd);
    if (close_check < 0) {
	perror("close");
	return;
    }
}

static void set_netns(const char *netns_name) {
    char netns_path[PATH_MAX];
    snprintf(netns_path, sizeof(netns_path), "/var/run/netns/%s", netns_name);

    int fd = open(netns_path, O_RDONLY);
    if (fd < 0) {
        die("netns open 실패");
    }

    if (setns(fd, CLONE_NEWNET) != 0) {
        die("setns 실패");
    }

    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "사용법: %s <rootfs 경로> <cgroup 경로> <netns 이름> <실행할 프로그램> [인자...]\n", argv[0]);
        return 1;
    }

    const char *my_cgroup_path = argv[2];
    join_cgroup(my_cgroup_path);	
    
    const char *netns_name = argv[3];
    set_netns(netns_name);

    char new_root[PATH_MAX];
    if (realpath(argv[1], new_root) == NULL) {
        die("realpath 실패 (경로가 존재하는지 확인)");
    }
    printf("[*] new_root = %s\n", new_root);

    /* ---------- 1단계: mount namespace 분리 ---------- */
    printf("[*] 1단계: unshare(CLONE_NEWNS) - mount namespace 분리\n");
    if (unshare(CLONE_NEWNS) != 0) {
        die("unshare 실패 (root 권한으로 실행했는지 확인: sudo ...)");
    }

    /* ---------- 2단계: propagation을 private로 전환 ---------- */
    printf("[*] 2단계: mount(MS_PRIVATE|MS_REC) - propagation shared -> private\n");
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        die("propagation private 설정 실패");
    }

    /* ---------- 3단계: new_root를 마운트 포인트로 승격 (self bind mount) ---------- */
    printf("[*] 3단계: mount(new_root, new_root, MS_BIND) - 마운트 포인트로 승격\n");
    if (mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL) != 0) {
        die("self bind mount 실패");
    }

    /* ---------- 4단계: put_old 디렉토리 준비 ---------- */
    char put_old[PATH_MAX];
    snprintf(put_old, sizeof(put_old), "%s/.old_root", new_root);
    printf("[*] 4단계: put_old 디렉토리 준비 (%s)\n", put_old);
    if (mkdir(put_old, 0700) != 0 && errno != EEXIST) {
        die("put_old mkdir 실패");
    }

    /* ---------- 5단계: pivot_root 실행 ---------- */
    printf("[*] 5단계: pivot_root(new_root, put_old) - root 자체를 교체\n");
    if (pivot_root(new_root, put_old) != 0) {
        die("pivot_root 실패 (new_root가 마운트 포인트인지, put_old가 그 하위인지 확인)");
    }

    /* ---------- 6단계: 작업 위치를 새 root로 이동 ---------- */
    printf("[*] 6단계: chdir(\"/\") - 새 root로 작업 디렉토리 이동\n");
    if (chdir("/") != 0) {
        die("chdir 실패");
    }

    /* ---------- 7단계: 옛 root를 마운트 트리에서 완전히 제거 ---------- */
    printf("[*] 7단계: umount2(\"/.old_root\", MNT_DETACH) - 옛 root 완전 격리\n");
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        die("옛 root umount 실패");
    }

    /* ---------- 8단계: 빈 디렉토리 정리 ---------- */
    printf("[*] 8단계: rmdir(\"/.old_root\") - 남은 빈 디렉토리 정리\n");
    if (rmdir("/.old_root") != 0) {
        /* 실패해도 치명적이지 않으니 경고만 출력하고 계속 진행 */
        fprintf(stderr, "[!] rmdir 경고: %s (계속 진행)\n", strerror(errno));
    }

    /* ---------- 9단계: procfs 재마운트 ---------- */
    printf("[*] 9단계: mount(\"proc\", \"/proc\", \"proc\") - procfs 마운트\n");
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        die("procfs 마운트 실패");
    }

    printf("\n[+] pivot_root 완료. 격리된 rootfs로 진입합니다.\n");
    printf("[+] 확인해볼 것: ls / (busybox rootfs만 보이는지), ps (procfs 정상 동작하는지)\n\n");

    /* ---------- 10단계: 격리된 셸로 진입 ---------- */
    execvp(argv[4], &argv[4]);
    die("execvp 실패 (프로그램을 찾을 수 없거나 실행 권한이 없는지 확인)");
    return 1;
}
