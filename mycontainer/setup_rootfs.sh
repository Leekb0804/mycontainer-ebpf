#!/bin/bash
# busybox-static 패키지를 이용해 busybox_rootfs 디렉토리를 생성한다.
# 생성 결과물(바이너리 + 심볼릭 링크 수백 개)은 용량 문제로 git에 커밋하지 않고,
# 대신 이 스크립트로 언제든 재생성한다.
set -e

ROOTFS_DIR="$(cd "$(dirname "$0")" && pwd)/busybox_rootfs"

if ! command -v busybox >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y busybox-static
fi

mkdir -p "$ROOTFS_DIR"/{bin,dev,etc,proc,sys,tmp,usr/bin,usr/sbin}

cp "$(command -v busybox)" "$ROOTFS_DIR/bin/busybox"
chmod +x "$ROOTFS_DIR/bin/busybox"

for applet in $("$ROOTFS_DIR/bin/busybox" --list); do
    [[ "$applet" == "busybox" ]] && continue
    ln -sf busybox "$ROOTFS_DIR/bin/$applet"
done

sudo mknod -m 666 "$ROOTFS_DIR/dev/null" c 1 3
sudo mknod -m 666 "$ROOTFS_DIR/dev/zero" c 1 5
sudo mknod -m 666 "$ROOTFS_DIR/dev/random" c 1 8
sudo mknod -m 666 "$ROOTFS_DIR/dev/urandom" c 1 9

echo "busybox_rootfs 생성 완료: $ROOTFS_DIR"
