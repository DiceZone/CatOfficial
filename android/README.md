# CatOfficial for Android

把现有的 C++ 核心跑在前台服务里，界面就是现在的 Web 管理面板（WebView 打开 `http://127.0.0.1:18082/`）。程序托管、网页终端、文件管理走同一套 POSIX 伪终端实现。

## 能力与限制

- **界面**：与桌面版同一套 WebUI。登录、机器人、连接、聊天、程序托管都在应用内操作。
- **程序托管**：子进程挂在前台服务上，锁屏后也会继续跑。启动命令由 `/system/bin/sh -c` 执行，工作目录在应用私有空间（`filesDir/data/instances/…`）。
  - 只能跑 **Android NDK 编出来的 ARM64 可执行文件**（bionic / `libc++`）、**完全静态链接**的 ARM64 程序，或 `sh` 脚本。
  - **不能**跑 Windows 的 `.exe`，也不能直接跑 GNU/Linux 发行版程序（它们依赖 `libstdc++.so.6` / `glibc`，Android 里没有）。官方 Dice!Next 安卓版是独立 APK（`libdice-next.so`），不能把 Linux 的 `dice-next` 丢进托管。
  - 从压缩包导入实例目录后，会自动给 ELF 和 `#!` 脚本加上执行权限（zip 常常丢掉 `+x`）。
  - Android 10 起不能从应用数据目录直接 `exec`；CatOfficial 会经系统 `linker64` 加载 PIE ELF。可执行文件仍须放在应用私有目录，不要从 Download/`/sdcard` 直接执行。
- **在线更新**：Android 上视为容器模式，只提示新版本，不在应用内替换 `.so`。请安装新的 APK。
- **系统托盘**：没有。用通知栏常驻。

最低系统 Android 8.0（API 26），目前只编 **arm64-v8a**。

## 编译

1. 安装 Android Studio（SDK 35、NDK 27、CMake、Ninja）、[vcpkg](https://github.com/microsoft/vcpkg)、Node.js。
2. 设置环境变量：
   - `ANDROID_HOME`（或 `ANDROID_SDK_ROOT`）
   - `ANDROID_NDK_HOME`（也可让脚本从 SDK 的 `ndk/` 里挑最新的）
   - `VCPKG_ROOT`
3. 复制 `android/local.properties.example` 为 `android/local.properties`，填本机 SDK 路径。
4. 在仓库根目录执行：

```powershell
.\scripts\build-android.ps1
```

脚本会：用 vcpkg 编 `arm64-android` 依赖 → 编出 `libcatofficial.so` → 拷到 `android/app/src/main/jniLibs/arm64-v8a/` → 编 WebUI 到 `android/app/src/main/assets/web/` → 下载 TLS 证书 → 打出 `release/CatOfficial-android.apk`。

GitHub 上对 `android` 分支（合并进 `main` 后也对 `main`）手动跑「Release CatOfficial」会多编一份 Android APK，随其它平台包一起挂到 Release。

也可在 `android/` 里执行 `gradlew.bat assembleRelease`（用 debug 密钥签名，可直接侧载）。

vcpkg 第一次编 Drogon 给 Android 会比较久。

## 使用程序托管

1. 打开应用，等面板加载即可进入（本机 WebView 不需要面板密码）。
2. 「程序托管」→「添加程序」。启动命令写成实例目录里的 NDK/静态 ARM64 二进制，例如 `./program` 或 `sh start.sh`。
3. 把该 ARM64 程序放进实例目录（面板「文件管理」或「导入压缩包」）。
4. 建议在系统设置里关掉对该应用的电池优化，否则厂商 ROM 可能在后台杀掉前台服务和托管进程。
