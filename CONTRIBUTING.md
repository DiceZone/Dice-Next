# 为 Dice!Next 做贡献

感谢关注！完整的贡献指南在文档站（`Dice-Next-Doc` 仓 `src/develop/contribution.md`，含项目约定与注意事项）。这里只放最快上手路径。

## 仓库布局

Dice!Next 拆分为多个仓库，构建脚本按**同级目录**互相寻找：

```
你的工作目录/
├── Dice-Next/          ← 本仓库：C++ 后端（server/ CMake 工程、package.ps1 打包）
├── Dice-Next-WebUI/    ← Web 管理面板（Vite + React + TS）
├── Dice-Next-Doc/      ← 文档站（VitePress）
├── Dice-Next-Docker/   ← 容器化部署（可选）
└── onedice-cpp-lib/    ← OneDice 表达式引擎（编译后端必需）
```

## 构建后端

依赖：VS2022 Build Tools（或对应平台工具链）、CMake ≥ 3.20、vcpkg（manifest 模式自动装依赖）。

```powershell
cmake -B server/build -S server -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release -j
# 运行（工作目录须为 server/，找 config/ 与 i18n/）
cd server && .\build\Release\dice-next-server.exe
```

详见 [BUILD.md](BUILD.md) 与文档站「从源码构建」（`Dice-Next-Doc` 仓 `src/develop/build.md`）；交叉编译见 [CROSS-COMPILE.md](CROSS-COMPILE.md)。

## 验证改动

不用连 QQ：启动后打开 `http://localhost:18088`，用侧边栏「测试台」直接发指令验证（后端接口为 `POST /api/test/message`）。

## 三条铁律

1. **先查原版再动手**：指令/规则/文案以原版 Dice! 行为为准（核心目标是老用户无痛迁移），不要自创语法。
2. **文案一律走 i18n**：不在代码里写死用户可见文本；新增键要同时补 zh-Hans / zh-Hant / en / ja 四语。
3. **改了行为就改文档**：对应更新 Dice-Next-Doc（用户/开发页）与 `docs/commands.json` 指令表。

想写插件而不是改内核？看文档站「插件开发快速上手」（`Dice-Next-Doc` 仓 `src/develop/plugin-quickstart.md`）。
