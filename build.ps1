$ErrorActionPreference = "Stop"

$msysRoot = "D:\msys64"
$bash = Join-Path $msysRoot "usr\bin\bash.exe"

if (-not (Test-Path $bash)) {
    Write-Host "MSYS2 was not found at C:\msys64."
    Write-Host "Install MSYS2 from https://www.msys2.org/, open the MSYS2 shell once, update packages, then rerun this script."
    Write-Host "Expected bash path: $bash"
    exit 1
}

$projectPath = (Resolve-Path $PSScriptRoot).Path
$msysProjectPath = $projectPath -replace "\\", "/"
if ($msysProjectPath -match "^([A-Za-z]):/(.*)$") {
    $drive = $matches[1].ToLowerInvariant()
    $rest = $matches[2]
    $msysProjectPath = "/$drive/$rest"
}

$command = @"
set -e
cd '$msysProjectPath'
pacman -S --needed --noconfirm mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-libjpeg-turbo mingw-w64-x86_64-cmake mingw-w64-x86_64-toolchain mingw-w64-x86_64-libimobiledevice mingw-w64-x86_64-libusbmuxd
mkdir -p build
cd build
cmake .. -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release
mingw32-make
cp droidcam-whip.exe ..
"@

$env:MSYSTEM = "MINGW64"
& $bash -lc $command
