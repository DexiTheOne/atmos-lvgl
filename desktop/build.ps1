param([switch]$Run)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$toolBin = 'C:\msys64\ucrt64\bin'
if (Test-Path $toolBin) { $env:PATH = "$toolBin;$env:PATH" }
if (!(Test-Path 'vendor/lvgl/lvgl.h')) {
    git -c core.longpaths=true -c advice.detachedHead=false clone --quiet --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git vendor/lvgl
    if ($LASTEXITCODE) { throw 'LVGL download failed' }
}
$revision = git -C vendor/lvgl rev-parse HEAD
# Match ESPHome's LVGL (esphome/components/lvgl LVGL_VERSION). Delete vendor/lvgl to re-fetch after a change.
if ($revision -ne '85aa60d18b3d5e5588d7b247abf90198f07c8a63') { throw 'Unexpected LVGL revision: expected v9.5.0. Delete desktop/vendor/lvgl and rerun.' }
cmake -S . -B build -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { throw 'Configure failed' }
cmake --build build --parallel 4
if ($LASTEXITCODE) { throw 'Build failed' }
ctest --test-dir build --output-on-failure
if ($LASTEXITCODE) { throw 'Verification failed' }
if ($Run) { Start-Process -FilePath "$PSScriptRoot/build/atmos.exe" -WindowStyle Hidden }
