# Build CatOfficial for Android (arm64-v8a): native .so + WebUI assets + Gradle APK.
# Requires: Android SDK + NDK, CMake, Ninja, vcpkg, Node.js, JDK 17.
# Optional env: ANDROID_HOME / ANDROID_SDK_ROOT, ANDROID_NDK_HOME, VCPKG_ROOT, JAVA_HOME.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function First-Existing([string[]]$paths) {
    foreach ($p in $paths) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

if (-not $env:JAVA_HOME) {
    $env:JAVA_HOME = First-Existing @(
        "E:\jdk-17",
        "E:\jdk-17\jdk-17.0.21.1+2",
        (Get-ChildItem "E:\jdk-17" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName),
        (Get-ChildItem "C:\Program Files\Microsoft" -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "jdk-17*" } | Select-Object -First 1 -ExpandProperty FullName),
        (Get-ChildItem "C:\Program Files\Eclipse Adoptium" -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "jdk-17*" } | Select-Object -First 1 -ExpandProperty FullName)
    )
}
if ($env:JAVA_HOME) { $env:PATH = "$($env:JAVA_HOME)\bin;$env:PATH" }

$ninja = First-Existing @(
    (Get-Command ninja -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)
if ($ninja) {
    $env:PATH = "$(Split-Path $ninja);$env:PATH"
    $env:CMAKE_MAKE_PROGRAM = $ninja
}

if (-not $env:ANDROID_HOME -and $env:ANDROID_SDK_ROOT) { $env:ANDROID_HOME = $env:ANDROID_SDK_ROOT }
if (-not $env:ANDROID_HOME) {
    $env:ANDROID_HOME = First-Existing @("E:\Android\Sdk", "$env:LOCALAPPDATA\Android\Sdk")
}
if ($env:ANDROID_HOME) { $env:ANDROID_SDK_ROOT = $env:ANDROID_HOME }

if (-not $env:ANDROID_NDK_HOME) {
    $sdk = $env:ANDROID_HOME
    if ($sdk) {
        $ndkRoot = Join-Path $sdk "ndk"
        if (Test-Path $ndkRoot) {
            $env:ANDROID_NDK_HOME = Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1 | ForEach-Object { $_.FullName }
        }
        $bundle = Join-Path $sdk "ndk-bundle"
        if (-not $env:ANDROID_NDK_HOME -and (Test-Path $bundle)) { $env:ANDROID_NDK_HOME = $bundle }
    }
}
if (-not $env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = First-Existing @("D:\vcpkg", "C:\vcpkg", "$env:USERPROFILE\vcpkg")
}

function Require-Env([string]$name) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrWhiteSpace($value)) { throw "Please set $name" }
    return $value
}

$Ndk = Require-Env "ANDROID_NDK_HOME"
$Vcpkg = Require-Env "VCPKG_ROOT"
$Sdk = Require-Env "ANDROID_HOME"
$env:VCPKG_KEEP_ENV_VARS = "ANDROID_NDK_HOME;ANDROID_SDK_ROOT;ANDROID_HOME;JAVA_HOME"
$Triplet = "arm64-android"
$Abi = "arm64-v8a"
$Api = "26"
$BuildDir = Join-Path $Root "build-android"
$JniDir = Join-Path $Root "android\app\src\main\jniLibs\$Abi"
$WebDir = Join-Path $Root "android\app\src\main\assets\web"
$OverlayTriplets = Join-Path $Root "vcpkg-overlays\triplets"
$VcpkgExe = Join-Path $Vcpkg "vcpkg.exe"

Write-Host "JAVA_HOME: $env:JAVA_HOME"
Write-Host "SDK: $Sdk"
Write-Host "NDK: $Ndk"
Write-Host "vcpkg: $Vcpkg"
Write-Host "ninja: $ninja"

if (-not (Test-Path $VcpkgExe)) { throw "vcpkg.exe not found at $VcpkgExe" }

