#!/bin/bash
set -e
cd "$(dirname "$0")"
gcc -o chroot_escape chroot_escape.c
echo "빌드 완료: chroot_escape"
