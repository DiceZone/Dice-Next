# Dice!Next 交叉编译指南

本目录包含 Dice!Next 的交叉编译配置和脚本，支持以下平台：
- ✅ Linux AMD64
- ✅ Linux ARM64 (aarch64)
- ✅ Windows AMD64
- ✅ Windows ARM64
- ✅ macOS ARM64 (Apple Silicon)

---

## 📋 目录结构

```
cross-compile/
├── vcpkg-triplets/          # vcpkg 交叉编译配置
│   ├── arm64-linux-dynamic.cmake
│   ├── arm64-windows-dynamic.cmake
│   └── arm64-linux-toolchain.cmake
├── docker/                   # Docker 构建环境
│   ├── Dockerfile.linux-amd64
│   └── Dockerfile.linux-arm64
└── scripts/                  # 构建和打包脚本
    ├── build-linux.sh        # Linux 构建脚本
    ├── build-windows.ps1    # Windows 构建脚本
    ├── build-macos.sh       # macOS 构建脚本
    ├── package-linux.sh     # Linux 打包脚本
    ├── package-windows.ps1  # Windows 打包脚本
    └── package-macos.sh     # macOS 打包脚本

.github/workflows/
└── release.yml               # GitHub Actions 自动构建与发布

cross-compile.sh              # 主 orchestration 脚本 (Linux/macOS)
```

发布前请将 `Dice-Next`、`Dice-Next-WebUI` 和 `Dice-Next-Doc` 放在同一父目录。GitHub Actions 会从 DiceZone 组织下的同名仓库检出前端和文档；若三个仓库尚未公开，需要配置可读取它们的 `DICEZONE_CI_TOKEN` secret。

---

## 🚀 快速开始

### 方式一：GitHub Actions (推荐)

最简单的方案，使用 GitHub Actions 自动化构建所有平台：

1. **推送代码到 GitHub**
   ```bash
   git push origin main
   ```

2. **创建标签触发构建**
   ```bash
   git tag v3.0.0
   git push origin v3.0.0
   ```

3. **下载构建产物**
   - 进入 GitHub 仓库 → Actions → 选择 workflow run
   - 下载所有平台的构建产物 (Artifacts)
   - 或等待自动创建 Release 并下载

GitHub Actions 使用原生 runner 构建，无需处理交叉编译工具链。

---

### 方式二：Docker 编译 Linux (跨平台)

在任意平台 (Windows/macOS/Linux) 上使用 Docker 编译 Linux 版本：

```bash
# 构建 Docker 镜像并编译
./cross-compile.sh docker-linux

# 输出: release/DiceNext-linux-*.tar.gz
```

**前置要求**:
- 安装 Docker Desktop (Windows/macOS) 或 Docker Engine (Linux)

---

### 方式三：本地编译

#### Linux AMD64

```bash
# 1. 安装依赖
sudo apt-get install -y build-essential cmake git wget curl pkg-config libssl-dev libsqlite3-dev

# 2. 安装 vcpkg
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)
cd ..

# 3. 编译并打包
./cross-compile.sh linux-amd64

# 输出: release/DiceNext-linux-amd64-*.tar.gz
```

#### Linux ARM64

```bash
# 1. 安装交叉编译工具链
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# 2. 设置 vcpkg (同上)

# 3. 编译并打包
./cross-compile.sh linux-arm64

# 输出: release/DiceNext-linux-arm64-*.tar.gz
```

#### Windows AMD64

```powershell
# 1. 安装 Visual Studio 2022 (带 C++ 开发工具)
# 2. 安装 vcpkg
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = (Get-Location).Path
cd ..

# 3. 编译并打包
.\cross-compile\scripts\build-windows.ps1 -Architecture amd64
.\cross-compile\scripts\package-windows.ps1 -Architecture amd64

# 输出: release/DiceNext-windows-amd64-*.zip
```

#### Windows ARM64 (实验性)

