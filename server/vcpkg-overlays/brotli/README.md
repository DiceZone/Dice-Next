# Brotli 安全补丁

基于 `server/vcpkg.json` 固定的 vcpkg 基线
`44819aa2a6c10e56065e2b0330e7d6c89d1d2574` 中的 Brotli 1.2.0 端口，
保留原始源码校验和及安装补丁，仅回移以下上游安全修复：

- [google/brotli@0ed4268a4401abcb4472096340477cf4af7d1a2b](https://github.com/google/brotli/commit/0ed4268a4401abcb4472096340477cf4af7d1a2b)
- [Node.js 对 CVE-2026-111497 的修复说明](https://nodejs.org/en/blog/release/v26.12.1)

`server/vcpkg-configuration.json` 会让 manifest 构建优先使用此端口。
CI 缓存键包含配置与补丁内容；补丁变动后不能沿用未修复的二进制包。
经典模式的构建脚本与 Docker 构建也必须显式传入此 overlay。

移除此覆盖端口前，必须确认替代的 Brotli 版本包含同等修复，且通过
`BrotliSecurity` 测试。不能仅凭升级 Node 或修改版本声明视为修复后端依赖。
