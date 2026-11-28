# Dice!Next for Android

把现有的 C++ 核心跑在前台服务里，界面就是现在的 Web 管理面板（WebView 打开 `http://127.0.0.1:18088/`）。适配器、回复、牌堆、Mod 与桌面版同一套实现。

## 能力与限制

- **界面**：与桌面版同一套 WebUI。回环连接具有面板管理员权限。同一设备上能连接该端口的其他应用也能管理骰娘。
- **网络**：面板只绑 `127.0.0.1`。QQ 官方机器人登录优先跳转到 QQ，二维码和手动填密钥仍是备选。
- **在线更新**：下载并校验与设备匹配的 APK 后，系统安装器会请求确认。覆盖安装需要相同的应用签名。
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

## GitHub APK 签名

直接分发的 APK 使用固定签名，以便用户保留应用数据覆盖升级。在主程序仓库的 Actions secrets 配置：

- `ANDROID_KEYSTORE_BASE64`：正式发布密钥库的 Base64 内容。
- `ANDROID_KEYSTORE_PASSWORD`、`ANDROID_KEY_ALIAS`、`ANDROID_KEY_PASSWORD`：密钥库密码、密钥别名和密钥密码。

配置齐全时，Release 同时构建并签名 Android arm64 安装包。缺少配置时仅发布桌面包。请备份签名密钥库，后续版本须沿用同一把密钥。本地发布构建使用 `ANDROID_KEYSTORE_PATH` 指向密钥库文件，并设置上述密码和别名。本地测试可用 Gradle 的 `assembleDebug`。
