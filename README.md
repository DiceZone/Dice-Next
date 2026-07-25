# Dice!Next

Dice!Next 是面向 QQ 跑团与骰点场景的本地部署骰娘。它通过 OneBot v11 接入机器人平台，提供掷骰、人物卡、跑团日志、规则包与插件能力，并在本机托管管理后台。

> 项目仍在开发与内部测试阶段。问题、建议和测试反馈请通过 QQ 群 `933145116` 提交。

## 核心能力

- OneBot v11 正向 / 反向 WebSocket 适配与多平台会话管理。
- COC、DND 等规则指令，人物卡、NPC、先攻、牌堆与自定义回复。
- 群日志、团务、多群跑团与 TXT、Excel、网页等日志导出。
- WebUI 管理后台：群组、用户、插件、规则包、日志、适配器和系统设置。
- JavaScript / Lua 插件与可扩展规则包。
- 本地数据存储；默认管理后台地址为 `http://localhost:18088`。

## 项目组成

完整开发环境由四个同级项目组成：

| 项目 | 用途 |
| --- | --- |
| `Dice-Next` | 本仓库：后端、消息处理、打包与 Release 工作流。 |
| `Dice-Next-WebUI` | React 管理后台前端。 |
| `Dice-Next-Doc` | 文档站、路线图与内置指令数据。 |
| `onedice-cpp-lib` | 掷骰表达式解析与计算引擎。 |

建议保持以下目录结构：

```text
workspace/
├─ Dice-Next/
├─ Dice-Next-WebUI/
├─ Dice-Next-Doc/
└─ onedice-cpp-lib/
```

## 本地构建

### 环境要求

- Windows 10/11 64 位
- Visual Studio 2022（C++ 桌面开发组件）与 CMake 3.20+
- vcpkg
- Node.js 20+

### 构建前端

```powershell
cd ..\Dice-Next-WebUI
npm ci
npm run build
```

### 构建后端

在本仓库根目录执行，替换为本机 vcpkg 路径：

```powershell
cmake -S server -B server\build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build server\build --config Release -j 6
```

### 生成测试包

```powershell
$env:DICENEXT_WEB_ROOT = "..\Dice-Next-WebUI"
$env:DICENEXT_DOC_ROOT = "..\Dice-Next-Doc"
powershell -File package.ps1
```

默认输出到 `release/`。可用 `DICENEXT_RELEASE_ROOT` 指定其他输出目录。

## 自动发布

推送 `v*` 格式的 Git 标签会触发 Release 工作流，自动构建并发布：

- Linux：amd64、arm64
- Windows：amd64、arm64
- macOS：arm64

工作流会检出同级的前端、文档与 OneDice 项目并组装完整发行包。三个依赖仓库在私有阶段需要配置一个可读取它们的 CI token secret。

## 致谢

Dice!Next 是对 [Dice!](https://github.com/Dice-Developer-Team/Dice) 的致敬重构项目。它以现代 C++、本地 WebUI 与可维护的模块化结构重新实现骰娘的核心使用体验；并非 Dice! 的官方后续版本，也不与原项目团队存在从属关系。

感谢 Dice! 的开发者与贡献者，尤其是 w4123 溯洄、String.Empty 和 Shiki，对中文 TRPG 骰娘生态所作的长期贡献。重构过程中，Dice! 的公开行为、文档和开源实现为兼容性设计提供了重要参考。

同时感谢以下项目、协议与社区：

- [OneDice](https://github.com/OlivOS-Team/lib-onedice)：统一骰点表达式标准；本项目使用同级的 `onedice-cpp-lib` 实现。
- [SealDice](https://github.com/sealdice/sealdice-core)：骰娘生态的开放实践，以及日志与插件互操作的参考。
- [OneBot v11](https://github.com/botuniverse/onebot-11)：机器人平台适配协议。
- [NapCat](https://github.com/NapNeko/NapCatQQ)：基于 NTQQ 的机器人协议端实现。
- [LLOneBot](https://www.llonebot.com/)：QQ 机器人框架与 OneBot 协议实现。
- [SnowLuma](https://snowluma.github.io/)：面向 NTQQ 与 OneBot 生态的远程协议框架。
- 所有为 TRPG 工具、规则包和模组做出贡献的开发者、骰主和测试用户。

## 运行与数据

首次启动会自动生成配置。生产环境请保留安装目录外的配置和数据库；升级时替换程序、资源与 WebUI 文件即可。更多配置、指令与更新记录请查看 `Dice-Next-Doc`。

## 开源许可证

Dice!Next 以 **GNU Affero General Public License v3.0 or later（AGPL-3.0-or-later）** 发布。你可以在遵守该许可证的前提下使用、复制、修改和再发布本项目。

如果你修改本项目并通过网络向他人提供服务，AGPL 要求向相应用户提供对应的完整源代码；分发修改版本时也必须保留许可证、版权与致谢信息。完整条款见 [LICENSE](LICENSE)。
