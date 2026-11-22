cmake_minimum_required(VERSION 3.21)
project(CatOfficial VERSION 0.1.0 LANGUAGES CXX)

# ─── Core library ────────────────────────────────────────────
# QQ 官方机器人协议层：会话、Gateway / Webhook、事件解析、富媒体上传、Bot。
# 全部是头文件；调用方需自行提供 Drogon、nlohmann_json 与 OpenSSL。
add_library(catofficial_core INTERFACE)
add_library(catofficial::core ALIAS catofficial_core)
target_include_directories(catofficial_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(catofficial_core INTERFACE cxx_std_20)

# ─── OneBot V11 中继程序 ─────────────────────────────────────
# 作为子项目被引用（例如 Dice!Next）时默认不编译程序，只提供上面的协议层。
option(CATOFFICIAL_BUILD_APP "Build the CatOfficial OneBot relay" ${PROJECT_IS_TOP_LEVEL})
option(CATOFFICIAL_BUILD_TESTS "Build CatOfficial unit tests" ${PROJECT_IS_TOP_LEVEL})

if(CATOFFICIAL_BUILD_APP OR CATOFFICIAL_BUILD_TESTS)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(MSVC)
        add_compile_options(/utf-8 /W3 /EHsc /bigobj)
        add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    find_package(Drogon CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    find_package(CURL CONFIG REQUIRED)
    find_package(ixwebsocket CONFIG REQUIRED)
    find_package(ZLIB REQUIRED)

    # 版本号：发布时 CI 传入 -DCATOFFICIAL_BUILD=N，显示为 0.1.0-beta.N；本地编译为 0，显示 0.1.0。
    set(CATOFFICIAL_BUILD 0 CACHE STRING "Release build number (0 for local builds)")
    if(CATOFFICIAL_BUILD GREATER 0)
        set(CATOFFICIAL_VERSION_STRING "${PROJECT_VERSION}-beta.${CATOFFICIAL_BUILD}")
    else()
        set(CATOFFICIAL_VERSION_STRING "${PROJECT_VERSION}")
    endif()
    configure_file(app/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/catofficial_version.h @ONLY)

    add_library(catofficial_app_deps INTERFACE)
    target_include_directories(catofficial_app_deps INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(catofficial_app_deps INTERFACE
        catofficial::core Drogon::Drogon nlohmann_json::nlohmann_json
        OpenSSL::SSL OpenSSL::Crypto unofficial::sqlite3::sqlite3 CURL::libcurl ixwebsocket::ixwebsocket ZLIB::ZLIB)

    # 在线更新（检查、下载、校验、解压）；POSIX 用自带的 tar.gz 读取器，Windows 用系统的 tar.exe。
    set(CATOFFICIAL_UPDATE_SOURCES app/update/update_service.cpp app/update/update_archive.cpp app/platform/system_info.cpp)
endif()

if(CATOFFICIAL_BUILD_APP)
    # 主程序（core）：Windows 发行包里放在 app\ 下，与全部 DLL 同目录。
    add_executable(catofficial-core app/main.cpp ${CATOFFICIAL_UPDATE_SOURCES})
    target_link_libraries(catofficial-core PRIVATE catofficial_app_deps)

    # 启动器 / 管理程序（与 Dice!Next 的 dice-next 相同）：处理待安装的更新再拉起 core。
    # Windows 上静态链接运行库（/MT）、只依赖系统 DLL，所以能在 app\ 的依赖加载之前运行；
    # POSIX 上是前台管理程序，core 以退出码 42 请求重启时由它安装更新并重新拉起。
    if(WIN32)
        add_executable(catofficial-launcher app/launcher/launcher_win.cpp)
        set_property(TARGET catofficial-launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_libraries(catofficial-launcher PRIVATE shell32 user32)
    else()
        add_executable(catofficial-launcher app/launcher/launcher_posix.cpp)
        target_link_libraries(catofficial-launcher PRIVATE nlohmann_json::nlohmann_json)
    endif()
    set_target_properties(catofficial-launcher PROPERTIES OUTPUT_NAME catofficial)
    target_include_directories(catofficial-launcher PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

    if(WIN32)
        # 程序图标（托盘也用它）与版本信息。
        enable_language(RC)
        target_sources(catofficial-core PRIVATE app/resources/app.rc app/resources/version.rc)
        target_compile_definitions(catofficial-core PRIVATE CATOFFICIAL_RC_CORE=1)
        target_sources(catofficial-launcher PRIVATE app/resources/app.rc app/resources/version.rc)
    endif()
endif()

if(CATOFFICIAL_BUILD_TESTS)
    enable_testing()
    add_executable(catofficial-tests tests/test_main.cpp ${CATOFFICIAL_UPDATE_SOURCES})
    target_link_libraries(catofficial-tests PRIVATE catofficial_app_deps)
    add_test(NAME catofficial-tests COMMAND catofficial-tests)
endif()
