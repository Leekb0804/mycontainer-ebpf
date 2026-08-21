#!/bin/bash
set -e

# 소스가 바뀌었거나 바이너리가 없으면 재컴파일
if [[ ! -x ./mycontainer_run || mycontainer_run.c -nt ./mycontainer_run ]]; then
    echo "[*] mycontainer_run.c 컴파일 중..."
    gcc -Wall -o mycontainer_run mycontainer_run.c
fi

MEMORY="max"
CPU="max"
PIDS="max"
ROOTFS_PATH="/home/dev/practice/mycontainer/busybox_rootfs"   # 이번엔 lowerdir(이미지 레이어)로 사용됨
CONTAINER_DIR="/var/lib/mycontainer/containers/mycontainer-$$"  # upper/work/merged를 담을 디렉토리
CMD=()

if [[ "$1" == "run" ]]; then
    shift
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --memory=*) MEMORY="${1#*=}"; shift ;;
        --cpu=*) CPU="${1#*=}"; shift ;;
        --pids=*) PIDS="${1#*=}"; shift ;;
        --)
            shift
            CMD=("$@")
            break
            ;;
        *) ROOTFS_PATH="$1"; shift ;;
    esac
done

if [[ "$CPU" == "max" ]]; then
    CPU_MAX_VALUE="max 100000"
else
    PERCENT="${CPU%\%}"
    QUOTA=$(( PERCENT * 100000 / 100 ))
    CPU_MAX_VALUE="$QUOTA 100000"
fi

echo "CPU_MAX_VALUE=$CPU_MAX_VALUE"

CGROUP_NAME="mycontainer-$$"
CGROUP_PATH="/sys/fs/cgroup/$CGROUP_NAME"

sudo mkdir "$CGROUP_PATH"
echo "$MEMORY" | sudo tee "$CGROUP_PATH/memory.max" > /dev/null
echo "$CPU_MAX_VALUE" | sudo tee "$CGROUP_PATH/cpu.max" > /dev/null
echo "$PIDS" | sudo tee "$CGROUP_PATH/pids.max" > /dev/null

NETNS_NAME="mycontainer-$$"
VETH_HOST="v-$$"
VETH_PEER="v-$$-p"

sudo ip netns add "$NETNS_NAME"
sudo ip link add "$VETH_HOST" type veth peer name "$VETH_PEER"
sudo ip link set "$VETH_PEER" netns "$NETNS_NAME"

VETH_PEER_IP="10.0.0.11/24"
BRIDGE_IP="10.0.0.1/24"
BRIDGE_GATEWAY_IP="10.0.0.1"

sudo ip netns exec "$NETNS_NAME" ip addr add "$VETH_PEER_IP" dev "$VETH_PEER"
sudo ip netns exec "$NETNS_NAME" ip link set "$VETH_PEER" up
sudo ip netns exec "$NETNS_NAME" ip link set lo up

sudo ip link show br0 2> /dev/null || {
    sudo ip link add br0 type bridge
    sudo ip link set br0 up
    sudo ip addr add "$BRIDGE_IP" dev br0
    sudo sysctl -w net.ipv4.ip_forward=1
    sudo iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -o enp0s1 -j MASQUERADE
    sudo iptables -A FORWARD -i br0 -o enp0s1 -j ACCEPT
    sudo iptables -A FORWARD -i enp0s1 -o br0 -m state --state RELATED,ESTABLISHED -j ACCEPT
}

sudo ip link set "$VETH_HOST" master br0
sudo ip link set "$VETH_HOST" up

sudo ip netns exec "$NETNS_NAME" ip route add default via "$BRIDGE_GATEWAY_IP"

# 상위 디렉토리 미리 생성하기.
sudo mkdir -p "$(dirname "$CONTAINER_DIR")"

# --- 여기만 기존 pivot_root_container에서 mycontainer_run으로 교체 ---
# 인자 순서: <lowerdir> <컨테이너 작업디렉토리> <cgroup 경로> <netns 이름> <실행할 프로그램...>
./mycontainer_run "$ROOTFS_PATH" "$CONTAINER_DIR" "$CGROUP_PATH" "$NETNS_NAME" "${CMD[@]}" || true
sudo rmdir "$CGROUP_PATH" || echo "[!] cgroup 정리 실패: $CGROUP_PATH (계속 진행)"
sudo ip netns delete "$NETNS_NAME" || echo "[!] netns 정리 실패: $NETNS_NAME (계속 진행)"
