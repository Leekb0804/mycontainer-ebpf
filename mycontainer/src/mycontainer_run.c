/*
 * mycontainer_run.c
 *
 * 5주차 통합본: OverlayFS + User Namespace + 기존 pivot_root/cgroup/netns 로직
 *
 * 구조:
 *   main()        = "관리자" 역할. cgroup/netns 설정, overlay용 디렉토리 준비,
 *                    clone()으로 자식을 만들고 uid_map/gid_map을 써준 뒤 대기.
 *   child_entry() = "컨테이너 안" 역할. 파이프로 매핑 완료 신호를 기다린 뒤,
 *                    overlay 마운트 -> pivot_root -> exec 순서로 진행.
 *
 * 사용법:
 *   sudo ./mycontainer_run <이미지 lowerdir 콜론연결> <컨테이너작업디렉토리> \
 *                           <cgroup 경로> <netns 이름> <실행할 프로그램> [인자...]
 *
 * 예:
 *   sudo ./mycontainer_run \
 *     "/var/lib/mycontainer/images/ubuntu-base/layerC:/var/lib/mycontainer/images/ubuntu-base/layerB:/var/lib/mycontainer/images/ubuntu-base/layerA" \
 *     "/var/lib/mycontainer/containers/mycontainer-1234" \
 *     "/sys/fs/cgroup/mycontainer-1234" \
 *     "mycontainer-1234" \
 *     bash
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/sched.h>
#include <limits.h>
#include <sched.h>
#include <fcntl.h>

#define STACK_SIZE (1024 * 1024)

static void die(const char *msg) {
    fprintf(stderr, "[!] %s: %s\n", msg, strerror(errno));
    exit(1);
}

/* pivot_root는 glibc 래퍼가 없는 경우가 많아 syscall()로 직접 호출 */
static int pivot_root(const char *new_root, const char *put_old) {
    return syscall(SYS_pivot_root, new_root, put_old);
}

static void join_cgroup(const char *cgroup_path) {
    pid_t my_pid = getpid();
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", my_pid);

    char procs_file[PATH_MAX];
    snprintf(procs_file, sizeof(procs_file), "%s/cgroup.procs", cgroup_path);

    int fd = open(procs_file, O_WRONLY);
    if (fd < 0) { perror("open cgroup.procs"); return; }

    ssize_t n = write(fd, pid_str, strlen(pid_str));
    if (n != (ssize_t)strlen(pid_str)) { perror("write cgroup.procs"); }

    close(fd);
}

static void set_netns(const char *netns_name) {
    char netns_path[PATH_MAX];
    snprintf(netns_path, sizeof(netns_path), "/var/run/netns/%s", netns_name);

    int fd = open(netns_path, O_RDONLY);
    if (fd < 0) die("netns open 실패");

    if (setns(fd, CLONE_NEWNET) != 0) die("setns 실패");

    close(fd);
}

/* ---------- uid_map / gid_map 작성 (관리자 쪽에서 호출) ---------- */

/*
 * sudo로 실행된 경우 getuid()는 이미 0(root)을 반환하기 때문에 매핑 기준으로
 * 쓸 수 없다. SUDO_UID/SUDO_GID 환경변수(원래 sudo를 실행시킨 사람의 UID)를
 * 우선적으로 사용하고, 없으면 real UID로 대체한다.
 */
static uid_t resolve_mapping_uid(void) {
    const char *sudo_uid = getenv("SUDO_UID");
    if (sudo_uid != NULL) return (uid_t)atoi(sudo_uid);
    return getuid();
}

static gid_t resolve_mapping_gid(void) {
    const char *sudo_gid = getenv("SUDO_GID");
    if (sudo_gid != NULL) return (gid_t)atoi(sudo_gid);
    return getgid();
}

static void write_id_map(pid_t child_pid, const char *file, unsigned int host_id) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/%s", child_pid, file);

    int fd = open(path, O_WRONLY);
    if (fd < 0) die("uid/gid map open 실패");

    char buf[64];
    /* "namespace안UID 호스트UID 범위" : namespace의 0(root)을 host_id 한 개에 매핑 */
    int len = snprintf(buf, sizeof(buf), "0 %u 1\n", host_id);

    if (write(fd, buf, len) != len) die("uid/gid map write 실패");

    close(fd);
}

