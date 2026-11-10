# mycontainer: Docker 컨테이너와의 비교·검증 기록

> **목적**: Docker와 **똑같이 만드는 것이 아니라**, 직접 만든 컨테이너와 Docker 컨테이너를 **비교하고 검증하는 것**.
> 그래서 Docker와 다른 점은 고칠 대상이 아니라 관찰하고 설명할 대상이다.
> 코드를 바꾸는 경우는 실제 버그일 때, 또는 실험에서 변수 하나만 바꿔보고 싶을 때뿐이다.

표기: ✅ 확인됨 / 🔶 가설 / ⬜ 미실측 (Docker 쪽은 아직 이 환경에서 돌려보지 않은 항목이 많다)

---

## 0. 실험 환경

| 항목 | 값 |
|---|---|
| 커널 | Linux 7.0.0-27-generic (aarch64) |
| 배포판 | Ubuntu (`kernel.apparmor_restrict_unprivileged_userns = 1`) |
| cgroup | v2 |
| rootfs | busybox 1.37.0 (`busybox_rootfs`, overlay lowerdir로 사용) |
| 네트워크 | netns + veth + `br0`(10.0.0.1/24) + NAT |

`apparmor_restrict_unprivileged_userns = 1`이라서 **일반 사용자로는 user ns를 만들 수 없다** (`unshare -U`가 `uid_map` 쓰기에서 EPERM). user ns를 쓰는 실험은 sudo로 해야 한다.

---

## 1. 구조 대응: 내 코드 ↔ Docker 계층

Docker는 `docker CLI → dockerd → containerd → runc`로 나뉘어 있다.

| 내 코드 | Docker 쪽 대응 | 비고 |
|---|---|---|
| `mycontainer_run.c`: namespace, user ns, pivot_root | runc | |
| `run_phase2` 2단계: overlay 마운트 | containerd snapshotter | runc는 이미 준비된 rootfs를 받기만 함. 마운트하는 위치가 다름 |
| `run_container.sh`: cgroup 생성과 제한 | runc (`config.json`의 resources) | |
| `run_container.sh`: veth, br0, NAT | dockerd (libnetwork) | docker0 브리지에 해당 |
| `mycontainer_exec` / `mycontainer_ps` / `mycontainer_stop` | `runc exec` / `runc list` / `docker stop` | |
| `ebpf-observe/` + `register_cgroup` | 없음 (Falco, Tetragon 같은 외부 도구) | 런타임과 분리 (§5-1) |

---

## 2. 검증 결과

### 2-1. 기본 격리 동작 ✅

`sudo ./run_container.sh -- /bin/sh`

| 확인 | 결과 | 의미 |
|---|---|---|
| 0~10단계 진행 | 전부 통과 | user ns → overlay → pivot_root → execvp |
| `hostname` | `mycontainer` | UTS ns 격리 |
| `id` | `uid=0 gid=0 groups=65534` | user ns 매핑 (ns 0 → 호스트 1000) |
| `ls /` | busybox rootfs | overlay + pivot_root |
| eBPF 로더가 없을 때 | 경고만 찍고 컨테이너는 뜸 | 관측 기능은 선택 사항 (§5-1) |

**`groups=65534`**: sudo로 실행한 호스트 root의 보조 그룹(0)이 `gid_map`에 없어서 overflow gid(nobody)로 보인다. `setgroups`를 `deny`로 막아뒀기 때문에 컨테이너 안에서 이 그룹을 떼어낼 수도 없다.
- Docker(rootful) ⬜: user ns를 쓰지 않으므로 `groups=0(root)...`처럼 정상 그룹이 보일 것

### 2-2. user ns와 capability의 효력 범위 (ping) ✅

**질문**: 컨테이너 안에서 uid 0이고 capability도 다 있는데 왜 `ping`이 거부되나?

| 실행 위치 | user ns | `CapEff` | `uid_map` | ping |
|---|---|---|---|---|
| 호스트 root: `ip netns exec $NS ping` | 호스트 | 전부 | `0 0 4294967295` | ✅ 성공 |
| **run 쉘** (컨테이너 init) | 새 user ns | `000001ffffffffff` (전부) | `0 1000 1` | ❌ `permission denied (are you root?)` |
| **exec 쉘** (`mycontainer_exec`) | 호스트 (user ns에 안 들어감) | 전부 | `0 0 4294967295` | ✅ 성공 |

같은 컨테이너, 같은 netns, 같은 busybox인데 **user ns 하나만 달라서 결과가 갈린다.**

**원인** ✅
- raw 소켓을 만들 때 커널은 `ns_capable(net->user_ns, CAP_NET_RAW)`, 즉 **netns 주인 user ns 기준으로** 권한을 검사한다.
- netns는 `ip netns add`로 호스트 root가 만들었으므로 주인은 호스트(init) user ns다.
- run 쉘의 `CAP_NET_RAW`는 새 user ns 기준의 권한이라 이 netns에는 효력이 없다.
- 정리: **capability는 "무엇을 할 수 있나"에 더해 "어느 user ns 소속 자원에 대해서인가"까지 포함한다.** `CapEff`만 보면 다 있는 것처럼 보인다.

