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

### 2-3-1. 컨테이너 PID 1의 두 시점과 고아 처리 (개념 정리, 실측 🔶)

**같은 프로세스를 두 시점에서 보는 것**
```
호스트 시점                                    컨테이너 시점
systemd                     PID 1
 └─ mycontainer_run  PID 1538300
     └─ ash          PID 1538391   ═══  ash  PID 1  (PPid: 0)
         └─ ls       PID 1538402   ═══   └─ ls  PID 2
```
- PID 번호는 namespace마다 따로 매겨진다. 부모-자식 관계는 커널이 namespace와 상관없이 하나로 관리한다.
- 컨테이너 init의 진짜 부모는 `mycontainer_run`이다. 컨테이너 안에서 `getppid()`가 0인 건 부모가 바깥 ns에 있어서 안 보이기 때문이다.
- 그래서 **부모-자식 관계와 고아 여부는 호스트 시점으로 봐야 한다.** 컨테이너 안에서는 "최상위"처럼 보여도 호스트에서는 고아가 될 수 있다.
- `clone()` 한 번에 프로세스 하나가 생기고, 그게 새 PID ns의 PID 1이다. `/proc/self/exe` 재실행이나 execvp는 같은 프로세스가 프로그램만 바꾸는 것이다.

**고아 입양 규칙** (커널 reparenting)
1. 죽은 부모의 조상 중에 subreaper(`PR_SET_CHILD_SUBREAPER`)가 있으면 그 프로세스에게 입양된다.
2. 없으면 **죽은 부모가 속한 PID ns의 init**에게 입양된다.
- `mycontainer_run`(호스트 ns)이 죽으면 컨테이너 init은 systemd에게 입양된다. systemd는 입양한 자식이 끝나면 `wait()`로 바로 거두므로 좀비로 남지 않는다.

**거두는 역할은 두 층에 있다**

| 층 | 고아를 거두는 프로세스 |
|---|---|
| 호스트 | systemd |
| 컨테이너 | 컨테이너의 PID 1 (지금은 ash) |
| Docker ⬜ | containerd-shim이 subreaper로 컨테이너 init을 입양 |

- 컨테이너 PID 1이 init용 프로그램이 아니면 입양한 자식을 `wait()`하지 않을 수 있다. 그러면 컨테이너 안에 좀비가 쌓인다. 이게 §2-3의 SIGTERM 무시와 함께 Docker `--init`(tini)이 있는 두 번째 이유다.

**확인 방법** (지금 구조는 `/proc`이 호스트 것이라 컨테이너 안에서 두 시점을 동시에 볼 수 있다)
```sh
# 컨테이너 안
echo $$                                          # 1 (getpid: 컨테이너 시점)
grep -E '^(Pid|PPid|NSpid)' /proc/self/status    # Pid/PPid = 호스트 시점, NSpid = "호스트PID 1"
```
```bash
# 호스트: 고아 만들기
ps -o pid,ppid,comm -p <ash 호스트 PID>   # PPID = mycontainer_run
sudo kill <mycontainer_run PID>
ps -o pid,ppid,comm -p <ash 호스트 PID>   # PPID = 1 (systemd)
sudo ./mycontainer_ps                     # 고아 컨테이너가 running으로 나와야 함 (§4-1 수정 검증)
```

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
- 수정 여부: **미정**. 지금은 비교 기록으로만 남겨둔다. 고친다면 `setns(CLONE_NEWUSER)` 후 `setgid(0)`/`setuid(0)` 처리가 필요하다.

---

## 3. procfs "Mount too revealing": 원인은 마운트 순서 ✅ (2026-11-11 실험으로 확정)

**현상**: 새 procfs를 마운트하면 `VFS: Mount too revealing`으로 거부됐다. 그래서 한동안 호스트 `/proc`을 bind mount해서 우회했고, 컨테이너 안 `ps`에 호스트 프로세스가 보였다.

**수정 (2026-11-13)**: 4.5단계(pivot_root 전)의 호스트 `/proc` bind를 새 procfs 마운트로 바꿨다 (`mycontainer_run.c` 4.5단계, `mount("proc", merged/proc, "proc", MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL)`). 틀린 원인 설명이 남아 있던 3단계·4.5단계 주석도 고쳤다. **실제 컨테이너로 검증함** ✅ (아래 "검증 결과").

