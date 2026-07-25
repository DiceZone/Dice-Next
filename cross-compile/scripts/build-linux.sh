#!/bin/bash
# Linux 构建脚本 (amd64/arm64)
# 用法: ./build-linux.sh [amd64|arm64]

set -e

ARCH=${1:-amd64}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SERVER_DIR="$PROJECT_ROOT/server"

echo "构建 Linux $ARCH..."

# 检查 vcpkg
if [ -z "$VCPKG_ROOT" ]; then
    echo "错误: 未设置 VCPKG_ROOT 环境变量"
    echo "请先安装 vcpkg 并设置 VCPKG_ROOT"
    exit 1
fi

# 安装依赖
echo "安装 vcpkg 依赖..."
if [ "$ARCH" = "arm64" ]; then
    TRIPLET="arm64-linux-dynamic"
    export CMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
else
    TRIPLET="x64-linux-dynamic"
fi

$VCPKG_ROOT/vcpkg install \
    drogon[sqlite3] \
    nlohmann-json \
    sqlite-orm \
    spdlog \
    yaml-cpp \
    zstd \
    quickjs-ng \
    lua \
    --triplet=$TRIPLET

# 配置 CMake
echo "配置 CMake..."
cd "$SERVER_DIR"
if [ "$ARCH" = "arm64" ]; then
    cmake -B build-arm64 -S . \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=arm64-linux-dynamic
    BUILD_DIR="build-arm64"
else
    cmake -B build -S . \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
    BUILD_DIR="build"
fi

# 编译
echo "编译..."
cmake --build $BUILD_DIR --config Release --parallel $(nproc)

echo "✓ Linux $ARCH 构建完成"
echo "输出: $SERVER_DIR/$BUILD_DIR/dice-next-server"