/*
 * gid_map을 쓰려면 그 전에 setgroups를 deny로 막아야 하는 커널 제약이 있다
 * (리눅스 3.19+, 그룹 권한 악용 방지 목적).
 */
static void deny_setgroups(pid_t child_pid) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/setgroups", child_pid);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        /* 이 파일이 없는 커널도 있을 수 있어 치명적 에러로 취급하지 않음 */
        return;
    }
    const char *deny = "deny";
    if (write(fd, deny, strlen(deny)) < 0) {
        /* 실패해도 계속 진행 (일부 환경에서는 불필요할 수 있음) */
    }
    close(fd);
}

/* ---------- phase2: execve() 이후에 실행되는 "진짜 컨테이너 진입" 로직 ---------- */
/*
 * clone() 직후에는 아직 execve()를 한 번도 거치지 않아서, uid_map으로 매핑을
 * 마쳐도 capability(CapEff)가 전부 0인 상태로 남는다 (man capabilities(7):
 * "이 값들의 재계산은 execve() 도중에 일어난다"). CAP_SYS_ADMIN이 필요한
 * overlay 마운트를 이 상태로 시도하면 Permission denied가 난다.
 * 그래서 이 함수는 child_entry가 execve("/proc/self/exe", ...)로 자기
 * 자신을 다시 실행한 뒤에야 호출된다 - 그 시점에는 매핑된 UID(0) 기준으로
 * capability가 재계산되어 있다.
 */
static void run_phase2(const char *lowerdir, const char *upperdir,
                        const char *workdir, const char *mergeddir,
                        char **cmd_argv) {
    /* setuid/setgid + re-exec 이후 실제로 namespace 0(root)로 보이는지 확인 */
    printf("[*] phase2 진입: getuid=%d geteuid=%d\n", getuid(), geteuid());

    /* ---------- 1단계: propagation을 private로 전환 ---------- */
    printf("[*] 1단계: mount(MS_PRIVATE|MS_REC) - propagation shared -> private\n");
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        die("propagation private 설정 실패");
    }

    /* ---------- 2단계: overlay 마운트 ---------- */
    char overlay_opts[PATH_MAX * 4];
    snprintf(overlay_opts, sizeof(overlay_opts),
             "lowerdir=%s,upperdir=%s,workdir=%s",
             lowerdir, upperdir, workdir);

    printf("[*] 2단계: overlay 마운트 -o %s\n", overlay_opts);
    if (mount("overlay", mergeddir, "overlay", 0, overlay_opts) != 0) {
        die("overlay 마운트 실패");
    }

    /* ---------- 3단계: mergeddir을 마운트 포인트로 승격 (self bind mount) ---------- */
    printf("[*] 3단계: mount(mergeddir, mergeddir, MS_BIND) - 마운트 포인트로 재확인\n");
    if (mount(mergeddir, mergeddir, NULL, MS_BIND | MS_REC, NULL) != 0) {
        die("self bind mount 실패");
    }

    /* ---------- 4단계: put_old 디렉토리 준비 ---------- */
    char put_old[PATH_MAX];
    snprintf(put_old, sizeof(put_old), "%s/.old_root", mergeddir);
    printf("[*] 4단계: put_old 디렉토리 준비 (%s)\n", put_old);
    if (mkdir(put_old, 0700) != 0 && errno != EEXIST) {
        die("put_old mkdir 실패");
    }

    /* ---------- 5단계: pivot_root 실행 ---------- */
    printf("[*] 5단계: pivot_root(merged, put_old) - root 자체를 교체\n");
    if (pivot_root(mergeddir, put_old) != 0) {
        die("pivot_root 실패");
    }

    /* ---------- 6단계: 작업 위치를 새 root로 이동 ---------- */
    printf("[*] 6단계: chdir(\"/\")\n");
    if (chdir("/") != 0) die("chdir 실패");

    /* ---------- 7단계: 옛 root를 마운트 트리에서 완전히 제거 ---------- */
    printf("[*] 7단계: umount2(\"/.old_root\", MNT_DETACH)\n");
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        die("옛 root umount 실패");
    }

    /* ---------- 8단계: 빈 디렉토리 정리 ---------- */
    printf("[*] 8단계: rmdir(\"/.old_root\")\n");
    if (rmdir("/.old_root") != 0) {
        fprintf(stderr, "[!] rmdir 경고: %s (계속 진행)\n", strerror(errno));
    }

    /* ---------- 9단계: procfs 재마운트 ---------- */
    printf("[*] 9단계: mount(\"proc\", \"/proc\", \"proc\", subset=pid)\n");
    if (mount("proc", "/proc", "proc", 0, "subset=pid") != 0) {
        die("procfs 마운트 실패");
    }

    printf("\n[+] 컨테이너 진입 완료. execvp로 넘어갑니다.\n\n");

    /* ---------- 10단계: 지정한 프로그램 실행 ---------- */
    execvp(cmd_argv[0], cmd_argv);
    die("execvp 실패");
}