**결론** ✅
- 원인은 **overlay나 self bind가 아니라 마운트하는 순서**다. 커밋 `fa28978`과 당시 `mycontainer_run.c` 주석(3단계, 4.5단계)에 적힌 "overlay가 원인"이라는 설명은 **틀렸다** (아래 D, G로 두 번 반증). 주석은 2026-11-13에 고쳤다.
- 커널 조건(`mount_too_revealing` → `mnt_already_visible`): user ns 안에서 새 procfs를 마운트하려면, **지금 이 mount ns 안에** 가려진 곳 없이 전부 보이는 proc 마운트가 이미 있어야 한다.
- **커널 원문 대조** ✅ (2026-11-13, upstream `v7.0`의 `fs/namespace.c`. 실행 중인 Ubuntu `7.0.0-27`과는 패치 차이가 있을 수 있다)
  - **한 문장 요약**: 새 procfs 마운트는 `do_new_mount_fc()`에서 `mount_too_revealing()`이 true면 `"Mount too revealing"`을 찍고 `-EPERM`을 돌려준다. 이 함수는 mount ns의 소유 user ns가 init이 아니고 fs가 `SB_I_USERNS_VISIBLE`(procfs는 `fs/proc/root.c:260`에서 설정)일 때 `!mnt_already_visible()`을 돌려준다. 그러므로 **현재 mount ns(`current->nsproxy->mnt_ns`)의 마운트 중에 "같은 fs 타입 + fs 루트를 통째로 마운트 + 잠긴 플래그 조건 통과 + 잠긴 하위 마운트가 영구히 빈 디렉토리만 가림"을 만족하는 것이 하나도 없으면 거부된다.**
  - 실험과 대응:
    - E/G: pivot_root 후 옛 root를 떼면 ns의 마운트 목록(`ns->mounts`)에 proc 타입이 하나도 없다. 루프가 `false`를 돌려주고 거부된다.
    - F: 아직 호스트 `/proc`(fs 루트 그대로)가 ns 안에 있으니 통과한다.
    - C(user ns 없음): `if (ns->user_ns == &init_user_ns) return false;`로 검사 자체를 건너뛴다.
    - H(bind): `mount_too_revealing`의 호출 지점은 `do_new_mount_fc()`(`:3774`)와 `fsmount()`(`:4461`) 두 곳뿐이다. bind mount 경로는 이 검사를 거치지 않는다.
  - 하위 마운트 조건은 `MNT_LOCKED`인 자식이 `is_empty_dir_inode()`가 아닌 곳을 가리면 그 후보를 건너뛰는 것이다. 호스트 `/proc/sys/fs/binfmt_misc`의 autofs/binfmt_misc 하위 마운트가 있어도 A/D/F가 성공했다. 이 실험 결과는 그 마운트 지점이 "영구히 빈 디렉토리"로 취급된다는 것과 맞는다. 다만 해당 sysctl 마운트 지점을 등록하는 코드 위치까지는 확인하지 않았다.
  - 덤: 통과하면 `*new_mnt_flags |= mnt_flags & (MNT_LOCK_READONLY | MNT_LOCK_ATIME)`로, 기존 proc의 잠긴 읽기 전용/atime 속성이 새 마운트에 그대로 옮겨진다.
- **runc와 대조** ✅ (2026-11-13, `opencontainers/runc` main의 `libcontainer/rootfs_linux.go`): `prepareRootfs()`는 rootfs를 self bind한 뒤 `for _, m := range config.Mounts { setupAndMountToRootfs(...) }`로 proc를 포함한 모든 마운트를 rootfs 아래에 먼저 건다. 마지막에 `pivotRoot(rootFd)`(`NoPivotRoot`이면 `msMoveRoot`, mount ns가 없으면 `chroot`)를 호출한다. proc는 `mountToRootfs`의 `case "proc", "sysfs":` 분기에서 `checkProcMount`를 거쳐 새로 마운트된다. 즉 runc는 처음부터 F 순서다. 호스트 `/proc`가 아직 mount ns에 보이는 동안 새 procfs를 마운트하므로 이 문제를 겪지 않는다.
- 원래 코드는 7단계(`umount2("/.old_root", MNT_DETACH)`)에서 호스트 `/proc`까지 같이 떼어낸 **뒤에**, 9단계에서 새 procfs를 마운트했다. 그 시점에 ns 안에 proc 마운트가 없으니 거부된다.
- 4.5단계 bind mount가 동작하는 것도 같은 이유다. 호스트 `/proc`이 아직 보일 때 실행되기 때문이다.
- 호스트 `/proc` 아래 `binfmt_misc` 하위 마운트는 원인이 아니다 (A, D, F가 이 하위 마운트가 있는 상태에서 성공).