```powershell
# 需要 Visual Studio 2022 17.4+ (带 ARM64 交叉编译工具)
# 1. 安装 vcpkg (同上)

# 2. 编译并打包
.\cross-compile\scripts\build-windows.ps1 -Architecture arm64
.\cross-compile\scripts\package-windows.ps1 -Architecture arm64

# 输出: release/DiceNext-windows-arm64-*.zip
```

**注意**: Windows ARM64 交叉编译可能需要手动配置，vcpkg 对 ARM64 Windows 的支持仍在完善中。

#### macOS ARM64

```bash
# 1. 安装 Homebrew 和依赖
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install cmake git wget curl pkg-config openssl sqlite

# 2. 安装 vcpkg
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)
cd ..

# 3. 编译并打包
./cross-compile.sh macos-arm64

# 输出: release/DiceNext-macos-arm64-*.tar.gz
```

---

## 🔧 依赖说明

### vcpkg 依赖

项目使用 vcpkg 管理以下依赖：
- `drogon[sqlite3]` - HTTP 框架 + WebSocket + SQLite3
- `nlohmann-json` - JSON 解析
- `sqlite-orm` - SQLite ORM
- `spdlog` - 日志库
- `yaml-cpp` - YAML 解析
- `quickjs-ng` - JavaScript 引擎
- `lua` - Lua 引擎

### 系统依赖

**Linux**:
- `libssl-dev` - OpenSSL
- `libsqlite3-dev` - SQLite3
- `pkg-config` - 依赖查找

**macOS**:
- `openssl` - OpenSSL (通过 Homebrew)
- `sqlite` - SQLite3 (通过 Homebrew)

**Windows**:
- Visual Studio 2022 (带 C++ 开发工具)
- Windows SDK

---

## 📦 打包格式

| 平台 | 格式 | 说明 |
|------|------|------|
| Linux AMD64/ARM64 | `.tar.gz` | 包含可执行文件 + 运行脚本 |
| Windows AMD64/ARM64 | `.zip` | 包含 exe + DLL + 使用说明 |
| macOS ARM64 | `.tar.gz` | 包含可执行文件 + 运行脚本 |

---

## ⚠️ 已知限制

1. **Windows ARM64**: vcpkg 对 ARM64 Windows 的交叉编译支持有限，可能需要手动配置。
2. **macOS 交叉编译**: Apple 不允许在非 Apple 硬件上交叉编译 macOS 应用，必须在 Mac 上编译。
3. **Linux ARM64**: 可以使用 Docker + QEMU 模拟 ARM64 环境，但原生 ARM64 机器编译速度更快。

---

## 🐛 故障排除

### vcpkg 安装依赖失败

```bash
# 尝试更新 vcpkg
cd $VCPKG_ROOT
git pull
./bootstrap-vcpkg.sh

# 清除缓存并重试
rm -rf $VCPKG_ROOT/buildtrees $VCPKG_ROOT/packages
$VCPKG_ROOT/vcpkg install ...
```

### CMake 找不到 vcpkg 依赖

```bash
# 确保正确指定 toolchain 文件
cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

### Linux ARM64 交叉编译失败

```bash
# 确保安装了交叉编译工具链
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# 检查 toolchain 文件是否正确
cat cross-compile/vcpkg-triplets/arm64-linux-toolchain.cmake
```

---

## 📚 参考资料

- [vcpkg 官方文档](https://vcpkg.io/)
- [CMake 交叉编译指南](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)
- [GitHub Actions 文档](https://docs.github.com/en/actions)
- [Docker 多平台构建](https://docs.docker.com/build/building/multi-platform/)

---

## 🤝 贡献

如果你改进了交叉编译流程，欢迎提交 PR！

特别是以下方面：
- Windows ARM64 交叉编译优化
- macOS AMD64 (Intel) 支持
- 更多 Linux 发行版支持

---

**最后更新**: 2026-06-22