$localProps = Join-Path $Root "android\local.properties"
$sdkEscaped = ($Sdk -replace '\\', '\\')
"sdk.dir=$sdkEscaped" | Set-Content -Path $localProps -Encoding ASCII

& $VcpkgExe install --triplet $Triplet --overlay-triplets $OverlayTriplets
if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed" }

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$Toolchain = Join-Path $Vcpkg "scripts\buildsystems\vcpkg.cmake"
$AndroidToolchain = Join-Path $Ndk "build\cmake\android.toolchain.cmake"
$cmakeArgs = @(
    "-S", $Root, "-B", $BuildDir, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$AndroidToolchain",
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    "-DVCPKG_OVERLAY_TRIPLETS=$OverlayTriplets",
    "-DANDROID_ABI=$Abi",
    "-DANDROID_PLATFORM=android-$Api",
    "-DANDROID_STL=c++_shared",
    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
    "-DCATOFFICIAL_BUILD_ANDROID=ON",
    "-DCATOFFICIAL_BUILD_APP=OFF",
    "-DCATOFFICIAL_BUILD_TESTS=OFF"
)
if ($env:CMAKE_MAKE_PROGRAM) { $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$($env:CMAKE_MAKE_PROGRAM)" }
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

New-Item -ItemType Directory -Force -Path $JniDir | Out-Null
$So = Get-ChildItem $BuildDir -Recurse -Filter "libcatofficial.so" | Select-Object -First 1
if (-not $So) { throw "libcatofficial.so not found" }
Copy-Item $So.FullName (Join-Path $JniDir "libcatofficial.so") -Force

$Stl = Get-ChildItem (Join-Path $Ndk "toolchains\llvm\prebuilt") -Recurse -Filter "libc++_shared.so" |
    Where-Object { $_.FullName -match "aarch64-linux-android" } |
    Select-Object -First 1
if (-not $Stl) {
    $Stl = Get-ChildItem $Ndk -Recurse -Filter "libc++_shared.so" | Where-Object { $_.FullName -match "aarch64-linux-android" } | Select-Object -First 1
}
if ($Stl) { Copy-Item $Stl.FullName (Join-Path $JniDir "libc++_shared.so") -Force }
else { Write-Warning "libc++_shared.so not found in NDK" }

$strip = Get-ChildItem (Join-Path $Ndk "toolchains\llvm\prebuilt") -Recurse -Filter "llvm-strip.exe" | Select-Object -First 1
if ($strip) {
    Get-ChildItem $JniDir -Filter "*.so" | ForEach-Object {
        & $strip.FullName --strip-unneeded $_.FullName
    }
}

Push-Location (Join-Path $Root "webui")
if (-not (Test-Path "node_modules")) { npm ci } else { npm ci --prefer-offline }
npm run build
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "webui build failed" }
Pop-Location
if (Test-Path $WebDir) { Remove-Item $WebDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $WebDir | Out-Null
Copy-Item -Path (Join-Path $Root "webui\dist\*") -Destination $WebDir -Recurse -Force

$Cert = Join-Path $Root "android\app\src\main\assets\cacert.pem"
if (-not (Test-Path $Cert)) {
    Write-Host "Downloading CA bundle for native TLS..."
    Invoke-WebRequest -Uri "https://curl.se/ca/cacert.pem" -OutFile $Cert
}

Write-Host "Assembling APK..."
Push-Location (Join-Path $Root "android")
& .\gradlew.bat --no-daemon assembleRelease
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "gradle assembleRelease failed" }
Pop-Location

$Apk = Join-Path $Root "android\app\build\outputs\apk\release\app-release.apk"
if (-not (Test-Path $Apk)) { throw "APK not found: $Apk" }
$OutDir = Join-Path $Root "release"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Out = Join-Path $OutDir "CatOfficial-android.apk"
Copy-Item $Apk $Out -Force
Write-Host "APK: $Out"
Get-Item $Out | Format-List FullName, Length, LastWriteTime