**실험 1: unshare 단독** (`unshare -Urmpf --propagation private`, sudo)

| 케이스 | 조건 | 결과 |
|---|---|---|
| A | user ns 있음, `/proc` 보이는 상태에서 새 procfs | ✅ 성공, PID 디렉토리 4개 (새 PID ns) |
| B | user ns 있음, `umount -l /proc` 후 새 procfs | ⚠️ **무효**: `umount: /proc: not mounted` (umount 단계에서 실패) |
| C | user ns 없음, `umount -l /proc` 후 새 procfs | ✅ 성공 → 가시성 검사는 **user ns 안에서만** 걸린다 |
| D | user ns 있음, overlay + self bind 후 `/proc` 보이는 상태 | ✅ 성공, dmesg 경고 없음 → **overlay와 self bind는 원인이 아니다** |

- **B가 무효인 이유**: 덜 특권적인 user ns가 소유한 mount ns로 복사된 마운트는 잠긴다(locked). 잠긴 `/proc`는 하나만 떼어낼 수 없다(EINVAL → util-linux가 "not mounted"로 표시). 그래서 예전에 이 문서에 적어둔 B 명령으로는 순서 가설을 검증할 수 없었다. mycontainer는 옛 root를 **통째로** 떼어내므로 잠긴 하위 마운트까지 함께 떨어진다. 그래서 실험 2에서 pivot_root까지 재현했다.

**실험 2: mycontainer 순서 재현** (`experiments/procfs/procfs_order.c`, 로그 `experiments/procfs/logs/20261112-004932/procfs_order.log`. `mycontainer_run.c`(HEAD `cef41b5`) 경로를 그대로 따름: uid_map/gid_map `0 → 1000`, `setgroups=deny`, setgid(0)/setuid(0) 후 re-exec, `CapEff=000001ffffffffff`, busybox_rootfs, pivot_root → `umount2(MNT_DETACH)`)

| 케이스 | 순서 | 결과 | dmesg |
|---|---|---|---|
| E | overlay → self bind → pivot_root → 옛 root 떼어냄 → **그 뒤에** 새 procfs (원래 코드) | ❌ `Operation not permitted (errno=1)` | "Mount too revealing" +1 |
| G | E와 같은 순서, overlay 대신 bind | ❌ 같은 실패 | +1 |
| F | overlay → self bind → **pivot_root 전에** `merged/proc`에 새 procfs → pivot_root → 옛 root 떼어냄 | ✅ 성공, PID 디렉토리 **1개** (`NSpid: 1`) | 0 |
| H | 호스트 `/proc`를 rbind (예전 우회) | ✅ 성공, PID 디렉토리 **180개** (`NSpid: 1677718 1`) | 0 |

- **F를 `mycontainer_run.c`에 적용했다** (2026-11-13). 새 procfs 인스턴스라서 컨테이너 PID만 보인다 (1개 vs 180개). F가 1개인 이유: 재현 프로그램은 PID 1 자신이 직접 개수를 센다. 실제 컨테이너라면 셸과 `ps` 정도가 보일 것이다.
- 수치 정정 (2026-11-13): 이 표에는 예전에 "F 4개 / H 163개"라고 적혀 있었다. 그 수치는 그전에 `unshare` 셸 스크립트(`-S 0 -G 0`, setgroups deny 없음)로 돌린 결과이고, 로그는 저장하지 않았다. 지금 남아 있는 로그는 위 C 재현 프로그램의 것뿐이라서 로그 기준으로 바꿨다. 성공과 실패 패턴은 두 실행에서 같다.

**검증 결과 (2026-11-13, 실제 컨테이너)** ✅
```bash
cd ~/practice/mycontainer/src
sudo ./run_container.sh -- /bin/sh -c 'ls /proc | grep -cE "^[0-9]+$"; ps'
sudo dmesg | grep "Mount too revealing" | tail -1
```
- 1~10단계를 모두 통과했다. 4.5단계 로그는 `새 procfs -> .../merged/proc 마운트 (pivot_root 전)`.
- `/proc`의 PID 디렉토리는 **3개**였다. 세는 순간 살아 있던 sh(PID 1), ls, grep이다. 예전 우회(H)에서는 180개였다.
- `ps` 출력은 `1 0 {ps} /bin/sh -c ...` 한 줄이다. sh가 마지막 명령을 exec해서 ps가 PID 1이 됐다.
- dmesg의 마지막 "Mount too revealing"은 그대로 `[3123016.126547]`이다(11-12 실험 G에서 찍힌 줄). 새로 찍힌 경고는 없다.
- 주의: `sudo` 없이 `./run_container.sh`로 실행하면 `mycontainer_run`이 일반 사용자로 돈다(스크립트 101줄에만 sudo가 없다). 그러면 `cgroup.procs` 열기와 `setns(NET)`가 EPERM으로 실패해서 clone 전에 멈춘다. 2026-11-13에 실제로 겪었다.

