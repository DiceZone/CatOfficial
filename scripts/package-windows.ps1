# CatOfficial Windows 打包脚本（布局与 Dice!Next 相同）
#   用法：scripts\package-windows.ps1 -Version 0.1.0-beta.1 [-Architecture amd64|arm64] [-BuildDir build\msvc] [-WebDist webui\dist]
#   产物：release\CatOfficial-<Version>-windows-<Architecture>.zip，解压出固定名称的 CatOfficial-beta 文件夹：
#     catofficial.exe          启动器：安装待装的更新后拉起 app\ 里的主程序，随即退出
#     app\catofficial-core.exe 主程序，与全部依赖 DLL、MSVC 运行库同目录
#     web\                     管理面板
#     update-mirrors.json      在线更新的加速镜像列表
#   不打包配置文件与 data\：首次运行会在安装目录生成，升级覆盖解压或在线更新都不会碰它们。
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [ValidateSet('amd64', 'arm64')][string]$Architecture = 'amd64',
    [string]$BuildDir = 'build\msvc',
    [string]$WebDist = 'webui\dist'
)

$ErrorActionPreference = 'Stop'
function Fail($message) { Write-Host "[FAIL] $message" -ForegroundColor Red; exit 1 }

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
$relDir = Join-Path $BuildDir 'Release'
$core = Join-Path $relDir 'catofficial-core.exe'
$launcher = Join-Path $relDir 'catofficial.exe'
foreach ($required in $core, $launcher) { if (-not (Test-Path $required)) { Fail "找不到 $required，请先编译 Release" } }
if (-not (Test-Path (Join-Path $WebDist 'index.html'))) { Fail "找不到前端文件 $WebDist\index.html，请先在 webui 目录运行 npm run build" }

$name = "CatOfficial-$Version-windows-$Architecture"
$out = Join-Path $root 'release'
$stageRoot = Join-Path $out '.stage'
$stage = Join-Path $stageRoot 'CatOfficial-beta'   # 内层文件夹固定，避免解压出一堆带版本号的文件夹
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$appStage = Join-Path $stage 'app'
New-Item -ItemType Directory -Force -Path $appStage | Out-Null

# ── 1. 启动器 / 主程序 + 依赖 ─────────────────────────────────
# 依赖 DLL 与主程序同在 app\：exe 所在目录是 Windows 解析静态导入的第一顺位，不需要设置 PATH。
# 启动器是静态链接（/MT），不依赖这些 DLL，所以放在根目录也能先于主程序运行。
Copy-Item $launcher (Join-Path $stage 'catofficial.exe')
Copy-Item $core (Join-Path $appStage 'catofficial-core.exe')
Get-ChildItem (Join-Path $relDir '*.dll') | Copy-Item -Destination $appStage

# MSVC 运行库必须与主程序同目录。用户机器 System32 里的旧版运行库可能早于编译所用的工具集，
# 微软不保证兼容；随包的这份在搜索顺序上优先（Dice!Next 曾因此崩溃）。
$runtimeDirs = @()
$redistArch = if ($Architecture -eq 'arm64') { 'arm64' } else { 'x64' }
foreach ($vsRoot in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
    if (-not $vsRoot) { continue }
    foreach ($edition in 'BuildTools', 'Community', 'Professional', 'Enterprise') {
        $redistRoot = Join-Path $vsRoot "Microsoft Visual Studio\2022\$edition\VC\Redist\MSVC"
        if (-not (Test-Path $redistRoot)) { continue }
        foreach ($verDir in (Get-ChildItem $redistRoot -Directory | Sort-Object Name -Descending)) {
            $candidate = Join-Path $verDir.FullName "$redistArch\Microsoft.VC143.CRT"
            if (Test-Path $candidate) { $runtimeDirs += $candidate }
        }
    }
}
# System32 只在架构一致时能兜底；x64 主机打 arm64 包时它是错的架构。
if ($Architecture -ne 'arm64' -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
    $runtimeDirs += (Join-Path $env:SystemRoot 'System32')
}
foreach ($dll in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') {
    $copied = $false
    foreach ($dir in $runtimeDirs) {
        $source = Join-Path $dir $dll
        if (Test-Path $source) { Copy-Item $source $appStage -Force; $copied = $true; break }
    }
    if (-not $copied) { Fail "缺少 MSVC 运行库 $dll（$Architecture），请安装 VS 2022 生成工具的 C++ 重发行包" }
}
$dllCount = (Get-ChildItem (Join-Path $appStage '*.dll') | Measure-Object).Count