**`ping_group_range` 우회 실험에서 알게 된 것** ✅
- 새 netns의 `net.ipv4.ping_group_range` 기본값은 `1 0`(꺼짐). 호스트(init netns)는 `0 2147483647`.
- 이 값을 열어도 run 쉘의 ping은 여전히 실패했다.
- 원인: **busybox ping은 `SOCK_RAW` 하나만 시도하고, 실패하면 바로 종료한다**(`strace`로 확인). `ping_group_range`는 ICMP `SOCK_DGRAM` 소켓에만 적용된다. 같은 조건에서 `SOCK_DGRAM` ICMP 소켓 생성 자체는 성공한다.
- 실험 도구가 검증하려는 가설에 맞는지 먼저 확인해야 한다는 교훈.
- 참고: 호스트의 iputils `ping`이 일반 사용자로도 되는 건 바이너리에 `cap_net_raw=ep` file capability가 붙어 있어서다.

**Docker 대조** ⬜
- rootful Docker: 컨테이너 root가 호스트 root이고 netns 주인도 같은 user ns. 기본 capability 목록에 `NET_RAW`가 들어 있고, 20.10부터는 `ping_group_range`도 열어줌 → 성공 예상
- rootless Docker: 내 컨테이너와 비슷한 조건 → 비교해볼 가치 있음
- 대조 명령: `docker run --rm busybox ping -c1 8.8.8.8`, `docker run --rm --cap-drop NET_RAW busybox ping -c1 8.8.8.8`

### 2-3. stop과 PID 1의 시그널 처리 ✅

```
[*] SIGTERM 전송
[*] 타임아웃(10초) - 강제 종료(SIGKILL) 전송
```

- 컨테이너 init은 새 PID ns의 PID 1이다. 커널은 PID 1이 **핸들러를 등록하지 않은 시그널을 버린다.** 바깥 ns에서 보내도 SIGKILL과 SIGSTOP만 예외다.
- ash는 SIGTERM 핸들러가 없으므로 항상 10초를 다 기다린 뒤 SIGKILL로 끝난다.
- stop 후 정리 ✅: state 파일, cgroup, netns는 제거됨. upper/work/merged 디렉토리는 남음 (§4-2).

**Docker 대조** ⬜
- `docker stop`도 기본 동작이 "SIGTERM 후 10초 기다렸다가 SIGKILL"로 같다.
- `docker run busybox sh` + `docker stop` → 10초 걸릴 것으로 예상
- `docker run --init ...` (tini가 PID 1) → 바로 종료될 것으로 예상

### 2-4. exec는 init보다 권한이 크다 ✅

`mycontainer_exec`는 pid, mnt, net, uts ns와 cgroup에는 들어가지만 **user ns에는 들어가지 않는다** (코드 주석에 "의도적 생략"으로 기록됨).

| | run 쉘 (init) | exec 쉘 |
|---|---|---|
| `id` | `uid=0 gid=0 groups=65534` | `uid=0 gid=0 groups=0` |
| `uid_map` | `0 1000 1` | `0 0 4294967295` |
| 호스트 기준 정체 | uid 1000 | **진짜 호스트 root** |
| ping | ❌ | ✅ |

- 결과적으로 exec로 들어간 프로세스가 컨테이너 init보다 **권한이 크다.**
- Docker ⬜: `docker exec`는 원래 프로세스와 같은 조건(같은 user ns, capability, seccomp)으로 들어간다.

---

## 3. 가설: procfs "Mount too revealing" 🔶

**현상**: 새 procfs를 마운트하면 `VFS: Mount too revealing`으로 거부된다. 지금은 호스트 `/proc`을 bind mount해서 우회하고 있다(4.5단계). 그래서 컨테이너 안 `ps`에 호스트 프로세스가 보인다.

**코드 주석에 적힌 원인**: overlay 마운트 자체 → 🔶 **아닐 가능성이 높음**

**확인한 것**
- 호스트 `/proc` 아래에 `/proc/sys/fs/binfmt_misc` (autofs + binfmt_misc) 하위 마운트가 있다. 하지만 커널이 이 경로를 "항상 빈 디렉토리"로 등록하므로 원인일 가능성은 낮다.

**유력한 가설: 마운트하는 순서**
- 커널 조건(`mount_too_revealing` → `mnt_already_visible`): **지금 이 mount ns 안에** 가려진 곳 없이 전부 보이는 proc 마운트가 이미 있어야 한다.
- 원래 코드는 7단계(`umount2("/.old_root", MNT_DETACH)`)에서 호스트 `/proc`까지 같이 떼어낸 **뒤에**, 9단계에서 새 procfs를 마운트했다. 그 시점에 ns 안에 proc 마운트가 없으니 거부된다.
- self bind(3단계)를 지워도 결과가 같았던 것도 이걸로 설명된다.
- 4.5단계 bind mount가 동작하는 것도 같은 이유다. 호스트 `/proc`이 아직 보일 때 실행되니까.

