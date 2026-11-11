/*
 * mycontainer_state.h
 *
 * state 파일의 PID가 "그 컨테이너 프로세스"를 아직 가리키는지 판별하는 공통 코드.
 * run(기록) / ps / stop / exec(검증)가 같은 규칙을 써야 하므로 한 곳에 둔다.
 *
 * 왜 필요한가:
 *   PID는 프로세스가 죽으면 다른 프로세스에 재사용된다. state 파일이 남아
 *   있으면(예: mycontainer_run이 Ctrl+C로 죽어서 정리를 못 한 경우)
 *   - ps: 죽은 컨테이너를 running으로 표시하고
 *   - stop: 그 PID를 새로 받은 "상관없는 호스트 프로세스"에 SIGTERM/SIGKILL을 보낸다.
 *   kill(pid, 0)은 "그 PID에 누군가 있다"만 알려줄 뿐, "같은 프로세스"인지는 모른다.
 *
 * 판별 방법:
 *   (PID, starttime) 쌍으로 프로세스를 식별한다. starttime은 /proc/<pid>/stat의
 *   22번째 필드(부팅 후 clock tick 단위의 프로세스 시작 시각)라서, PID가 같아도
 *   다른 프로세스면 값이 다르다. run이 clone 직후 이 값을 state에 기록하고,
 *   나머지 도구는 현재 값과 비교한다. STARTTIME이 없는 옛 state 파일은
 *   검증할 수 없으므로 "죽은 것"으로 취급한다.
 */
#ifndef MYCONTAINER_STATE_H
#define MYCONTAINER_STATE_H

#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/*
 * /proc/<pid>/stat에서 starttime(22번째 필드)을 읽는다. 실패(프로세스 없음 등)하면 0.
 *
 * 주의: 2번째 필드(comm)는 "(프로세스 이름)" 형태인데, 이름에 공백이나 ')'가
 * 들어갈 수 있어서 앞에서부터 공백으로 세면 틀린다. 그래서 마지막 ')'를 찾아
 * 그 뒤부터 센다 - ')' 뒤 첫 토큰이 3번째 필드(state)이므로 22번째 필드는
 * 거기서 20번째 토큰이다.
 */
static inline unsigned long long read_proc_starttime(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char *p = strrchr(buf, ')');
    if (!p) return 0;

    char *save = NULL;
    char *tok = strtok_r(p + 1, " ", &save);
    for (int field = 3; tok != NULL; field++) {
        if (field == 22) {
            unsigned long long v = 0;
            return sscanf(tok, "%llu", &v) == 1 ? v : 0;
        }
        tok = strtok_r(NULL, " ", &save);
    }
    return 0;
}

/* state에 기록된 (pid, starttime)이 지금도 같은 프로세스를 가리키면 1 */
static inline int is_same_process(pid_t pid, unsigned long long expected_starttime) {
    if (pid <= 0 || expected_starttime == 0) return 0;
    return read_proc_starttime(pid) == expected_starttime;
}

#endif