/* ---------- 자식(컨테이너) 쪽에서 clone() 직후 실행할 로직 ---------- */

struct child_args {
    int pipe_read_fd;
    int pipe_write_fd;   /* clone()으로 물려받은 여분의 쓰기 끝. 자식이 직접 닫아야 함 */
    uid_t map_uid;       /* uid_map에 쓴 값과 동일 (namespace 0에 대응하는 호스트 UID) */
    gid_t map_gid;
    const char *lowerdir;   /* 콜론으로 연결된 이미지 레이어 경로들 */
    const char *upperdir;   /* 관리자가 이미 mkdir+chown까지 끝내둔 경로들 */
    const char *workdir;
    const char *mergeddir;
    char **cmd_argv;     /* 컨테이너 안에서 실행할 프로그램 + 인자 */
};

static int child_entry(void *arg) {
    struct child_args *args = (struct child_args *)arg;

    /*
     * clone()은 fork()처럼 부모의 파일 디스크립터 테이블을 통째로 복제한다.
     * 그래서 이 자식 프로세스도 pipefd[1](쓰기 끝)의 "자기 몫 사본"을
     * 이미 열린 채로 물려받은 상태다. 이걸 안 닫으면, 부모가 자기 몫의
     * 쓰기 끝을 닫아도 커널이 보기엔 "아직 쓰기 끝을 쥔 프로세스가 있다"고
     * 판단해 EOF를 안 보내고, 아래 read()가 영원히 깨어나지 않는다
     * (자기 자신 때문에 생기는 데드락).
     */
    close(args->pipe_write_fd);

    /* ---------- 0단계: 부모가 uid_map/gid_map 쓸 때까지 대기 ---------- */
    printf("[*] 0단계: 매핑 완료 신호 대기 중...\n");
    char buf;
    read(args->pipe_read_fd, &buf, 1); /* 부모가 close하면 0을 리턴하며 깨어남 */
    close(args->pipe_read_fd);
    printf("[*] 매핑 완료 확인.\n");

    /*
     * ---------- 핵심: 이 프로세스 자신의 real UID를 매핑값에 맞춤 ----------
     * main()이 sudo로 실행됐기 때문에, clone()으로 태어난 이 자식도 real UID를
     * 그대로 물려받아 host UID 0(진짜 root)이다. uid_map은
     * "namespace 0 = host map_uid(예: 1000)"으로 써뒀는데, 매핑표에 host UID 0에
     * 대한 항목이 없으므로 지금 이 프로세스가 자기 자신을 namespace 관점에서
     * 봐도 nobody(overflow uid)로 나온다.
     *
     * 여기서 넘기는 숫자는 "호스트 기준 UID/GID"가 아니라 "지금 이 자식이 속한
     * user namespace 관점의 값"으로 해석된다 (이미 clone() 시점에 이 namespace
     * 안으로 들어와 있으므로). gid_map/uid_map은 "namespace 0 = host
     * map_uid/map_gid"로 되어 있으니, namespace 쪽에서 유효한 값은 오직 0뿐이다.
     * setgid(args->map_gid)처럼 호스트 값(1000)을 그대로 넘기면, 그 namespace
     * 관점에서 "GID 1000"이라는 건 매핑표에 없는 범위 밖 값이라 EINVAL이 난다
     * (처음엔 이렇게 시도했다가 실패를 확인함).
     *
     * setgid(0)/setuid(0)으로 - namespace 관점의 0을 넘기면 커널이 매핑표를
     * 참고해 이 프로세스의 real UID/GID를 host map_uid/map_gid로 실제로
     * 바꿔준다. setgid가 setuid보다 먼저여야 한다 - uid를 먼저 낮추면
     * CAP_SETGID를 잃어서 그 다음 setgid가 실패할 수 있다.
     */
    printf("[*] DEBUG: setgid(0)/setuid(0) 호출 (namespace 관점의 root) - "
           "gid_map/uid_map: namespace 0 -> host uid=%u gid=%u\n",
           args->map_uid, args->map_gid);
    if (setgid(0) != 0) die("setgid 실패");
    if (setuid(0) != 0) die("setuid 실패");
    printf("[*] setuid(0)/setgid(0) 완료. 재실행합니다.\n");

    /*
     * ---------- re-exec: capability를 매핑된 UID 기준으로 재계산시킴 ----------
     * "/proc/self/exe"는 항상 지금 실행 중인 바이너리 자신을 가리키는 심볼릭
     * 링크다. 이 경로로 스스로를 execve하면, 커널이 execve() 도중 규칙에
     * 따라 (지금 매핑된 UID가 0이므로) capability를 전부 부여한 채로
     * 다시 시작된다. 그 두 번째 실행을 "--phase2"로 표시해 main()이
     * 구분하게 한다.
     */
    char *new_argv[7 + 32]; /* lowerdir/upper/work/merged 4개 + cmd 최대 32개 여유 */
    int i = 0;
    new_argv[i++] = "/proc/self/exe";
    new_argv[i++] = "--phase2";
    new_argv[i++] = (char *)args->lowerdir;
    new_argv[i++] = (char *)args->upperdir;
    new_argv[i++] = (char *)args->workdir;
    new_argv[i++] = (char *)args->mergeddir;
    for (int j = 0; args->cmd_argv[j] != NULL && i < 7 + 31; j++) {
        new_argv[i++] = args->cmd_argv[j];
    }
    new_argv[i] = NULL;

    extern char **environ;
    execve("/proc/self/exe", new_argv, environ);
    die("phase2 재실행(execve) 실패");
    return 1;
}

