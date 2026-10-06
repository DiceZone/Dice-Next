# Release 构建提速与测试门禁

## 自动发布说明

Release 工作流使用 `.github/scripts/release-notes.py` 生成中文说明，通过
`body_path: release-notes.md` 写入 Release，不再只依赖 GitHub 的 PR 摘要。

- 比较起点是比本次 tag 更早、源码属于本次构建历史且五个平台安装包及更新清单完整的已发布版本，包含 Beta；草稿、当前 tag、未来版本及未上传完整的发布不作基准。
- 主程序范围截止于本次 `github.sha`，WebUI 范围截止于实际构建任务检出的 SHA，不采纳构建期间后来进入 main 的提交。生成器同样读取本次构建 SHA 中已测试的脚本，说明在构建号提交 / rebase 之前生成。
- 每次发布额外附带 `release-sources.json`，记录主程序和 WebUI 的仓库、完整 SHA 与比较基准；不改变更新清单 schema、安装包或客户端更新协议。
- 首次接入旧 Release 时，尝试从同一主程序 SHA 的成功 Release 运行中恢复 WebUI 检出 SHA；晚于该 Release 发布的重跑不作依据。日志过期或无读取权限时明确提示无法比较前端，不按提交日期猜测，也不把整个前端历史当作本次新增。
- `feat` / `fix` / `perf` 自动分类为新增功能、问题修复、性能优化，破坏性变更单列；重复摘要合并并保留对应提交链接。构建号、普通 CI / 文档等提交保留在折叠记录中。摘要和折叠记录有长度限制，超出部分明确指向完整 Git 比较链接。
- 元数据读取失败不会悄悄选一个更旧的比较起点；确认不了主程序发布范围，或已发布源码记录的格式 / 仓库 / SHA 校验失败时，在发布前报错。已有有效源码记录时无需依赖旧日志。脚本只进行 GitHub GET 和本地 Git 读取，不创建 tag、提交、发布或执行 commit 文本。

大提交可以在正文中附带可选发布摘要；未填写时使用标题，不自动抄入测试过程或其他正文：

```text
feat(core): 支持定时安装更新

Release-Notes:
- feat: 支持先下载更新包，再按服务器时区定时安装，默认凌晨 04:00。
- fix: 开启自动安装后，手动重启也会安装已下载更新。
End-Release-Notes

验证：这里记录测试范围，不会进入上述摘要。
```

本地验证可运行 `python -m unittest discover -s .github/scripts -p 'test_*.py' -v`。
使用脚本 CLI 做只读预览时，传入两个仓库的完整构建 SHA、目标 tag，以及
独立的 `--output` / `--sources-output` 路径；该操作仅生成文件，不触发 Release。

## 独立前端检查（本轮本地新增）

WebUI 仓的 `.github/workflows/checks.yml` 在 push / pull_request / 手动运行时执行 `npm ci`、`npm test`、类型检查、生产构建和 PWA 产物检查。固定 Ubuntu 24.04 / Node 24，缓存 npm 下载，旧任务可被新提交取消。只读权限、不保留检出凭据、不使用发布密钥、不部署，不修改主仓后端发布工作流或 vcpkg 缓存。首次 GH 执行须等提交后验证。

## 2026-09-28–30 修复

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
- 上述为本地验证范围；后续云端结果见下方，缓存加速幅度仍随 runner 环境和命中率变化，不能保证固定耗时。

## 2026-09-30：修正 vcpkg 历史版本检出

- 首次云端验证发现共用 action 默认浅克隆 vcpkg；安装清单锁定的 Lua 5.4.8 需要历史端口树，导致五个平台构建与两个后端测试任务均在依赖安装阶段失败。WebUI 构建及产物上传正常。
- 为 vcpkg 检出显式设置 `fetch-depth: 0`，保留固定 baseline 与 Lua 版本，不改变运行时代码或放宽版本约束。缓存提速与测试门禁仍保留。
- 新增检出配置回归测试：旧配置下失败，修复后通过；本地完整 vcpkg 仓库确认包含日志中缺失的 Lua 5.4.8 端口树。

## 2026-10-03：已发布结果与验证边界

- 上述缓存与检出修复已合入 main，最近 [Release 工作流](https://github.com/DiceZone/Dice-Next/actions/runs/37108609089) 成功，beta.925 已发布；不再标记为“本地未发布”。
- `2c704f7` 升级安全扫描环境与检出动作；`22904bb` 的更新模块专项测试 22 项 / 169 条断言通过。后续 `75ec20d` 为帮助文案更新，不能把其安全扫描成功当成又跑过一次完整发布构建。
- 缓存检查现有 4 项回归用例。正式版可配置 `-DDICENEXT_PRERELEASE=OFF`；当前 Release 工作流仍按 Beta 发布。
- 这些结果不代替平台真实消息发送、Windows 升级取消 / 安装链路或缓存长期命中率的验收。