**덤: 실험 스크립트를 만들다가 겪은 user ns 함정 2개** ✅
1. `unshare -r`(ns 0 → 호스트 0)로는 `/home/dev`(750, dev 소유)에 있는 busybox_rootfs에 들어가지 못했다(overlay, bind 둘 다 실패). user ns 안 root의 `CAP_DAC_OVERRIDE`는 **inode 소유자 uid가 그 ns에 매핑돼 있을 때만** 효력이 있다. dev(1000)는 매핑에 없으니 "기타 사용자" 권한으로 검사된다. §2-2 ping과 같은 원리다(capability는 어느 user ns 소속 자원에 대해서인가까지 포함). mycontainer는 `0 1000 1`로 매핑하므로 문제가 없었다.
2. `unshare --map-groups=... --setgroups=deny` → `write failed /proc/self/setgroups: Operation not permitted`. `setgroups`는 **gid_map을 쓰기 전에만** 바꿀 수 있다. unshare가 gid_map을 먼저 써버렸기 때문이다. `mycontainer_run.c`가 `deny_setgroups()`를 `write_id_map()`보다 먼저 호출하는 이유다.
- 참고: util-linux `--map-user=1000`은 "ns 안 1000 = 현재 사용자"라는 뜻이라 방향이 반대다. "ns 0 = 호스트 1000"은 `--map-users=0:1000:1`이다.

**관찰 포인트**: PID 격리는 "PID ns"와 "procfs 인스턴스" 두 겹이다. 예전 우회(H)는 PID ns는 분리했지만 procfs는 호스트 것을 썼다(H의 180개). 2026-11-13 수정으로 두 겹 모두 분리됐다(실제 컨테이너에서 PID 3개 확인 ✅). eBPF의 `container_pid`나 `/proc/self/status`의 `NSpid`로 보면 PID ns 분리는 확인된다.

---

## 4. 발견된 버그

### 4-1. ps 유령 + PID 재사용 위험 ✅ (수정함, 2026-11-11, 실제 컨테이너로 검증함)
- 2026-11-10 기준: 컨테이너 디렉토리 1,841개 중 **state 파일이 748개 남아 있었고, 그중 PID가 살아 있는 것은 0개.** `mycontainer_ps`가 748개 전부 running으로 표시했다.
- 원인 🔶: `mycontainer_run`이 신호(SIGINT, SIGHUP, kill 등)로 먼저 죽으면 `waitpid` 뒤에 있는 `remove_state_file()`까지 가지 못한다. 컨테이너 안에서 `exit`하거나 `mycontainer_stop`으로 끝내는 정상 경로에서는 run이 살아 있으니 유령이 생기지 않는다. (원인 자체는 아직 고치지 않음. 아래 수정으로 유령이 남아도 해가 없게 만듦)
- 단서 ✅:
  - 748개 모두 마지막 부팅(2026-10-05) **이후**에 생겼다. 그러니 재부팅은 원인이 아니다.
  - 생성 시각이 몰려 있다. 1분에 70개 넘게 생긴 구간이 여러 번 있다(10/25 14:31, 10/26 06:54~55, 07:29~30). 날짜로는 10/23~10/26에 668개가 몰려 있다.
  - 이 패턴은 스크립트로 반복 실행하다가 run이 계속 죽은 것으로 보인다. 사용자 기억으로는 eBPF 실험 중 스크립트로 반복 실행했다.
  - 2026-11-11 기준 남은 cgroup, netns, 호스트 veth, 좀비 프로세스는 **모두 0개**다. 남은 건 state 파일뿐이다.
  - 해석: `run_container.sh`는 살아서 `|| true` 다음의 정리 단계까지 실행했다. cgroup `rmdir`이 성공했다는 건 그 시점에 컨테이너 프로세스도 끝나 있었다는 뜻이다. 즉 **스크립트는 살고, run만 죽은** 경우다. 스크립트 전체를 죽였다면(`timeout`이나 Ctrl+C) cgroup이 남았을 것이다.
  - 원인 후보 🔶: ① 출력을 `| head` 같은 파이프로 넘겨서 run이 state 기록 직후 `printf`에서 SIGPIPE로 죽음, ② `pkill`로 run만 종료. 반복 실행하던 스크립트를 보면 판별할 수 있다.
