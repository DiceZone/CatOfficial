#!/usr/bin/env bash
# CatOfficial Linux / macOS 打包脚本
#   用法：scripts/package-unix.sh <linux|macos> <amd64|arm64> <版本号> [编译目录=build] [前端目录=webui/dist]
#   产物：release/CatOfficial-<版本号>-<系统>-<架构>.tar.gz
#   依赖用 vcpkg 静态库链接进程序，包里只有一个可执行文件、web/（管理面板）与说明文件。
set -euo pipefail

os="${1:?缺少系统（linux|macos）}"
arch="${2:?缺少架构（amd64|arm64）}"
version="${3:?缺少版本号}"
build_dir="${4:-build}"
web_dist="${5:-webui/dist}"

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
binary="$build_dir/catofficial"
[ -x "$binary" ] || { echo "找不到 $binary，请先编译 Release" >&2; exit 1; }
[ -f "$web_dist/index.html" ] || { echo "找不到前端文件 $web_dist/index.html，请先在 webui 目录运行 npm run build" >&2; exit 1; }

name="CatOfficial-${version}-${os}-${arch}"
stage_root="$root/release/.stage"
stage="$stage_root/$name"
rm -rf "$stage_root"
mkdir -p "$stage"

cp "$binary" "$stage/"
strip "$stage/catofficial" 2>/dev/null || true
cp -R "$web_dist" "$stage/web"
cp LICENSE README.md "$stage/"

cat > "$stage/使用说明.txt" <<EOF
CatOfficial ${version} — ${os} ${arch} 使用说明
=======================================

【运行】
1. 解压：tar -xzf ${name}.tar.gz && cd ${name}
2. 启动：./catofficial
   首次启动会在程序旁边生成 config.json，控制台里会显示管理面板密码（也写在 config.json 的 webui.password 里）。
3. 浏览器打开管理面板：http://localhost:18082
   面板默认只监听本机。需要从其他机器访问时，在面板「设置」里修改监听地址，并使用足够强的密码。

【说明】
  - 数据保存在当前目录的 data 目录，配置在 config.json。也可以用 ./catofficial /path/to/config.json 指定配置文件，数据目录随之放在它旁边。
  - 升级时解压覆盖程序与 web 目录即可，config.json 与 data 不会被覆盖。
  - 长期运行建议交给 systemd（Linux）或 launchd（macOS）守护。

—— DiceZone
EOF

for required in catofficial web/index.html LICENSE; do
  [ -e "$stage/$required" ] || { echo "打包结果缺少 $required" >&2; exit 1; }
done

mkdir -p "$root/release"
archive="$root/release/$name.tar.gz"
rm -f "$archive"
tar -C "$stage_root" -czf "$archive" "$name"
rm -rf "$stage_root"
echo "[OK] 打包完成：release/$name.tar.gz（$(du -h "$archive" | cut -f1)）"
