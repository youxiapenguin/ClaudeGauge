#!/bin/bash
# 编译独立任务栏程序 -> ClaudeGauge.exe (x64, GUI 子系统)
set -e
cd "$(dirname "$0")"
CXX=""
for c in x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++; do
    command -v "$c" >/dev/null 2>&1 && { CXX="$c"; break; }
done
[ -z "$CXX" ] && { echo "缺 mingw：sudo apt install -y mingw-w64"; exit 1; }
echo "编译器：$CXX"

"$CXX" -std=c++17 -O2 -mwindows \
    -static -static-libgcc -static-libstdc++ \
    -o ClaudeGauge.exe ClaudeTaskbar.cpp \
    -lgdi32 -luser32 -lkernel32 -ladvapi32 -lcomctl32 -lgdiplus -lshell32 -lole32 -lwininet

echo "==> 已生成 $(pwd)/ClaudeGauge.exe"
ls -la ClaudeGauge.exe