- 유령 ≠ 좀비 ✅:
  - 좀비는 부모가 살아 있는데 `wait()`를 안 해서 프로세스 테이블에 항목이 남은 것이다.
  - run이 죽으면 컨테이너 init은 고아가 되어 systemd(호스트 PID 1)에게 입양된다. 입양된 뒤 끝나면 systemd가 바로 거두므로 좀비로 남지 않는다. 남는 건 디스크의 state 파일뿐이다.
  - 입양된 뒤에도 컨테이너 init이 계속 실행 중이면 "고아 컨테이너"다. 이 경우 (PID, starttime)이 맞으므로 새 ps는 running으로 정상 표시하고, stop으로 끌 수 있다.
- 질문을 나누면 이렇다:
  - "고아가 남았나?" → **프로세스**에 대한 질문: run이 죽은 뒤에도 컨테이너가 살아 있나. 자원과 보안 측면, 아무도 정리하지 않음.
  - "유령이 남았나?" → **기록**에 대한 질문: state 파일이 실제와 맞나. 도구가 올바르게 동작하느냐의 문제. 이번에 고친 건 이쪽이다.
  - 이번 748개는 유령이다. 고아였던 적이 있는지는 지금 알 수 없다(정리 시점에는 이미 끝나 있었음).
- Docker 대조 ⬜ (관리 프로세스가 죽었을 때 컨테이너는 어떻게 되나):
  - Docker 구조는 `dockerd → containerd → containerd-shim → 컨테이너 init`이다. runc는 컨테이너를 만들고 바로 종료하고, **shim**이 부모로 남는다. shim은 `PR_SET_CHILD_SUBREAPER`로 고아도 입양한다.
  - dockerd나 containerd가 재시작해도 컨테이너는 고아가 되지 않는다. 재시작한 데몬이 shim에 다시 붙는다(`live-restore`).
  - 내 구조에서 shim 역할은 `mycontainer_run`(clone 후 `waitpid`)이 한다. 하지만 run이 죽으면 대신할 게 없어서 컨테이너가 systemd에 입양된다.
  - 실험 아이디어: 컨테이너를 띄운 뒤 `mycontainer_run`만 `kill` → 컨테이너가 고아로 계속 도는지, 새 ps에 나오는지, stop으로 끌 수 있는지 확인. Docker에서는 `containerd`를 재시작했을 때 컨테이너가 유지되는지와 비교.
- "해가 없게"의 의미: 유령은 여전히 생기고 남는다. 하지만 ps, stop, exec가 그걸 믿고 행동하지 않는다. `kill -9`이나 OOM으로 run이 죽으면 어떤 정리 코드도 실행될 수 없으므로, 원인을 고쳐도 이 방어는 필요하다.
- **위험**: 죽은 컨테이너의 PID는 다른 프로세스에 다시 배정될 수 있다. 이때 `mycontainer_stop`을 쓰면 상관없는 호스트 프로세스에 SIGTERM이나 SIGKILL을 보낼 수 있다. `exec`도 그 프로세스의 namespace로 들어가게 된다.
- **왜 `kill(pid, 0)`으로는 부족한가**: "그 PID에 누군가 있다"만 알려줄 뿐 "같은 프로세스인지"는 모른다. 게다가 좀비 상태에도 성공을 돌려준다.

**수정 내용**
- 프로세스를 **(PID, starttime) 쌍**으로 식별한다. starttime은 `/proc/<pid>/stat`의 22번째 필드다. 공통 코드는 `src/mycontainer_state.h`에 있다.
  - run: clone 직후 자식의 starttime을 `STARTTIME=`으로 state 파일에 기록한다. execve를 거쳐도 starttime은 바뀌지 않는다.
  - ps: PID와 starttime이 둘 다 맞을 때만 표시하고, 걸러낸 유령은 개수만 출력한다.
  - stop: `pidfd_open` → 검증 → `pidfd_send_signal` 순서. 종료 대기는 `poll(pidfd)`로 한다.
  - exec: ns fd를 모두 연 **다음에** 검증한다.
  - STARTTIME이 없는 옛 state 파일은 검증할 수 없으므로 "죽은 것"으로 취급한다.
