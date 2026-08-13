#!/bin/bash
set -e
cd "$(dirname "$0")"
gcc -o pivot_root_container pivot_root_container.c
echo "빌드 완료: pivot_root_container"
