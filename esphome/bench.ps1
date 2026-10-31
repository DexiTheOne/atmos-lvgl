# Build, flash and record the frame-pacing benchmark (atmos-bench.yaml).
#   ./esphome/bench.ps1 -Port COM12 -Seconds 90           # compile, flash, record
#   ./esphome/bench.ps1 -Port COM12 -Seconds 90 -NoFlash  # record only
#   ./esphome/bench.ps1 -Port COM3 -Seconds 120 -Config atmos-rlcd.yaml  # real panel
# Output: esphome/bench.log (ANSI colour codes stripped).
param([string]$Port = 'COM12', [int]$Seconds = 90, [switch]$NoFlash, [string]$Config = 'atmos-bench.yaml')
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot
$venv = Join-Path $root '.venv-esphome/Scripts'
# ESP-IDF toolchain paths exceed 260 characters unless Windows long paths are enabled.
$env:ESPHOME_ESP_IDF_PREFIX = 'C:\ESPHome\idf'
$env:PYTHONIOENCODING = 'utf-8'
Push-Location $PSScriptRoot
try {
    if (!$NoFlash) {
        & (Join-Path $venv 'esphome.exe') run $Config --device $Port --no-logs *> upload.log
        if ($LASTEXITCODE) {
            Select-String -Path upload.log -Pattern 'error' | Select-Object -First 10 | ForEach-Object Line
            throw "Build or upload failed (see esphome/upload.log)"
        }
    }
    $capture = @"
import re, sys, time, serial
port, seconds = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.5)
end = time.time() + seconds
with open('bench.log', 'w', encoding='utf-8') as f:
    while time.time() < end:
        line = s.readline().decode('utf-8', 'replace')
        if line:
            f.write(re.sub(r'\x1b\[[0-9;]*m', '', line))
"@
    & (Join-Path $venv 'python.exe') -c $capture $Port $Seconds
    Write-Output "Recorded $Seconds s to esphome/bench.log"
} finally { Pop-Location }

