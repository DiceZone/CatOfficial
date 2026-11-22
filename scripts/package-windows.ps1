# CatOfficial Windows 打包脚本
#   用法：scripts\package-windows.ps1 -Version 0.1.0-beta.1 [-Architecture amd64|arm64] [-BuildDir build\msvc] [-WebDist webui\dist]
#   产物：release\CatOfficial-<Version>-windows-<Architecture>.zip
#   内容：catofficial.exe、依赖 DLL、MSVC 运行库、web\（管理面板）、LICENSE、README.md、使用说明.txt。
#   不打包配置文件与 data\：首次运行会在程序旁边生成，升级覆盖解压不会丢数据。
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
$exe = Join-Path $relDir 'catofficial.exe'
if (-not (Test-Path $exe)) { Fail "找不到 $exe，请先编译 Release" }
if (-not (Test-Path (Join-Path $WebDist 'index.html'))) { Fail "找不到前端文件 $WebDist\index.html，请先在 webui 目录运行 npm run build" }

$name = "CatOfficial-$Version-windows-$Architecture"
$out = Join-Path $root 'release'
$stageRoot = Join-Path $out '.stage'
$stage = Join-Path $stageRoot $name
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# ── 1. 程序与依赖 ─────────────────────────────────────────────
# 依赖 DLL 与 exe 同目录：exe 所在目录是 Windows 解析静态导入的第一顺位，不需要设置 PATH。
Copy-Item $exe $stage
Get-ChildItem (Join-Path $relDir '*.dll') | Copy-Item -Destination $stage

# MSVC 运行库也放在 exe 旁边。用户机器 System32 里的旧版运行库可能早于编译所用的工具集，
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
        if (Test-Path $source) { Copy-Item $source $stage -Force; $copied = $true; break }
    }
    if (-not $copied) { Fail "缺少 MSVC 运行库 $dll（$Architecture），请安装 VS 2022 生成工具的 C++ 重发行包" }
}
$dllCount = (Get-ChildItem (Join-Path $stage '*.dll') | Measure-Object).Count

# ── 2. 管理面板与文档 ─────────────────────────────────────────
# 配置里 webui.root 的默认值是 web（相对配置文件所在目录）。
Copy-Item $WebDist (Join-Path $stage 'web') -Recurse
Copy-Item LICENSE, README.md $stage

foreach ($required in 'catofficial.exe', 'web\index.html', 'msvcp140.dll', 'vcruntime140.dll', 'LICENSE') {
    if (-not (Test-Path (Join-Path $stage $required))) { Fail "打包结果缺少 $required" }
}

$readme = @"
CatOfficial $Version — Windows $Architecture 使用说明
=======================================

【运行】
1. 解压本压缩包到任意文件夹（路径建议不含特殊字符）。
2. 双击 catofficial.exe 启动。首次启动会在程序旁边生成 config.json，控制台里会显示管理面板密码
   （也写在 config.json 的 webui.password 里）。
3. 浏览器打开管理面板：http://localhost:18082
   之后每次启动都会最小化到右下角托盘。右键托盘图标可以打开应用目录、显示或隐藏控制台、
   打开网页面板、设置开机自启动与退出。

【接入】
  - 在管理面板「登录管理」里添加 QQ 官方机器人（扫码或填写 AppID 与 AppSecret）。
  - 在「连接」里配置 OneBot V11 连接，让 Dice!、海豹等 OneBot 应用接入。

【说明】
  - 数据保存在程序旁边的 data 目录，配置在 config.json。升级时解压覆盖全部文件即可，这两项不会被覆盖。
  - 面板端口默认 18082，可在面板「设置」里修改（需要重启）。
  - 需要 64 位 Windows 10/11。所有依赖与运行库都已随包附带，通常无需额外安装。
  - 不想启动时最小化到托盘：在面板「设置 → Windows 托盘」关闭「启动后最小化到托盘」。

—— DiceZone
"@
[IO.File]::WriteAllText((Join-Path $stage '使用说明.txt'), $readme, (New-Object System.Text.UTF8Encoding($false)))

# ── 3. 压缩（UTF-8 条目名，中文文件名不乱码）──────────────────
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true, [System.Text.Encoding]::UTF8)
# 部分 vcpkg DLL 带只读属性，先解除再清理临时目录。
Get-ChildItem $stageRoot -Recurse -Force | Where-Object { -not $_.PSIsContainer } | ForEach-Object { $_.IsReadOnly = $false }
Remove-Item $stageRoot -Recurse -Force

$sizeMB = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "[OK] 打包完成：release\$name.zip（$sizeMB MB，$dllCount 个 DLL）" -ForegroundColor Green
