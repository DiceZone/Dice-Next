<p align="center">
  <img src=".github/assets/readme-hero.zh-Hant.svg" width="100%" alt="Dice!Next — 擲骰、人物卡與跑團日誌" />
</p>

<p align="center">
  <a href="README.md">English</a> · <a href="README.zh-Hans.md">简体中文</a> · <b>繁體中文</b> · <a href="README.ja.md">日本語</a>
</p>

<p align="center">
  <a href="https://github.com/DiceZone/Dice-Next/releases"><img src="https://img.shields.io/github/v/release/DiceZone/Dice-Next?include_prereleases&amp;style=flat-square&amp;label=release&amp;color=6366f1" alt="最新發行版本" /></a>
  <a href="https://github.com/DiceZone/Dice-Next/actions/workflows/release.yml"><img src="https://github.com/DiceZone/Dice-Next/actions/workflows/release.yml/badge.svg" alt="Release 工作流狀態" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0--or--later-64748b?style=flat-square" alt="AGPL-3.0-or-later" /></a>
</p>

<p align="center">
  <a href="https://github.com/DiceZone/Dice-Next/releases">下載</a> ·
  <a href="#快速開始">快速開始</a> ·
  <a href="https://docs.dice.zone/zh-hant/">文件</a> ·
  <a href="https://github.com/DiceZone/Dice-Next/issues">問題反饋</a>
</p>

Dice!Next 是面向 TRPG 跑團的自託管骰娘。連線你的機器人賬號，在聊天中擲骰、管理人物卡、記錄跑團；通過網頁面板配置和維護。

