#pragma once
// 当前运行平台（给面板 API 用，前端据此显示 Android 上程序托管的说明）。

namespace catofficial::app {

inline const char* runtimeOs() {
#if defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

}  // namespace catofficial::app
