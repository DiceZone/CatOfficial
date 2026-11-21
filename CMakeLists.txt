cmake_minimum_required(VERSION 3.20)
project(CatOfficial VERSION 0.1.0 LANGUAGES CXX)

# ─── Core library ────────────────────────────────────────────
# QQ 官方机器人协议层：Gateway / Webhook / 协议字段解析 / 消息格式。
# 目前全部是头文件；调用方需自行提供 Drogon、nlohmann_json 与 OpenSSL。
add_library(catofficial_core INTERFACE)
add_library(catofficial::core ALIAS catofficial_core)
target_include_directories(catofficial_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(catofficial_core INTERFACE cxx_std_20)
