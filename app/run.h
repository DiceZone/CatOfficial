#pragma once
// 主程序入口。桌面版由 main() 调用；Android 由 JNI 在前台服务里调用。

namespace catofficial::app {

int run(int argc, char** argv);

}  // namespace catofficial::app