**검증 방법** (sudo 필요)
```bash
mkdir -p /tmp/p
# A: /proc가 보이는 상태 → 성공해야 함
sudo unshare -Urmpf --propagation private sh -c 'mount -t proc proc /tmp/p && echo 성공 && ls /tmp/p | grep -E "^[0-9]+$"'
# B: /proc를 먼저 떼어낸 상태 → EPERM, dmesg에 "Mount too revealing"이 찍혀야 함
sudo unshare -Urmpf --propagation private sh -c 'umount -l /proc && mount -t proc proc /tmp/p && echo 성공'
```

**관찰 포인트**: PID 격리는 "PID ns"와 "procfs 인스턴스" 두 겹이다. 지금 내 컨테이너는 PID ns는 분리되어 있지만 procfs는 호스트 것을 쓴다. eBPF의 `container_pid`나 `/proc/self/status`의 `NSpid`로 보면 PID ns 분리는 확인된다.

---

## 4. 발견된 버그

### 4-1. ps 유령 + PID 재사용 위험 ✅ (미수정)
- 2026-11-10 기준: 컨테이너 디렉토리 1,841개 중 **state 파일이 748개 남아 있었고, 그중 PID가 살아 있는 것은 0개.** `mycontainer_ps`가 748개 전부 running으로 표시했다.
- 원인 🔶: `mycontainer_run`이 Ctrl+C(SIGINT)로 죽으면 `waitpid` 뒤에 있는 `remove_state_file()`까지 가지 못한다.
- **위험**: 죽은 컨테이너의 PID는 다른 프로세스에 다시 배정될 수 있다. 이때 `mycontainer_stop`을 쓰면 상관없는 호스트 프로세스에 SIGTERM이나 SIGKILL을 보낼 수 있다.
- 수정 방향: state 파일에 `starttime`(`/proc/<pid>/stat`의 22번째 필드)을 같이 기록하고, PID와 starttime이 둘 다 맞을 때만 살아 있는 걸로 판단한다. `kill(pid, 0)`만으로는 부족하다.

### 4-2. 컨테이너 디렉토리가 쌓임 ✅ (미수정)
- upper/work/merged를 지우는 코드가 없다. 위 시점 기준 416MB.

---

## 5. 설계 결정 로그

### 5-1. eBPF를 런타임에서 분리 (2026-11-10, `5946b7d`)
- **문제**: `mycontainer_run.c`에서 libbpf를 쓰게 되자 빌드가 깨졌다(`-lbpf` 없음). `-lbpf`를 붙이면 런타임 전체가 빌드 시점과 실행 시점 모두 `libbpf.so`에 의존하게 된다. 코드에서는 "맵이 없어도 진행"하도록 짜여 있었지만, 이 의도가 실행 중 단계에만 반영되는 모순이 있었다.
- **결정**: `mycontainer_run`에서 bpf 코드를 모두 빼고, `register_cgroup add|del`을 `run_container.sh`가 컨테이너 실행 전후로 호출하도록 바꿨다.
- **비교 관점의 이점**: 같은 관측기를 Docker 컨테이너에도 붙일 수 있다.
  ```bash
  sudo ./register_cgroup add /sys/fs/cgroup/system.slice/docker-<full-id>.scope
  ```
  경로는 systemd cgroup driver 기준이다.

---

## 6. 아직 기록할 차이 (관찰 대상, 미실측 ⬜)

| 항목 | 내 컨테이너 | Docker(rootful) 기본값 |
|---|---|---|
| user ns | 사용 (ns 0 → 1000) | 사용 안 함 (`userns-remap`은 옵트인) |
| IPC ns | 없음 | 있음 |
| cgroup ns | 없음 | 있음 (cgroup v2) |
| procfs | 호스트 `/proc` bind | 새 인스턴스 + masked paths (`/proc/kcore` 등) |
| capability | user ns 안에서 전부 | 14개 정도의 기본 목록 |
| seccomp | 없음 | 기본 프로파일 |
| `/sys` | (확인 필요) | 읽기 전용 |
| `/dev` | rootfs 그대로 | tmpfs에 최소 장치만 |
| PID 1 | 사용자 명령 그대로 | 사용자 명령 그대로 (`--init` 옵션으로 tini) |

**공통 측정 방법**: 두 컨테이너의 PID 1에 대해 같은 명령을 실행한다.
```bash
ls -l /proc/$PID/ns/
grep Cap /proc/$PID/status
cat /proc/$PID/uid_map
cat /proc/$PID/cgroup
sudo nsenter -t $PID -m findmnt
```
Docker 쪽 PID는 `docker inspect -f '{{.State.Pid}}' <id>`, 내 컨테이너는 `mycontainer_ps`로 구한다.

---

## 7. 다음 할 일
- [ ] §4-1 ps 유령과 PID 재사용 수정
- [ ] §3 procfs A/B 실험
- [ ] Docker 대조 실험: §2-2 ping, §2-3 stop/`--init`, §6 공통 측정
- [ ] Docker 컨테이너에 eBPF 관측기 붙여보기
