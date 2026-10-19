param([switch]$Run)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$toolBin = 'C:\msys64\ucrt64\bin'
if (Test-Path $toolBin) { $env:PATH = "$toolBin;$env:PATH" }
if (!(Test-Path 'vendor/lvgl/lvgl.h')) {
    git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git vendor/lvgl
    if ($LASTEXITCODE) { throw 'LVGL download failed' }
}
$revision = git -C vendor/lvgl rev-parse HEAD
if ($revision -ne '7f07a129e8d77f4984fff8e623fd5be18ff42e74') { throw 'Unexpected LVGL revision' }
cmake -S . -B build -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { throw 'Configure failed' }
cmake --build build --parallel 4
if ($LASTEXITCODE) { throw 'Build failed' }
ctest --test-dir build --output-on-failure
if ($LASTEXITCODE) { throw 'Verification failed' }
if ($Run) { Start-Process -FilePath "$PSScriptRoot/build/atmos.exe" -WindowStyle Hidden }
