#!/bin/bash
# macOS ARM64 构建脚本
# 用法: ./build-macos.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SERVER_DIR="$PROJECT_ROOT/server"

echo "构建 macOS ARM64..."

# 检查依赖
if ! command -v brew &> /dev/null; then
    echo "错误: 未安装 Homebrew"
    echo "请先安装 Homebrew: /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
    exit 1
fi

# 安装系统依赖
echo "安装系统依赖..."
brew install cmake git wget curl zip unzip tar pkg-config openssl sqlite

# 检查 vcpkg
if [ -z "$VCPKG_ROOT" ]; then
    echo "警告: 未设置 VCPKG_ROOT 环境变量"
    echo "正在克隆 vcpkg 到 $PROJECT_ROOT/vcpkg..."
    git clone https://github.com/microsoft/vcpkg.git "$PROJECT_ROOT/vcpkg"
    export VCPKG_ROOT="$PROJECT_ROOT/vcpkg"
    $VCPKG_ROOT/bootstrap-vcpkg.sh
fi

# 安装 vcpkg 依赖
echo "安装 vcpkg 依赖..."
$VCPKG_ROOT/vcpkg install \
    drogon[sqlite3] \
    nlohmann-json \
    sqlite-orm \
    spdlog \
    yaml-cpp \
    zstd \
    quickjs-ng \
    lua \
    --triplet=arm64-osx-dynamic

# 配置 CMake
echo "配置 CMake..."
cd "$SERVER_DIR"
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# 编译
echo "编译..."
cmake --build build --config Release --parallel $(sysctl -n hw.ncpu)

echo "✓ macOS ARM64 构建完成"
echo "输出: $SERVER_DIR/build/dice-next-server"
