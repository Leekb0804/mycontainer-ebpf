#!/bin/bash
# before/after mycontainer_run 비교. sudo로 실행: sudo bash verify.sh
# 만드는 것: cgroup mc-verify-*, netns mc-verify-*, 컨테이너 디렉토리 mc-verify-* (끝나면 전부 삭제)
# 건드리지 않는 것: sysctl, iptables, br0, eBPF(register_cgroup)
set -u
S="$(cd "$(dirname "$0")" && pwd)"
ROOTFS=/home/dev/practice/mycontainer/busybox_rootfs
AFTER=/home/dev/practice/mycontainer/src/mycontainer_run
BEFORE="$S/mycontainer_run.before"
: "${SUDO_UID:?sudo로 실행해야 함}"

INNER='echo "HOSTNAME=$(hostname)"; echo "SHELL_PID=$$"; id;
grep -E "^(Uid|Gid|CapEff)" /proc/self/status;
echo "PROC_SELF=$(readlink /proc/self)"; echo "NPID=$(ls /proc | grep -c "^[0-9][0-9]*$")";
echo "CGROUP=$(cat /proc/self/cgroup)"; echo "NETDEVS=$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d " " | tr "\n" " ")";
grep -E " / | /proc " /proc/self/mounts; ls /.old_root 2>&1 | head -1'

run_one() {  # $1=label $2=binary $3=mode(tty|pipe) [$4=lowerdir 덮어쓰기]
    local L=$1 BIN=$2 MODE=$3 LOWER=${4:-$ROOTFS}
    local N="mc-verify-$L-$MODE" CG="/sys/fs/cgroup/mc-verify-$L-$MODE"
    local CD="/var/lib/mycontainer/containers/mc-verify-$L-$MODE" OUT="$S/out.$L.$MODE"
    mkdir "$CG"; ip netns add "$N"; mkdir -p "$(dirname "$CD")"
    if [[ $MODE == tty ]]; then
        script -qec "$(printf '%q ' "$BIN" "$LOWER" "$CD" "$CG" "$N" /bin/sh -c "$INNER")" /dev/null > "$OUT" 2>&1
    else
        "$BIN" "$LOWER" "$CD" "$CG" "$N" /bin/sh -c "$INNER" > "$OUT" 2>&1
    fi
    echo "EXIT=$?" >> "$OUT"
    echo "STATE_LEFT=$(ls "$CD/state" 2>/dev/null || echo none)" >> "$OUT"
    umount -l "$CD/merged" 2>/dev/null
    rmdir "$CG" 2>/dev/null || echo "[!] cgroup 정리 실패 $CG"
    ip netns delete "$N"; rm -rf "$CD"
    # 비교용 정규화: 이름/PID 등 실행마다 달라지는 값
    sed -E -e "s/mc-verify-$L-$MODE/mc-verify-X/g" -e 's/\r$//' \
        -e 's/(PID=|STARTTIME=)[0-9]+/\1N/g' -e 's/^(PROC_SELF=)[0-9]+/\1N/' "$OUT" > "$OUT.norm"
}

for m in tty pipe; do
    run_one before "$BEFORE" $m
    run_one after  "$AFTER"  $m
done
# 실패 경로(errno 이름 출력 확인): 없는 lowerdir로 overlay 실패 유도
run_one before "$BEFORE" fail /nonexistent-lower
run_one after  "$AFTER"  fail /nonexistent-lower
chown -R "$SUDO_UID" "$S"
for m in tty pipe fail; do
    echo "===== $m: diff before after ====="
    diff "$S/out.before.$m.norm" "$S/out.after.$m.norm" && echo "(동일)"
done
