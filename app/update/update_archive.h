#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace catofficial::app::update {
// Read gzip/tar directly: shell tools' line-oriented listings cannot safely
// validate PAX paths, symlinks, duplicate files or special-device entries.
/// 解 tar / tar.gz（程序托管导入压缩包用）：Windows 用系统的 tar.exe（先列一遍条目检查路径），POSIX 用上面的读取器（只认 tar.gz）。
bool extractTarArchive(const std::filesystem::path& archive, const std::filesystem::path& output, std::string& error);

bool extractPosixUpdate(const std::filesystem::path& archive, const std::filesystem::path& output,
                        std::string& error, const std::function<bool()>& cancelled = {});
}
