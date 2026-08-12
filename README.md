# Dice!Next

Dice!Next 是面向 TRPG 跑团与骰点场景的本地部署骰娘。它提供掷骰、人物卡、跑团日志、规则包与插件能力，并在本机托管管理后台；可同时接入 OneBot、QQ 官方机器人、Discord 与 KOOK。

> 项目目前处于公测（Beta）阶段。问题与功能建议请优先通过 [GitHub Issues](https://github.com/DiceZone/Dice-Next/issues) 提交；也欢迎加入 QQ 群 `933145116` 交流与反馈。

## 核心能力

- 多适配器会话管理：OneBot v11 正向 / 反向 WebSocket、QQ 官方机器人 2.0 Gateway WebSocket、Discord 与 KOOK。
- QQ 官方机器人支持 AppID + AppSecret 连接与扫码绑定；官方群聊获得的昵称会缓存并在同一官方机器人的私聊中复用，暂无昵称时显示为“用户”。
- COC、DND 等规则指令，人物卡、NPC、先攻、牌堆与自定义回复；支持 `.rd-5` 等默认骰修正、消息分段控制与统一时区。
- 群日志、团务、多群跑团与 TXT、Excel、网页等日志导出。
- WebUI 管理后台：群组、用户、插件、规则包、日志、适配器和系统设置；管理口令首次启动必须设置，支持登录限速与可选 `X-API-Key` 服务间鉴权。
- JavaScript / Lua 插件：JS 对齐海豹（SealDice）常用 API（含 `require`、WebSocket、`base64ToImage`、`.ts` 加载与模板名片），Lua 兼容 Dice! 生态；可选插件 RSA 签名校验、上传前权限声明与高危预检、行为审计日志。
- 心跳上报、云黑名单、通知窗口、定时任务按账号执行。
- 本地数据存储、手动/定时 ZIP 备份与网页恢复；默认管理后台地址为 `http://localhost:18088`。

## 项目组成

完整开发环境由以下同级项目组成：

| 项目 | 用途 |
| --- | --- |
| [Dice-Next](https://github.com/DiceZone/Dice-Next) | 本仓库：后端、消息处理、打包与 Release 工作流。 |
| [Dice-Next-WebUI](https://github.com/DiceZone/Dice-Next-WebUI) | React 管理后台前端。 |
| [Dice-Next-Doc](https://github.com/DiceZone/Dice-Next-Doc) | 文档站、路线图与内置指令数据。 |
| [Dice-Next-Docker](https://github.com/DiceZone/Dice-Next-Docker) | Docker 部署（含 NapCat 反向 WS 一键配置）。 |
| [onedice-cpp-lib](https://github.com/DiceZone/onedice-cpp-lib) | 掷骰表达式解析与计算引擎。 |

建议保持以下目录结构：

```text
workspace/
├─ Dice-Next/
├─ Dice-Next-WebUI/
├─ Dice-Next-Doc/
├─ Dice-Next-Docker/
└─ onedice-cpp-lib/
```

## 开发与构建

从源码构建、生成本地测试包和自动发布的说明均维护在文档站的[开发构建文档](https://github.com/DiceZone/Dice-Next-Doc/blob/main/src/develop/build.md)。Windows 本地打包也可直接运行仓库根目录的 `package.ps1`。

## 运行

启动程序后，默认管理后台地址为 [http://localhost:18088](http://localhost:18088)。**首次访问必须设置管理口令**（未设置口令时其它管理接口一律拒绝），之后使用该口令登录。

需要修改启动端口时，停止程序并编辑 `config/server.json` 中的 `port`，再重新启动——该文件在首次启动时自动生成，不随仓库分发。

Docker 部署（含 NapCat 模式）见 [Dice-Next-Docker](https://github.com/DiceZone/Dice-Next-Docker)。配置、部署、备份和升级的完整说明见 [Dice!Next 文档站](https://github.com/DiceZone/Dice-Next-Doc)。

## 安全

- 管理口令使用 PBKDF2-SHA256 加盐哈希存储，登录失败限速（5 次/分钟/ IP）。
- 默认监听 `0.0.0.0`：仅本机使用请忽略；如暴露到局域网/公网，务必设置强口令，并按需启用 Docker 最小权限加固或系统防火墙。
- 插件签名校验（PEM 公钥）、上传高危预检与行为审计日志均为可选/默认开启项，详见文档站。
- CI 内置 Gitleaks / Semgrep / Trivy 安全扫描。

## 致谢

Dice!Next 是对 [Dice!](https://github.com/Dice-Developer-Team/Dice) 的致敬重构项目。它以现代 C++、本地 WebUI 与可维护的模块化结构重新实现骰娘的核心使用体验；并非 Dice! 的官方后续版本，也不与原项目团队存在从属关系。

感谢 Dice! 的开发者与贡献者，尤其是 w4123 溯洄、String.Empty (Shiki)，对中文 TRPG 骰娘生态所作的长期贡献。重构过程中，Dice! 的公开行为、文档和开源实现为兼容性设计提供了重要参考。

同时感谢以下项目、协议与社区：

- [OneDice](https://github.com/OlivOS-Team/lib-onedice)：统一骰点表达式标准；本项目使用同级的 `onedice-cpp-lib` 实现。
- [SealDice](https://github.com/sealdice/sealdice-core)：骰娘生态的开放实践，以及日志与插件互操作的参考。
- [OlivaDice](https://github.com/OlivOS-Team/OlivaDiceCore)：开放的 TRPG 骰娘项目与 OneDice 生态实践。
- [OneBot v11](https://github.com/botuniverse/onebot-11)：机器人平台适配协议。
- [NapCat](https://github.com/NapNeko/NapCatQQ)：基于 NTQQ 的机器人协议端实现。
- [LLOneBot](https://www.llonebot.com/)：QQ 机器人框架与 OneBot 协议实现。
- [SnowLuma](https://snowluma.github.io/)：面向 NTQQ 与 OneBot 生态的远程协议框架。
- 所有为 TRPG 工具、规则包和模组做出贡献的开发者、骰主和测试用户。

## 开源许可证

Dice!Next 以 **GNU Affero General Public License v3.0 or later（AGPL-3.0-or-later）** 发布。你可以在遵守该许可证的前提下使用、复制、修改和再发布本项目。

如果你修改本项目并通过网络向他人提供服务，AGPL 要求向相应用户提供对应的完整源代码；分发修改版本时也必须保留许可证、版权与致谢信息。完整条款见 [LICENSE](LICENSE)。
 