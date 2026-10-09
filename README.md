<p align="center">
  <img src=".github/assets/readme-hero.svg" width="100%" alt="Dice!Next — 掷骰、人物卡与跑团日志" />
</p>

<p align="center">
  <a href="https://github.com/DiceZone/Dice-Next/releases"><img src="https://img.shields.io/github/v/release/DiceZone/Dice-Next?include_prereleases&amp;style=flat-square&amp;label=release&amp;color=6366f1" alt="最新发行版本" /></a>
  <a href="https://github.com/DiceZone/Dice-Next/actions/workflows/release.yml"><img src="https://github.com/DiceZone/Dice-Next/actions/workflows/release.yml/badge.svg" alt="Release 工作流状态" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0--or--later-64748b?style=flat-square" alt="AGPL-3.0-or-later" /></a>
</p>

<p align="center">
  <a href="https://github.com/DiceZone/Dice-Next/releases">下载</a> ·
  <a href="#快速开始">快速开始</a> ·
  <a href="https://docs.dice.zone/">文档</a> ·
  <a href="https://github.com/DiceZone/Dice-Next/issues">问题反馈</a>
</p>

Dice!Next 是面向 TRPG 跑团的自托管骰娘。连接你的机器人账号，在聊天中掷骰、管理人物卡、记录跑团；通过网页面板配置和维护。

