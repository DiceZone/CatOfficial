#!/usr/bin/env bash
# CatOfficial Linux / macOS 打包脚本（布局与 Dice!Next 相同）
#   用法：scripts/package-unix.sh <linux|macos> <amd64|arm64> <版本号> [编译目录=build] [前端目录=webui/dist]
#   产物：release/CatOfficial-<版本号>-<系统>-<架构>.tar.gz，解压出固定名称的 CatOfficial-beta 文件夹：
#     catofficial          前台管理程序：拉起主程序；主程序请求重启时安装已下载的更新再重新拉起
#     catofficial-core     主程序（依赖用 vcpkg 静态库链接进程序）
#     web/                 管理面板
#     update-mirrors.json  在线更新的加速镜像列表
set -euo pipefail

os="${1:?缺少系统（linux|macos）}"
arch="${2:?缺少架构（amd64|arm64）}"
version="${3:?缺少版本号}"
build_dir="${4:-build}"
web_dist="${5:-webui/dist}"

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
for binary in catofficial catofficial-core; do
  [ -x "$build_dir/$binary" ] || { echo "找不到 $build_dir/$binary，请先编译 Release" >&2; exit 1; }
done
[ -f "$web_dist/index.html" ] || { echo "找不到前端文件 $web_dist/index.html，请先在 webui 目录运行 npm run build" >&2; exit 1; }

name="CatOfficial-${version}-${os}-${arch}"
stage_root="$root/release/.stage"
stage="$stage_root/CatOfficial-beta"   # 内层文件夹固定，避免解压出一堆带版本号的文件夹
rm -rf "$stage_root"
mkdir -p "$stage"

install -m 0755 "$build_dir/catofficial" "$stage/catofficial"
install -m 0755 "$build_dir/catofficial-core" "$stage/catofficial-core"
strip "$stage/catofficial" "$stage/catofficial-core" 2>/dev/null || true
cp -R "$web_dist" "$stage/web"
cp resources/update-mirrors.json LICENSE README.md "$stage/"

cat > "$stage/使用说明.txt" <<EOF
CatOfficial ${version} — ${os} ${arch} 使用说明
=======================================

【运行】
1. 解压：tar -xzf ${name}.tar.gz && cd CatOfficial-beta
2. 启动：./catofficial
   首次启动会在本目录生成 config.json，控制台里会显示管理面板密码（也写在 config.json 的 webui.password 里），
   登录后请按提示设置你自己的密码。
3. 浏览器打开管理面板：http://localhost:18082
   面板默认只监听本机。需要从其他机器访问时，在面板「设置」里修改监听地址，并使用足够强的密码。

【说明】
  - catofficial 是前台管理程序，它拉起 catofficial-core 并在其请求重启时安装已下载的更新。
    用 systemd 或 launchd 守护时，ExecStart 请写 catofficial，不要写 catofficial-core。
  - 在线更新：面板「关于」里可以检查、下载并安装新版本，也可以设置自动检查与自动安装。容器内只提示新版本，请更新镜像。
  - 数据保存在本目录的 data 目录，配置在 config.json。手动升级时覆盖程序与 web 目录即可，这两项不会被覆盖。
  - 同一套数据只能运行一个进程；面板端口被占用时会自动顺延到下一个空闲端口并写回配置。

—— DiceZone
EOF

for required in catofficial catofficial-core web/index.html update-mirrors.json LICENSE; do
  [ -e "$stage/$required" ] || { echo "打包结果缺少 $required" >&2; exit 1; }
done

mkdir -p "$root/release"
archive="$root/release/$name.tar.gz"
rm -f "$archive"
tar -C "$stage_root" -czf "$archive" CatOfficial-beta
rm -rf "$stage_root"
echo "[OK] 打包完成：release/$name.tar.gz（$(du -h "$archive" | cut -f1)）"