# ── 2. 管理面板、镜像列表与文档 ───────────────────────────────
# 配置里 webui.root 的默认值是 web（相对配置文件所在的安装目录）。
Copy-Item $WebDist (Join-Path $stage 'web') -Recurse
Copy-Item 'resources\update-mirrors.json' $stage
Copy-Item LICENSE, README.md $stage

# 与在线更新的包校验保持一致（update_service.cpp 的 missingWindowsPackageComponents）。
foreach ($required in 'catofficial.exe', 'app\catofficial-core.exe', 'app\msvcp140.dll', 'app\vcruntime140.dll', 'app\vcruntime140_1.dll', 'web\index.html', 'update-mirrors.json', 'LICENSE') {
    if (-not (Test-Path (Join-Path $stage $required))) { Fail "打包结果缺少 $required" }
}

$readme = @"
CatOfficial $Version — Windows $Architecture 使用说明
=======================================

【运行】
1. 解压本压缩包到任意文件夹（路径建议不含特殊字符，不要直接在压缩包里运行）。
2. 双击 catofficial.exe 启动。首次启动会在本文件夹生成 config.json，控制台里会显示管理面板密码
   （也写在 config.json 的 webui.password 里），登录后请按提示设置你自己的密码。
3. 浏览器打开管理面板：http://localhost:18082
   之后每次启动都会最小化到右下角托盘。右键托盘图标可以打开应用目录、显示或隐藏控制台、
   打开网页面板、设置开机自启动与退出。

【接入】
  - 在管理面板「登录管理」里添加 QQ 官方机器人（扫码或填写 AppID 与 AppSecret）。
  - 在「连接管理」里配置 OneBot V11 连接，让 Dice!、海豹等 OneBot 应用接入。

【说明】
  - catofficial.exe 是启动器：安装已下载的更新后拉起 app\catofficial-core.exe 就退出，常驻的只有主程序一个进程。
    所有依赖与运行库都在 app\ 里，通常无需额外安装。
  - 在线更新：面板「关于」里可以检查、下载并安装新版本，也可以设置自动检查与自动安装。
  - 数据保存在本文件夹的 data 目录，配置在 config.json。手动升级时解压覆盖全部文件即可，这两项不会被覆盖。
  - 同一套数据只能运行一个进程；面板端口被占用时会自动顺延到下一个空闲端口并写回配置。
  - 需要 64 位 Windows 10/11。

—— DiceZone
"@
[IO.File]::WriteAllText((Join-Path $stage '使用说明.txt'), $readme, (New-Object System.Text.UTF8Encoding($false)))

# ── 3. 压缩（UTF-8 条目名，中文文件名不乱码）──────────────────
New-Item -ItemType Directory -Force -Path $out | Out-Null
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true, [System.Text.Encoding]::UTF8)
# 部分 vcpkg DLL 带只读属性，先解除再清理临时目录。
Get-ChildItem $stageRoot -Recurse -Force | Where-Object { -not $_.PSIsContainer } | ForEach-Object { $_.IsReadOnly = $false }
Remove-Item $stageRoot -Recurse -Force

$sizeMB = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "[OK] 打包完成：release\$name.zip（$sizeMB MB，$dllCount 个 DLL）" -ForegroundColor Green
