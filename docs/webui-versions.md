# WebUI 版本

## 现状（2026-11-02 起）

- 原 V2 成为唯一的 WebUI，维护于 `Dice-Next-WebUI` 的 `main`。
- 旧 V1 存档在 `v1-webui` 分支，只读，不再移植新功能。
- 安装包只有一套前端：`web/dist/index.html` 与 `web/dist/assets/`。Vite 使用相对资源地址（`base: './'`），反向代理前缀可正常使用。

## 升级兼容

- `config/webui.json` 里旧的 `ui_version` 字段不再读取，也不会被删除，保留它不影响启动。
- `GET/PUT /api/system/webui-version` 已移除，界面设置页不再显示版本切换。
- 旧书签 `/v1/`、`/v2/`、`/v1/index.html`、`/v2/index.html` 由服务器 302 跳回根地址，使用相对 `Location` 保留反代前缀和查询参数（`server/src/service/webui_legacy.h`）。

## 构建与打包

在 `Dice-Next-WebUI` 执行 `npm ci && npm test && npm run quality && npm run build`，再将 `DICENEXT_WEB_ROOT` 指向该仓库运行对应系统的打包脚本。脚本只要求 `dist/index.html` 与 `dist/assets/` 存在。

Release 检出 WebUI `main`，测试并构建后上传 `web/dist`，五个平台打包复用同一份；`tools/test-webui-package.mjs` 对每个平台的安装包验证首页、资源、旧链接跳转与升级配置。`release-sources.json` 记录 WebUI 源码 SHA。

## 历史

2026-10 至 2026-11-01 曾并行维护 V1（`main`）与 V2（`webui-v2`），安装包同时附带两套界面并可在设置中切换。验收记录见 `docs/webui-v2-validation.md`。
