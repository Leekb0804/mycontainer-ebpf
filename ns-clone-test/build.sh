#!/bin/bash
set -e
cd "$(dirname "$0")"
go build -o main main.go
echo "빌드 완료: main"
