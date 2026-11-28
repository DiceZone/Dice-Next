# Dice!Next for Android

把现有的 C++ 核心跑在前台服务里，界面就是现在的 Web 管理面板（WebView 打开 `http://127.0.0.1:18088/`）。适配器、回复、牌堆、Mod 与桌面版同一套实现。

## 能力与限制

- **界面**：与桌面版同一套 WebUI。本机 WebView 从 loopback 进入时免面板密码。
- **网络**：面板只绑 `127.0.0.1`。QQ 官方机器人登录优先跳转到 QQ，二维码和手动填密钥仍是备选。
- **在线更新**：Android 上视为容器模式（`DICENEXT_CONTAINER=1`），只提示新版本，不在应用内替换 `.so`。请安装新的 APK。
- **系统托盘 / 开机自启注册表**：没有。用通知栏常驻；可选开机启动前台服务。
- **适配器**：OneBot / 官方机器人 / Discord / KOOK 走网络协议。OpenOPQ 若下载 Linux ARM64 二进制，需要能在应用私有目录执行。

最低系统 Android 8.0（API 26），目前只编 **arm64-v8a**。

## 编译

1. 安装 Android SDK 35、NDK 27、CMake、Ninja、[vcpkg](https://github.com/microsoft/vcpkg)、Node.js、JDK 17。
2. 同级目录准备 `onedice-cpp-lib`、`dicescript-c-lib`、`CatOfficial`、`Dice-Next-WebUI`（安卓面板改动在 WebUI 的 `android` 分支）。
3. 设置 `ANDROID_HOME`（或 `ANDROID_SDK_ROOT`）、`ANDROID_NDK_HOME`、`VCPKG_ROOT`、`JAVA_HOME`。
4. 在仓库根目录执行：

```powershell
.\scripts\build-android.ps1
```

Linux / macOS / CI：

```bash
./scripts/build-android.sh
```

产物：`release/DiceNext-android.apk`。

vcpkg 第一次编 Drogon、Lua、QuickJS 给 Android 会比较久。

## 使用

1. 安装 APK 后打开应用，等面板加载即可进入。
2. 建议在系统设置里关掉对该应用的电池优化，否则厂商 ROM 可能在后台杀掉前台服务和适配器连接。
