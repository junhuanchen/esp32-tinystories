[CmdletBinding()]
param(
  [string]$Port,
  [switch]$Flash,
  [switch]$Monitor,
  [switch]$Clean,
  [string]$IdfPath = $env:IDF_PATH
)

# Build the standalone ESP-IDF firmware. Flashing is opt-in because it replaces
# the current partition table, application, and model on the connected board.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $Root 'idf'
if (-not (Test-Path -LiteralPath (Join-Path $Project 'CMakeLists.txt'))) {
  throw "ESP-IDF project not found: $Project"
}

if (-not $IdfPath) {
  $Candidates = @(
    (Get-ChildItem 'C:\Espressif\frameworks\esp-idf-*' -Directory -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName),
    (Join-Path $env:USERPROFILE 'esp\esp-idf')
  ) | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'export.ps1')) }
  $IdfPath = $Candidates | Select-Object -First 1
}

if (-not $IdfPath -or -not (Test-Path -LiteralPath (Join-Path $IdfPath 'export.ps1'))) {
  throw 'ESP-IDF was not found. Install ESP-IDF, or pass -IdfPath C:\path\to\esp-idf.'
}

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
  . (Join-Path $IdfPath 'export.ps1')
  if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "ESP-IDF activation did not provide idf.py: $IdfPath"
  }
}

Push-Location $Project
try {
  if ($Clean) {
    & idf.py fullclean
    if ($LASTEXITCODE -ne 0) { throw 'failed: idf.py fullclean' }
  }

  & idf.py set-target esp32s3
  if ($LASTEXITCODE -ne 0) { throw 'failed: idf.py set-target esp32s3' }

  & idf.py build
  if ($LASTEXITCODE -ne 0) { throw 'failed: idf.py build' }

  if ($Monitor -and -not $Flash) {
    throw '-Monitor requires -Flash and -Port COMx in the same invocation.'
  }
  if (-not $Flash) {
    Write-Host 'build complete; add -Flash -Port COM5 to write the board.'
    return
  }
  if (-not $Port) { throw '-Flash requires -Port COMx.' }

  & idf.py -p $Port flash
  if ($LASTEXITCODE -ne 0) { throw 'failed: idf.py flash' }

  if ($Monitor) {
    & idf.py -p $Port monitor
    if ($LASTEXITCODE -ne 0) { throw 'failed: idf.py monitor' }
  }
} finally {
  Pop-Location
}