- **"먼저 열고, 그다음 검증"하는 순서가 핵심이다.** pidfd와 ns fd는 연 시점의 대상을 붙잡고 있으므로, 연 다음에 검증을 통과하면 그 fd가 가리키는 게 확실히 컨테이너 프로세스다. 검증을 먼저 하면 검증과 열기 사이에 PID가 바뀔 틈이 생긴다.
- `/proc/<pid>/stat` 파싱 함정: 2번째 필드(comm)에 공백이나 `)`가 들어갈 수 있다. 그래서 마지막 `)` 뒤부터 세어야 한다. 이름을 `a) b c 1 2 3`으로 바꾼 프로세스로 실험했을 때 단순하게 `awk '{print $22}'`로 세면 `0`이 나왔다.

**검증 (sudo 없이, 가짜 state 파일 + 직접 띄운 테스트 프로세스)** ✅

| 시나리오 | 결과 |
|---|---|
| ps, 실제 데이터 | 유령 748개 전부 걸러짐, running 0개 |
| stop, 죽은 PID | `pidfd_open` ESRCH → "유령"으로 안내, exit 1 |
| stop, PID 재사용 (살아 있는 다른 프로세스, STARTTIME 불일치) | 신호 안 보냄, 대상 프로세스 생존 |
| stop, 옛 형식 (STARTTIME 없음) | 신호 안 보냄 |
| stop, 정상 | SIGTERM → 즉시 종료 확인 |
| stop, SIGTERM 무시 | 10초 → SIGKILL → 종료 확인. 부모가 아직 거두지 않은 **좀비 상태도 `poll(pidfd)`가 종료로 인식** |
| exec, STARTTIME 불일치 | setns 전에 거부 |
| exec, 일치 | 검증 통과 → `setns(PID)`까지 진행 (sudo가 아니라 거기서 EPERM, 예상대로) |

**검증 (sudo, 실제 컨테이너로 한 바퀴, 2026-11-11)** ✅

| 단계 | 결과 |
|---|---|
| `ps` (실행 중) | 살아 있는 컨테이너 1개만 표시 (PID 1548466), 유령 748개는 숨기고 안내 |
| `exec ... /bin/sh` | state의 STARTTIME 검증 통과 → setns(PID/NET/MNT/UTS), cgroup 가입 → ash 진입 |
| `stop` | SIGTERM → 10초 타임아웃 → SIGKILL → 종료 확인 (§2-3의 PID 1 규칙과 같음) |
| `ps` (종료 후) | 목록 비어 있음, 유령 수는 748개 그대로 (정상 종료라서 새 유령이 생기지 않음) |

- 덤: `exec`에 없는 경로(`/bin/s`)를 주면 setns와 cgroup 가입은 끝난 뒤 `execvp`에서 ENOENT로 실패한다. 경로는 컨테이너 mnt ns 기준으로 해석되므로 exec 직전까지 가야 실패를 알 수 있다.

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
| procfs | 새 인스턴스 (pivot_root 전에 마운트, 2026-11-13~). masked paths 없음 | 새 인스턴스 + masked paths (`/proc/kcore` 등) |
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
- [x] §4-1 ps 유령과 PID 재사용 수정 (실제 컨테이너로 ps/exec/stop 확인함)
- [ ] §4-1 원인 수정: `mycontainer_run`이 SIGINT로 죽어도 state를 정리하게 하기
- [ ] 남아 있는 유령 state 파일과 디렉토리 정리 (§4-2, 지우기 전에 확인 필요)
- [ ] §2-3-1 두 시점 확인 (`NSpid`) + 고아 만들기 실험 (`mycontainer_run`만 kill)
- [x] §3 procfs A/B 실험 (2026-11-11, 원인 = 마운트 순서로 확정)
- [x] §3 수정 F(pivot_root 전에 새 procfs 마운트)를 `mycontainer_run.c`에 적용 (2026-11-13, 컴파일 확인)
- [x] §3 수정 후 실제 컨테이너로 검증 (2026-11-13, PID 3개, dmesg 새 경고 없음)
- [x] §3 `fs/namespace.c`의 `mnt_already_visible` 원문과 대조 (2026-11-13, upstream v7.0)
- [ ] Docker 대조 실험: §2-2 ping, §2-3 stop/`--init`, §6 공통 측정
- [ ] Docker 컨테이너에 eBPF 관측기 붙여보기
