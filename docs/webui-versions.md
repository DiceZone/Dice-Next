# WebUI 双版本维护

## 约定

- `Dice-Next-WebUI` 的 `main` 维护 **V1**；`webui-v2` 独立分支维护 **V2（预览版）**。
- V2 在独立分支升级技术栈并改版，不将预览版的渲染技术变更混入 V1。除初始已提交功能外，现有 MCP / OpenOPQ 前端也已同步到 V2 工作树；这些独立功能的提交状态仍由各自任务负责。
- **在用户明确确立 V2 为正式版并删除 V1 前，功能增减、行为和 API 变化必须同时适配两个 UI。** 翻译、帮助、全局搜索、测试、文档同样需要同步。仓库 `AGENTS.md` 已记录该强制规则。
- 不能用旧的“经典/新版内容宽度”冒充两个版本，也不能把同一份 V1 构建复制两次冒充 V2。

## 配置与访问

`config/webui.json` 的 `ui_version` 为 `v1`（默认）或 `v2`。密码、SSO 及其他字段原样保留。
`GET /api/system/webui-version` 返回已保存选择、实际可用版本、资源可用性；
`PUT` 请求体为 `{"version":"v2"}`，遵守统一登录和受限管理员权限保护，只允许修改此字段。
保存成功后仅刷新当前网页，不重启骰娘、不撤销会话、不强制刷新其他标签页。

根地址按服务器配置选择版本；`/v1/` 和 `/v2/` 可直接访问指定版本用于恢复。
两套前端共用服务器数据、认证 Cookie 和现有浏览器偏好，不复制数据库或账号配置。
资源缺失时回退到可用界面，保留原选择并在设置页提示；不允许新选择不存在的版本。
旧安装仅有 `web/dist/index.html` 时仍按 V1 提供服务。

## 构建与打包

先在两个分支分别 `npm ci && npm test && npm run build`。Vite 使用相对资源地址。
随后在主程序仓库执行（输出目录须不存在，不覆盖旧文件）：

```powershell
python .github/scripts/bundle-webui.py --v1 ../Dice-Next-WebUI/dist --v2 ../Dice-Next-WebUI-v2/dist --output ../webui-bundle/dist
$env:DICENEXT_WEB_ROOT = (Resolve-Path ../webui-bundle).Path
```

再运行对应系统的打包脚本。包内结构：

```text
web/dist/
  index.html                 # 兼容入口；新版后端在根地址按配置选择
  webui-versions.json         # 两套 UI 的分支、源码 SHA 和预览标记
  v1/index.html + assets/
  v2/index.html + assets/
```

Release 分别检出 WebUI `main` / `webui-v2`，测试、构建后合成同一个资源附件，供五个平台打包复用；缺任一版本直接失败，不发布残缺包。
`release-sources.json` 同时记录 V1 和 V2 源码 SHA。首次启用此流程前需将两套分支的实现推送到 GitHub；本地未提交工作不会自动进入 CI。

## 并行开发的边界

MCP / OpenOPQ 已同步两套前端，2026-10-28 用户授权与双版本 UI 一并提交和发布。
后续这些功能继续变化时，仍须核对两边实现；本地验证与实际 Release 成功分开记录。

## V2 首版技术栈与验证

V2 使用 Tailwind 4 + Vite 插件，保留本地字体、四种语言、服务器时区和窄屏交互；V1 保留原技术栈。
TanStack Router 接管 hash 路由与按页加载，旧链接及搜索定位继续可用；站内链接统一经过离页确认。
TanStack Query 为核心读取、分作用域回复与服务器分页提供内存缓存和去重，写操作不自动重试；切换认证密钥或登出清理缓存。
表单使用 React Hook Form / Zod，日历由 shadcn CLI 生成、采用 DayPicker / date-fns；提示统一到 Sonner，动效由 Motion 执行并遵守减少动态效果设置。

现有 Zustand 接口暂作为兼容视图，核心读取已通过 Query；编辑草稿仍由本地状态管理。并非把所有历史表单与状态代码重写一遍，避免在技术迁移中无谓改变业务行为。TanStack Table 继续用于回复列表。
统计接入固定提交的 Tremor Raw 源码及 Apache-2.0 许可证，保留全部日期标签、骰点分布、在线历史与各类筛选。

新增 `npm run test:unit`（Vitest / Testing Library）、`npm run test:v2-browser`（Playwright 页面、暗色和窄屏回归）和 `npm run quality`（Biome）。原业务回归仍在 `npm run test:legacy`，`npm test` 同时运行两类测试。
初次使用新 CLI 组件时保留了自定义弹窗的固定编辑尺寸、内部滚动、问号焦点和响应式选项卡修复，没有直接覆盖为通用组件。

React Bits / Skiper 为可选素材，不为凑齐选型引入未使用依赖或许可不明确的源码。已复制的 Tremor、shadcn 源码许可证随 V2 构建保留；详见 V2 的 `THIRD_PARTY/README.md`。
