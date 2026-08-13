#!/bin/bash
set -e

MEMORY="max"
CPU="max"
PIDS="max"
ROOTFS_PATH="/home/dev/mycontainer/busybox_rootfs"
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

sudo ./pivot_root_container "$ROOTFS_PATH" "$CGROUP_PATH" "$NETNS_NAME" "${CMD[@]}"

if ! sudo rmdir "$CGROUP_PATH"; then
    echo "[DEBUG] rmdir 실패! 진단 정보 수집 중..."
    echo "[DEBUG] cgroup.procs 내용:"
    cat "$CGROUP_PATH/cgroup.procs" 2>/dev/null
    echo "[DEBUG] 관련 프로세스 확인:"
    ps aux | grep -i "pivot_root_container\|busybox\|/bin/sh"
    echo "[DEBUG] cgroup 디렉토리 상태:"
    ls -la "$CGROUP_PATH"
fi

sudo ip netns delete "$NETNS_NAME"