当前处于公测（Beta）阶段，提供 Windows、Linux、macOS 发行包与 [Docker 部署方案](https://github.com/DiceZone/Dice-Next-Docker)。

## 快速开始

1. 从 [Releases](https://github.com/DiceZone/Dice-Next/releases) 的 **Assets** 下载对应系统与架构的安装包，不要选择 `Source code`。完整解压到固定目录，不要在压缩包或系统临时目录中运行。
2. 按下表启动。发行包已包含 WebUI，无需另行构建前端。

   | 系统 | 架构 / 包格式 | 启动方式 |
   | --- | --- | --- |
   | Windows | x64（amd64）/ ARM64 · `.zip` | 双击 `dice-next.exe` |
   | Linux | x64（amd64）/ ARM64 · `.tar.gz` | 在解压目录执行 `./start.sh` |
   | macOS | Apple Silicon（arm64）· `.tar.gz` | 在解压目录执行 `./start.sh` |

3. 打开 [http://localhost:18088](http://localhost:18088)，按引导设置管理密码。Windows 默认在系统托盘运行，也可右键托盘图标选择「打开网页面板」；端口被占用时会自动换用可用端口。
4. 在「适配器管理」添加连接，连接成功后发送指令：

   ```text
   .help
   .r 3d6+2
   .r 2#d100
   ```

还没接入聊天平台？可以先用面板里的「测试台」试指令。遇到启动或连接问题，查看[安装说明](https://docs.dice.zone/guide/install.html)和[故障排查](https://docs.dice.zone/guide/troubleshooting.html)。

## 能做什么

| 场景 | 功能 |
| --- | --- |
| 掷骰与检定 | 通用骰式、暗骰、连掷；COC / DND / BRP 检定、奖惩骰与房规 |
| 人物与团务 | 人物卡、NPC、先攻、团务管理与多群跑团 |
| 牌堆与回复 | 内置及自定义牌堆、关键词 / 正则回复；掷骰文本与人格可多条加权，支持嵌套 `{sample:甲\|乙}` |
| 跑团记录 | 群聊记录、日志站上传、TXT / Excel / HTML 导出；日志可独立于普通 `bot off` 控制 |
| 网页管理 | 适配器、群组、玩家、权限、插件与规则包；全局设置搜索、指令测试、活跃度与在线历史统计 |
| 插件与规则 | Lua 插件 / 模组、SealDice 风格 JS 插件、自定义规则包与人物卡模板 |
| 日常维护 | 备份恢复、定时任务与插件指令调用、通知、敏感词拦截；按需接入 BDC 云人物卡、云黑名单与心跳 |

界面和回复内置简体中文、繁体中文、英语与日语。完整指令、配置和插件说明见[文档站](https://docs.dice.zone/)。

## 接入与兼容

QQ 可通过 **OneBot v11**（正向 / 反向 WebSocket）、**Milky**（HTTP API + WebSocket 事件）或 **QQ 官方机器人**接入；也支持 Discord 与 KOOK，可以同时管理多个适配器。连接参数见[适配器配置](https://docs.dice.zone/config/adapter.html)。

Dice!Next 本身不是 QQ 协议端；使用 OneBot / Milky 时，需要另行运行对应协议服务。消息格式、群管理和文件发送等能力受平台权限与协议端支持范围限制。

旧版迁移与插件兼容的范围：

- 以旧版 Dice! 的指令习惯和 Lua 生态为兼容目标；**JS 插件对齐 SealDice，不是旧版 Dice! JS**。
- 支持导入旧版 Dice! 的部分人物卡、群设置、回复、牌堆、插件与日志，具体范围见[数据迁移](https://docs.dice.zone/guide/migration.html)。
- 不保证所有旧插件原样运行，也不照搬远程 Shell 等高风险能力。迁移后请保留旧目录，并在测试群核对结果。

## 更新与数据

配置和主要用户数据保存在程序目录的 `config/` 与 `data/`。面板提供手动 / 定时备份及恢复；升级前请先备份。

- **Windows 发行包**：从「关于项目」检查、下载、安装更新；可先下载，再按服务器时区定时安装，默认凌晨 04:00。
- **Linux / macOS**：含 POSIX 管理器的完整包通过 `start.sh` 或 `dice-next` 启动后，支持安装并重启与定时自动安装。旧包（含 beta.928）需先手动升级一次；直接运行 `dice-next-server` 仍仅支持检查和下载。
- **容器部署**：只检查版本，不在容器内下载或安装程序更新；通过拉取新镜像并重建容器升级。

具体操作见[安装与升级](https://docs.dice.zone/guide/install.html)。管理面板默认监听所有网卡；远程访问请使用防火墙限制来源，并通过 HTTPS 反向代理，不要裸露管理端口。

## 开发与贡献

后端使用 C++20，管理面板使用 React。构建与依赖说明见[从源码构建](https://docs.dice.zone/develop/build.html)，插件作者可从[插件开发](https://docs.dice.zone/develop/plugin-quickstart.html)开始。

<details>
<summary>相关仓库与源码构建布局</summary>

构建脚本按同级目录查找依赖与资源；使用发行包不需要克隆这些仓库。

| 仓库 | 用途 |
| --- | --- |
| [Dice-Next](https://github.com/DiceZone/Dice-Next) | 本仓库：后端、启动器与打包 |
| [Dice-Next-WebUI](https://github.com/DiceZone/Dice-Next-WebUI) | 管理面板 |
| [Dice-Next-Doc](https://github.com/DiceZone/Dice-Next-Doc) | 文档与指令数据 |
| [Dice-Next-Docker](https://github.com/DiceZone/Dice-Next-Docker) | 容器化部署（可选） |
| [onedice-cpp-lib](https://github.com/DiceZone/onedice-cpp-lib) | OneDice 表达式引擎（后端构建必需） |
| [dicescript-c-lib](https://github.com/DiceZone/dicescript-c-lib) | DiceScript 表达式引擎（后端构建必需） |

贡献约定见 [CONTRIBUTING.md](CONTRIBUTING.md)，发布构建说明见 [docs/ci-release-build.md](docs/ci-release-build.md)。

</details>

## 反馈

问题和建议请提交 [Issue](https://github.com/DiceZone/Dice-Next/issues)，附上版本、平台、复现步骤与日志；分享前请隐去密码、密钥和个人信息。交流 QQ 群：`933145116`。

## 致谢

本项目是对 [Dice!](https://github.com/Dice-Developer-Team/Dice) 的致敬重构，**并非原项目的官方后续版本**。感谢 w4123 溯洄、String.Empty（Shiki）及 Dice! 的开发者与贡献者。

同时感谢以下项目与社区：

- [OneDice](https://github.com/OlivOS-Team/lib-onedice) — 掷骰表达式规范。
- [SealDice](https://github.com/sealdice/sealdice-core) — JS 插件兼容与互操作的参考。
- [OlivaDice](https://github.com/OlivOS-Team/OlivaDiceCore) — OneDice 生态与骰娘指令习惯的参考。
- [OneBot v11](https://github.com/botuniverse/onebot-11) — 机器人平台适配协议。
- [NapCat](https://github.com/NapNeko/NapCatQQ) — QQ 协议端与 OneBot 生态支持。
- [LLOneBot](https://www.llonebot.com/) — QQ 协议端与 OneBot 生态支持。
- [SnowLuma](https://github.com/SnowLuma/SnowLuma) — QQ 协议端，以及本项目 README 排版的参考。

感谢所有参与开发、测试、反馈与文档维护的贡献者和用户。

## 许可证

本项目以 **AGPL-3.0-or-later** 发布，完整条款见 [LICENSE](LICENSE)。