int main(int argc, char *argv[]) {
    /*
     * ---------- phase2 분기 ----------
     * child_entry가 execve("/proc/self/exe", ...)로 재실행할 때 이 분기를
     * 탄다. 이 시점은 capability가 매핑된 UID(0) 기준으로 재계산된 이후라,
     * 여기서 바로 overlay 마운트를 포함한 나머지 단계를 진행해도 된다.
     * clone/pipe/uid_map 설정은 이미 첫 번째 실행에서 다 끝난 뒤이므로
     * 다시 하지 않는다.
     */
    if (argc >= 6 && strcmp(argv[1], "--phase2") == 0) {
        const char *lowerdir = argv[2];
        const char *upperdir = argv[3];
        const char *workdir = argv[4];
        const char *mergeddir = argv[5];
        char **cmd_argv = &argv[6];
        run_phase2(lowerdir, upperdir, workdir, mergeddir, cmd_argv);
        return 1; /* run_phase2는 성공 시 execvp로 넘어가 여기로 안 돌아옴 */
    }

    if (argc < 6) {
        fprintf(stderr,
            "사용법: %s <lowerdir 콜론연결> <컨테이너 작업디렉토리> <cgroup 경로> "
            "<netns 이름> <실행할 프로그램> [인자...]\n", argv[0]);
        return 1;
    }

    const char *lowerdir = argv[1];
    const char *container_dir = argv[2];
    const char *cgroup_path = argv[3];
    const char *netns_name = argv[4];
    char **cmd_argv = &argv[5];

    /* ---------- 관리자 프로세스 준비 작업 (기존 로직 유지) ---------- */
    join_cgroup(cgroup_path);
    set_netns(netns_name);

    /* ---------- 매핑 기준 UID/GID 결정 (clone 이전에 미리 확인) ---------- */
    uid_t map_uid = resolve_mapping_uid();
    gid_t map_gid = resolve_mapping_gid();

    /* ---------- overlay용 디렉토리 준비: mkdir + chown(SUDO_UID) ---------- */
    /*
     * 이 시점에서 main()은 아직 clone 이전이라 real UID가 root(sudo 그대로)다.
     * (참고: clone 이후 자식도 real UID는 마찬가지로 root다 - "부모냐 자식이냐"는
     * 소유권 문제와 무관하다.) mkdir만 하면 어느 쪽에서 하든 root 소유로 남는다.
     * 핵심은 위치가 아니라 chown을 명시적으로 호출하는지 여부이며, 여기서는
     * 컨테이너를 요청한 원래 유저(SUDO_UID) 소유로 명시적으로 바꿔준다.
     */
    static char upperdir[PATH_MAX], workdir[PATH_MAX], mergeddir[PATH_MAX];
    snprintf(upperdir, sizeof(upperdir), "%s/upper", container_dir);
    snprintf(workdir, sizeof(workdir), "%s/work", container_dir);
    snprintf(mergeddir, sizeof(mergeddir), "%s/merged", container_dir);

    const char *dirs_to_prepare[] = { container_dir, upperdir, workdir, mergeddir };
    for (size_t i = 0; i < sizeof(dirs_to_prepare) / sizeof(dirs_to_prepare[0]); i++) {
        if (mkdir(dirs_to_prepare[i], 0755) != 0 && errno != EEXIST) {
            die("overlay 디렉토리 생성 실패");
        }
        if (chown(dirs_to_prepare[i], map_uid, map_gid) != 0) {
            die("overlay 디렉토리 chown 실패");
        }
    }

    /* ---------- 파이프 생성 (동기화용) ---------- */
    int pipefd[2];
    if (pipe(pipefd) != 0) die("pipe 생성 실패");

    struct child_args cargs = {
        .pipe_read_fd = pipefd[0],
        .pipe_write_fd = pipefd[1],
        .map_uid = map_uid,
        .map_gid = map_gid,
        .lowerdir = lowerdir,
        .upperdir = upperdir,
        .workdir = workdir,
        .mergeddir = mergeddir,
        .cmd_argv = cmd_argv,
    };

    char *stack = malloc(STACK_SIZE);
    if (!stack) die("스택 할당 실패");
    char *stack_top = stack + STACK_SIZE;

    printf("[*] clone(CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWPID|SIGCHLD) 호출\n");
    pid_t child_pid = clone(child_entry, stack_top,
                             CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | SIGCHLD,
                             &cargs);
    if (child_pid == -1) die("clone 실패");

    /* 부모는 파이프의 쓰기 끝만 쓰고, 읽기 끝은 자식 것이므로 부모는 안 씀 */
    close(pipefd[0]);

    /* ---------- 관리자: uid_map / gid_map 작성 ---------- */
    /* map_uid/map_gid는 위에서(디렉토리 준비 전) 이미 계산해둔 값을 그대로 사용 */
    printf("[*] uid_map: namespace 0 -> host %u\n", map_uid);
    printf("[*] gid_map: namespace 0 -> host %u\n", map_gid);

    deny_setgroups(child_pid);
    write_id_map(child_pid, "uid_map", map_uid);
    write_id_map(child_pid, "gid_map", map_gid);

    /* 매핑 완료 신호: 쓰기 끝을 닫으면 자식의 read()가 0을 리턴하며 깨어남 */
    close(pipefd[1]);

    /* ---------- 자식 종료까지 대기 ---------- */
    int status;
    waitpid(child_pid, &status, 0);

    free(stack);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
