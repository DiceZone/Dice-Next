# Release 构建提速与测试门禁

## 2026-09-28 修复

之前 Windows 的 Actions 缓存键没有包含 runner 镜像版本。环境升级后，vcpkg 判定旧二进制不兼容并重编，但 Actions 因主键命中而不保存新结果。日志表现为“缓存命中 → Restored 0 package(s) → 全量重编 → not saving cache”。

### 依赖缓存

- 三个平台和测试任务共用 `.github/actions/setup-vcpkg`，vcpkg 源码和工具仍按固定 baseline 引导，不使用 latest 工具版本，也不禁用 ABI / 编译器校验。
- 只缓存 `vcpkg-cache` 中的二进制包，不缓存整个源码、installed、buildtrees。vcpkg 每次安装时检查 ABI 并解包当前环境可用的依赖。
- 缓存按系统、主机架构、目标 triplet、baseline、清单及准备逻辑哈希、runner 镜像区分。先恢复相同镜像，再尝试同一依赖族的其他镜像；是否可复用仍由 vcpkg 决定。
- 快照键带 run ID、attempt 和 job，避免不可变缓存无法刷新。安装前后比较 ABI 压缩包清单，只有新增/变更包或迁移镜像时才保存，避免每轮复制不变缓存。
- 保存发生在依赖安装成功之后、项目编译之前；后续编译或测试失败不丢失已构建依赖。并发任务生成相同内容的独立快照是允许的，不覆盖彼此。
- 新缓存命名空间第一次运行需要预热，仍可能较慢；不能保证第一轮立即回到十分钟。不要仅根据 Actions “cache hit” 判断依赖是否真正复用，应查看 vcpkg 恢复包数及耗时。

### 发布与测试

- Windows x64/ARM64、Linux x64/ARM64、macOS ARM64 打包时显式设置 `BUILD_TESTING=OFF`。
- 独立 Windows x64、Linux x64 任务设置 `BUILD_TESTING=ON`，只编译测试目标，并真实执行 CTest。发布任务依赖全部平台构建与这两项测试成功，不忽略失败、不静默跳过空测试列表。
- 本地构建默认仍开启测试；`BUILD_TESTING` 采用标准 CTest 选项。
- Windows 使用一个 MSBuild 项目配合 `/MP2`，避免项目并行乘以文件并行导致内存峰值过高。可通过 `DICENEXT_MSVC_COMPILE_PROCESSES` 调整，本地默认 0（不主动添加 /MP）。
- 日志将依赖准备、CMake 配置、源码编译、测试、打包分开展示。

## 本地验证与限制

- actionlint 检查发布工作流及展开后的缓存 action；YAML、Bash 语法和发布依赖关系检查通过。
- 缓存快照单元测试 3 项通过，覆盖冷缓存、新 ABI、时间戳变化和无关文件。
- 全新 Windows Release 目录编译通过；`BUILD_TESTING=OFF` 时没有测试目标，生成的 MSVC 项目确认启用 2 个编译进程。
- Windows Release 测试通过：470 个核心用例 / 2996 条断言、Lua 151 条断言；外部用户插件语料未配置时仍按原有逻辑跳过。
- 修正既有人格测试中写死的“基础”文案：改为比对当前翻译，并验证继承后不再使用私人格、全局设置未变；未改人格功能实现。
- 云端 Linux/macOS/ARM64、实际缓存保存恢复和加速幅度仍须推送后运行 CI 验证。本地检查不会删除云缓存、触发 Release 或自动部署。
