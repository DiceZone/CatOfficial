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

    add_library(catofficial_app_deps INTERFACE)
    target_link_libraries(catofficial_app_deps INTERFACE
        catofficial::core Drogon::Drogon nlohmann_json::nlohmann_json
        OpenSSL::SSL OpenSSL::Crypto unofficial::sqlite3::sqlite3 CURL::libcurl ixwebsocket::ixwebsocket)
endif()

if(CATOFFICIAL_BUILD_APP)
    add_executable(catofficial app/main.cpp)
    target_link_libraries(catofficial PRIVATE catofficial_app_deps)
endif()

if(CATOFFICIAL_BUILD_TESTS)
    enable_testing()
    add_executable(catofficial-tests tests/test_main.cpp)
    target_link_libraries(catofficial-tests PRIVATE catofficial_app_deps)
    add_test(NAME catofficial-tests COMMAND catofficial-tests)
endif()
