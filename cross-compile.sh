#!/bin/bash
# Dice!Next 交叉编译主脚本
# 用法: ./cross-compile.sh [平台] [选项]
#
# 平台:
#   linux-amd64     编译 Linux AMD64
#   linux-arm64      编译 Linux ARM64
#   windows-amd64    编译 Windows AMD64
#   windows-arm64    编译 Windows ARM64
#   macos-arm64      编译 macOS ARM64
#   all               编译所有平台 (需要相应环境)
#   docker-linux      使用 Docker 编译 Linux (在任意平台运行)
#
# 选项:
#   --build-only      仅编译，不打包
#   --package-only   仅打包 (需要先编译)
#   --help           显示帮助

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

show_help() {
    cat << 'EOF'
Dice!Next 交叉编译脚本

用法: ./cross-compile.sh [平台] [选项]

平台:
  linux-amd64      编译 Linux AMD64 (需要 Linux 环境)
  linux-arm64      编译 Linux ARM64 (需要 Linux 环境 + ARM64 工具链)
  windows-amd64    编译 Windows AMD64 (需要 Windows 环境 + vcpkg)
  windows-arm64    编译 Windows ARM64 (需要 Windows 环境 + ARM64 工具链)
  macos-arm64      编译 macOS ARM64 (需要 macOS ARM64 环境)
  all               编译所有平台 (需要相应环境或 Docker)
  docker-linux      使用 Docker 编译 Linux AMD64/ARM64 (跨平台)

选项:
  --build-only      仅编译，不打包
  --package-only   仅打包 (需要先编译)
  --help           显示此帮助

示例:
  # 在 Linux 上编译 Linux AMD64
  ./cross-compile.sh linux-amd64

  # 使用 Docker 编译 (任意平台)
  ./cross-compile.sh docker-linux

  # 编译并打包所有平台 (需要 GitHub Actions 或相应环境)
  ./cross-compile.sh all

注意事项:
1. 需要安装 vcpkg 并设置 VCPKG_ROOT 环境变量
2. Windows ARM64 和 macOS ARM64 的编译需要相应平台
3. 推荐使用 GitHub Actions 进行自动化构建 (见 .github/workflows/cross-compile.yml)
4. Docker 方式可以在任意平台编译 Linux 版本

快速开始:
1. 安装 vcpkg: git clone https://github.com/microsoft/vcpkg.git && cd vcpkg && ./bootstrap-vcpkg.sh
2. 设置环境变量: export VCPKG_ROOT=/path/to/vcpkg
3. 运行脚本: ./cross-compile.sh linux-amd64
EOF
}

PLATFORM=""
BUILD_ONLY=false
PACKAGE_ONLY=false

while [[ $# -gt 0 ]]; do
    case $1 in
        linux-amd64|linux-arm64|windows-amd64|windows-arm64|macos-arm64|all|docker-linux)
            PLATFORM=$1
            shift
            ;;
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        --package-only)
            PACKAGE_ONLY=true
            shift
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            echo "错误: 未知选项 $1"
            show_help
            exit 1
            ;;
    esac
done

if [ -z "$PLATFORM" ]; then
    echo "错误: 请指定平台"
    show_help
    exit 1
fi

# Docker Linux 编译
if [ "$PLATFORM" = "docker-linux" ]; then
    echo "使用 Docker 编译 Linux AMD64/ARM64..."
    echo "构建 Docker 镜像..."
    docker build -t dice-next:linux-amd64 -f cross-compile/docker/Dockerfile.linux-amd64 .
    docker build -t dice-next:linux-arm64 -f cross-compile/docker/Dockerfile.linux-arm64 .

    echo "运行 Docker 编译..."
    docker run --rm -v $(pwd):/workspace dice-next:linux-amd64 bash -c "cd /workspace && cross-compile/scripts/build-linux.sh amd64 && cross-compile/scripts/package-linux.sh amd64"
    docker run --rm -v $(pwd):/workspace dice-next:linux-arm64 bash -c "cd /workspace && cross-compile/scripts/build-linux.sh arm64 && cross-compile/scripts/package-linux.sh arm64"

    echo "✓ Docker 编译完成"
    echo "输出: release/"
    exit 0
fi

# 检查 vcpkg
if [ -z "$VCPKG_ROOT" ]; then
    echo "警告: 未设置 VCPKG_ROOT 环境变量"
    echo "尝试使用 ./vcpkg..."
    if [ -d "$SCRIPT_DIR/vcpkg" ]; then
        export VCPKG_ROOT="$SCRIPT_DIR/vcpkg"
        echo "使用 $VCPKG_ROOT"
    else
        echo "错误: 未找到 vcpkg"
        echo "请安装 vcpkg 并设置 VCPKG_ROOT"
        exit 1
    fi
fi

# 编译函数
build_platform() {
    local platform=$1
    echo "=== 编译 $platform ==="

    case $platform in
        linux-amd64)
            if [ "$PACKAGE_ONLY" = false ]; then
                bash cross-compile/scripts/build-linux.sh amd64
            fi
            if [ "$BUILD_ONLY" = false ]; then
                bash cross-compile/scripts/package-linux.sh amd64
            fi
            ;;
        linux-arm64)
            if [ "$PACKAGE_ONLY" = false ]; then
                bash cross-compile/scripts/build-linux.sh arm64
            fi
            if [ "$BUILD_ONLY" = false ]; then
                bash cross-compile/scripts/package-linux.sh arm64
            fi
            ;;
        macos-arm64)
            if [ "$PACKAGE_ONLY" = false ]; then
                bash cross-compile/scripts/build-macos.sh
            fi
            if [ "$BUILD_ONLY" = false ]; then
                bash cross-compile/scripts/package-macos.sh
            fi
            ;;
        *)
            echo "错误: 不支持的平台 $platform (需要在相应平台上运行)"
            echo "建议使用 GitHub Actions 进行自动化构建"
            exit 1
            ;;
    esac
}

# 执行编译
if [ "$PLATFORM" = "all" ]; then
    echo "编译所有平台..."
    for p in linux-amd64 linux-arm64; do
        build_platform $p
    done
    echo "✓ 所有平台编译完成"
    echo "输出: release/"
else
    build_platform $PLATFORM
fi