當前處於公測（Beta）階段，提供 Windows、Linux、macOS 發行包與 [Docker 部署方案](https://github.com/DiceZone/Dice-Next-Docker)。

## 快速開始

1. 從 [Releases](https://github.com/DiceZone/Dice-Next/releases) 的 **Assets** 下載對應系統與架構的安裝包，不要選擇 `Source code`。完整解壓到固定目錄，不要在壓縮包或系統臨時目錄中執行。
2. 按下表啟動。發行包已包含 WebUI，無需另行構建前端。

   | 系統 | 架構 / 包格式 | 啟動方式 |
   | --- | --- | --- |
   | Windows | x64（amd64）/ ARM64 · `.zip` | 雙擊 `dice-next.exe` |
   | Linux | x64（amd64）/ ARM64 · `.tar.gz` | 在解壓目錄執行 `./start.sh` |
   | macOS | Apple Silicon（arm64）· `.tar.gz` | 在解壓目錄執行 `./start.sh` |

3. 開啟 [http://localhost:18088](http://localhost:18088)，按引導設定管理密碼。Windows 預設在系統托盤執行，也可右鍵托盤圖示選擇「開啟網頁面板」；埠被佔用時會自動換用可用埠。
4. 在「適配器管理」新增連線，連線成功後傳送指令：

   ```text
   .help
   .r 3d6+2
   .r 2#d100
   ```

還沒接入聊天平臺？可以先用面板裡的「測試臺」試指令。遇到啟動或連線問題，檢視[安裝說明](https://docs.dice.zone/zh-hant/guide/install.html)和[故障排查](https://docs.dice.zone/zh-hant/guide/troubleshooting.html)。

## 能做什麼

| 場景 | 功能 |
| --- | --- |
| 擲骰與檢定 | 通用骰式、暗骰、連擲；COC / DND / BRP 檢定、獎懲骰與房規 |
| 人物與團務 | 人物卡、NPC、先攻、團務管理與多群跑團 |
| 牌堆與回覆 | 內建及自訂牌堆、關鍵詞 / 正則回覆；擲骰文本與人格可多條加權，支援巢狀 `{sample:甲\|乙}` |
| 跑團記錄 | 群聊記錄、日誌站上傳、TXT / Excel / HTML 匯出；日誌可獨立於普通 `bot off` 控制 |
| 網頁管理 | 適配器、群組、玩家、許可權、插件與規則包；全域性設定搜尋、指令測試、活躍度與線上歷史統計 |
| 插件與規則 | Lua 插件 / 模組、SealDice 風格 JS 插件、自訂規則包與人物卡模板 |
| 日常維護 | 備份恢復、定時任務與插件指令呼叫、通知、敏感詞攔截；按需接入 BDC 雲人物卡、雲黑名單與心跳 |

介面和回覆內建簡體中文、繁體中文、英語與日語。完整指令、配置和插件說明見[文件站](https://docs.dice.zone/zh-hant/)。

## 接入與相容

QQ 可通過 **OneBot v11**（正向 / 反向 WebSocket）、**Milky**（HTTP API + WebSocket 事件）或 **QQ 官方機器人**接入；也支援 Discord 與 KOOK，可以同時管理多個適配器。連線引數見[適配器配置](https://docs.dice.zone/zh-hant/config/adapter.html)。

Dice!Next 本身不是 QQ 協議端；使用 OneBot / Milky 時，需要另行執行對應協議服務。訊息格式、群管理和檔案傳送等能力受平臺許可權與協議端支援範圍限制。

舊版遷移與插件相容的範圍：

- 以舊版 Dice! 的指令習慣和 Lua 生態為相容目標；**JS 插件對齊 SealDice，不是舊版 Dice! JS**。
- 支援匯入舊版 Dice! 的部分人物卡、群設定、回覆、牌堆、插件與日誌，具體範圍見[資料遷移](https://docs.dice.zone/zh-hant/guide/migration.html)。
- 不保證所有舊插件原樣執行，也不照搬遠端 Shell 等高風險能力。遷移後請保留舊目錄，並在測試群核對結果。

## 更新與資料

配置和主要使用者資料儲存在程式目錄的 `config/` 與 `data/`。面板提供手動 / 定時備份及恢復；升級前請先備份。

- **Windows 發行包**：從「關於專案」檢查、下載、安裝更新；可先下載，再按伺服器時區定時安裝，預設凌晨 04:00。
- **Linux / macOS**：含 POSIX 管理器的完整包通過 `start.sh` 或 `dice-next` 啟動後，支援安裝並重啟與定時自動安裝。舊包（含 beta.928）需先手動升級一次；直接執行 `dice-next-server` 仍僅支援檢查和下載。
- **容器部署**：只檢查版本，不在容器內下載或安裝程式更新；通過拉取新映像並重建容器升級。

具體操作見[安裝與升級](https://docs.dice.zone/zh-hant/guide/install.html)。管理面板預設監聽所有網絡卡；遠端訪問請使用防火牆限制來源，並通過 HTTPS 反向代理，不要裸露管理埠。

## 開發與貢獻

後端使用 C++20，管理面板使用 React。構建與依賴說明見[從原始碼構建](https://docs.dice.zone/zh-hant/develop/build.html)，插件作者可從[插件開發](https://docs.dice.zone/zh-hant/develop/plugin-quickstart.html)開始。

<details>
<summary>相關倉庫與原始碼構建佈局</summary>

構建指令碼按同級目錄查詢依賴與資源；使用發行包不需要克隆這些倉庫。

| 倉庫 | 用途 |
| --- | --- |
| [Dice-Next](https://github.com/DiceZone/Dice-Next) | 本倉庫：後端、啟動器與打包 |
| [Dice-Next-WebUI](https://github.com/DiceZone/Dice-Next-WebUI) | 管理面板 |
| [Dice-Next-Doc](https://github.com/DiceZone/Dice-Next-Doc) | 文件與指令資料 |
| [Dice-Next-Docker](https://github.com/DiceZone/Dice-Next-Docker) | 容器化部署（可選） |
| [onedice-cpp-lib](https://github.com/DiceZone/onedice-cpp-lib) | OneDice 表示式引擎（後端構建必需） |
| [dicescript-c-lib](https://github.com/DiceZone/dicescript-c-lib) | DiceScript 表示式引擎（後端構建必需） |

貢獻約定見 [CONTRIBUTING.md](CONTRIBUTING.md)，釋出構建說明見 [docs/ci-release-build.md](docs/ci-release-build.md)。

</details>

## 反饋

問題和建議請提交 [Issue](https://github.com/DiceZone/Dice-Next/issues)，附上版本、平臺、復現步驟與日誌；分享前請隱去密碼、金鑰和個人資訊。交流 QQ 群：`933145116`。

## 致謝

本專案是對 [Dice!](https://github.com/Dice-Developer-Team/Dice) 的致敬重構，**並非原專案的官方後續版本**。感謝 w4123 溯洄、String.Empty（Shiki）及 Dice! 的開發者與貢獻者。

同時感謝以下專案與社群：

- [OneDice](https://github.com/OlivOS-Team/lib-onedice) — 擲骰表示式規範。
- [SealDice](https://github.com/sealdice/sealdice-core) — JS 插件相容與互操作的參考。
- [OlivaDice](https://github.com/OlivOS-Team/OlivaDiceCore) — OneDice 生態與骰娘指令習慣的參考。
- [OneBot v11](https://github.com/botuniverse/onebot-11) — 機器人平臺適配協議。
- [NapCat](https://github.com/NapNeko/NapCatQQ) — QQ 協議端與 OneBot 生態支援。
- [LLOneBot](https://www.llonebot.com/) — QQ 協議端與 OneBot 生態支援。
- [SnowLuma](https://github.com/SnowLuma/SnowLuma) — QQ 協議端，以及本專案 README 排版的參考。

感謝所有參與開發、測試、反饋與文件維護的貢獻者和使用者。

## 許可證

本專案以 **AGPL-3.0-or-later** 釋出，完整條款見 [LICENSE](LICENSE)。
