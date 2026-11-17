# Builds the Android APK. Needs the tools from STADIUM.md ("Android build") and a
# finished Windows build, whose host file_to_c.exe the Android build runs.
#   powershell -ExecutionPolicy Bypass -File tools\android_build.ps1
#   ... -Native    native library only
#   ... -Package   skip the native build, only stage and package
#   ... -Install   also install on the connected device
param([switch]$Native, [switch]$Package, [switch]$Install)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$androidHome = Join-Path $env:LOCALAPPDATA 'Android'
$sdk = Join-Path $androidHome 'Sdk'
$ndk = Join-Path $sdk 'ndk\27.1.12297006'
$cmake = Join-Path $sdk 'cmake\3.22.1\bin\cmake.exe'
$gradle = Join-Path $androidHome 'gradle-8.0\bin\gradle.bat'
$adb = Join-Path $sdk 'platform-tools\adb.exe'
$out = Join-Path $root 'build-android'
$env:JAVA_HOME = Join-Path $androidHome 'jdk-17'
$env:ANDROID_HOME = $sdk
$env:NoDefaultCurrentDirectoryInExePath = $null

function Invoke-Checked([string]$command) {
    # cmd keeps native stderr from tripping PowerShell 5.1's error handling.
    cmd /c $command
    if ($LASTEXITCODE -ne 0) { throw "failed ($LASTEXITCODE): $command" }
}

if (-not $Package) {
    $ninja = (Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio' -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    if (-not $ninja) { throw 'ninja.exe not found (it comes with the VS Build Tools CMake component)' }
    $fileToC = Join-Path $root 'build\file_to_c.exe'
    if (-not (Test-Path $fileToC)) { throw "missing $fileToC : run build_ssanne.bat first" }
    $toolchain = (Join-Path $ndk 'build\cmake\android.toolchain.cmake') -replace '\\', '/'
    Invoke-Checked "`"$cmake`" -S `"$root`" -B `"$out`" -G Ninja `"-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\','/')`" `"-DCMAKE_TOOLCHAIN_FILE=$toolchain`" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=RelWithDebInfo `"-DRT64_FILE_TO_C=$($fileToC -replace '\\','/')`" -DWITH_ARES_BRIDGE=OFF -DWITH_DEV_DIVERGENCE=OFF -Wno-dev > `"$root\_cfg_android.log`" 2>&1"
    Invoke-Checked "start `"`" /belownormal /b /wait `"$cmake`" --build `"$out`" --target PokemonStadiumRecomp -- -j 6 > `"$root\_build_android.log`" 2>&1"
}
if ($Native) { return }

# Stage the native libraries where app/build.gradle packages them from.
$libs = Join-Path $out 'apk-libs\arm64-v8a'
New-Item -ItemType Directory -Force $libs | Out-Null
Copy-Item (Join-Path $out 'libPokemonStadiumRecomp.so') $libs -Force
$sdl = Get-ChildItem $out -Recurse -Filter 'libSDL2.so' | Select-Object -First 1
if (-not $sdl) { $sdl = Get-ChildItem $out -Recurse -Filter 'libSDL2-*.so' | Select-Object -First 1 }
Copy-Item $sdl.FullName (Join-Path $libs 'libSDL2.so') -Force
Copy-Item (Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so') $libs -Force

Push-Location (Join-Path $root 'android')
try {
    # Debug packaging is incremental: a changed file is appended to the existing
    # APK and the old copy left as dead space, so each rebuild of the 370 MB
    # library grew the APK by another copy. Start from a fresh APK.
    Remove-Item (Join-Path $root 'android\app\build\outputs\apk') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $root 'android\app\build\intermediates\packaged_res') -Recurse -Force -ErrorAction SilentlyContinue
    Invoke-Checked "`"$gradle`" --no-daemon assembleDebug > `"$root\_gradle_android.log`" 2>&1"
} finally {
    Pop-Location
}
$apk = Join-Path $root 'android\app\build\outputs\apk\debug\app-debug.apk'
"APK: $apk  ({0:N0} MB)" -f ((Get-Item $apk).Length / 1MB)

if ($Install) {
    Invoke-Checked "`"$adb`" install -r `"$apk`""
}
