# 构建说明 — Dice!Next

> 仅支持 64 位 Windows 10/11 + MSVC 2022。主程序、前端和文档为三个并列仓库。

## 工具链

| 组件 | 位置（本机参考） | 说明 |
| --- | --- | --- |
| CMake | `C:\Program Files\CMake` | ≥ 3.20 |
| vcpkg | `D:\vcpkg` | drogon / sqlite_orm / spdlog / nlohmann-json / yaml-cpp / quickjs-ng 预装 |
| MSVC | 2022 BuildTools | C++20 |
| Node / npm | `C:\nvm4w` | 前端构建 |

## 后端

```sh
cmake -B server\build -S server ^
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build server\build --config Release -j
```

产物：`server\build\Release\dice-next-server.exe`。运行时工作目录需为 `server\`（以便找到 `config\`、`i18n\`）。HTTP 默认端口为 `18088`。

## 前端

```sh
cd ..\Dice-Next-WebUI
npm ci
npm run build   # → dist（由主程序打包脚本带入安装包）
```

## 打包发布

```sh
powershell -File package.ps1
```

输出 `release\DiceNext-beta-3.0.0(NNN)-YYYY-MM-DD-HHMMSS.zip`（exe + 运行库 DLL + i18n/decks/web/dist + 示例 JS 插件 + 使用说明）。打包前请将 `Dice-Next`、`Dice-Next-WebUI` 和 `Dice-Next-Doc` 放在同一目录，或设置 `DICENEXT_WEB_ROOT`、`DICENEXT_DOC_ROOT`。

---

## 版本图标：正式版 (stable) 与开发版 (dev)

项目有**两套 logo**，正式版与开发版编译时使用不同图标。三处图标——**网页 favicon / 侧栏 logo、程序 exe 图标、系统托盘图标**——都跟随版本切换。

| 版本 | logo | 说明 |
| --- | --- | --- |
| `stable`（默认） | 灰阶 d20 多面体 | 正式发布版（当前先行预览版即视为 stable） |
| `dev` | 深色底 + 绿色角标 | 开发版 |

源文件：`logo-stable.svg` / `logo-dev.svg`（项目根目录）。
图标资源（已随仓库保留，无需重新生成）：`server/src/resources/app-stable.ico`、`app-dev.ico`。

### 构建正式版（默认）

无需额外参数，按上文「后端」「前端」正常构建即可。`..\Dice-Next-WebUI\public\favicon.svg` 当前已是 stable 内容。

### 构建开发版

**1. exe + 托盘图标** —— configure 时传 `-DDICE_EDITION=dev`：

```sh
cmake -B server\build -S server -DDICE_EDITION=dev ^
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build server\build --config Release -j
```

CMake 会把 `app-dev.ico` 复制成 `app.ico`（由 `app.rc` 嵌入，托盘也读取同一图标）。configure 日志会打印 `Dice!Next edition: dev`。切回正式版重新 configure 一次即可（`-DDICE_EDITION=stable` 或删除该缓存项）。

**2. 网页 favicon / 侧栏 logo** —— 构建前先把 dev 版图标拷成当前 favicon：

```sh
copy /Y ..\Dice-Next-WebUI\public\favicon-dev.svg ..\Dice-Next-WebUI\public\favicon.svg
pushd ..\Dice-Next-WebUI && npm run build && popd
```

> 注意：`Dice-Next-WebUI/public/favicon.svg` 是「当前激活」的图标，`favicon-dev.svg` 是 dev 备用。切回正式版前记得复制回 stable 内容（即 `logo-stable.svg`）再重新构建。

### 后期 GitHub 分支约定

- **stable 分支**：默认即可，无需任何额外参数。
- **dev 分支**：CI 中加 `-DDICE_EDITION=dev`，并在前端构建前执行 `copy favicon-dev.svg → favicon.svg`。
