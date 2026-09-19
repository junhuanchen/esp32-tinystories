[CmdletBinding()]
param(
  [Parameter(Mandatory)]
  [ValidateSet('tinystories', 'barista')]
  [string]$ModelKind,

  [Parameter(Mandatory)]
  [string]$Port
)

# Flash a verified model and matching firmware to an ESP32-S3 from Windows.
# The board holds one model partition. Compiling and host verification happen
# before either write, so a failed build cannot replace the running firmware.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function Require-Command([string]$Name) {
  $command = Get-Command $Name -ErrorAction SilentlyContinue
  if (-not $command) { throw "missing required command: $Name" }
  return $command.Source
}

function Invoke-Step([string]$Label, [string]$Command, [string[]]$Arguments) {
  Write-Host "=== $Label ==="
  & $Command @Arguments
  if ($LASTEXITCODE -ne 0) { throw "failed: $Label" }
}

switch ($ModelKind) {
  'tinystories' {
    $Sketch = 'firmware/esp32_tinystories'
    $Artifacts = if ($env:ARTIFACTS) { $env:ARTIFACTS } else { 'artifacts/tinystories' }
    $Required = @('model.bin', 'tokenizer.json')
  }
  'barista' {
    $Sketch = 'firmware/esp32_barista'
    $Artifacts = if ($env:ARTIFACTS) { $env:ARTIFACTS } else { 'artifacts/barista' }
    $Required = @('model.bin', 'tokenizer.json', 'vocab.json', 'layout.json')
  }
}

$Model = if ($env:MODEL) { $env:MODEL } else { Join-Path $Artifacts 'model.bin' }
$Tokenizer = if ($env:TOKENIZER) { $env:TOKENIZER } else { Join-Path $Artifacts 'tokenizer.json' }
$Golden = if ($env:GOLDEN) { $env:GOLDEN } else { Join-Path $Artifacts 'golden.txt' }
$Vocab = if ($env:VOCAB) { $env:VOCAB } else { Join-Path $Artifacts 'vocab.json' }
$Layout = if ($env:LAYOUT) { $env:LAYOUT } else { Join-Path $Artifacts 'layout.json' }

$RequiredPaths = foreach ($File in $Required) {
  switch ($File) {
    'model.bin' { $Model }
    'tokenizer.json' { $Tokenizer }
    'vocab.json' { $Vocab }
    'layout.json' { $Layout }
  }
}
$Missing = $RequiredPaths | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }
if ($Missing) {
  $Missing | ForEach-Object { Write-Error "missing artifact: $_" }
  throw "download the $ModelKind artifacts before deploying"
}

$Uv = Require-Command 'uv'
$ArduinoCli = Require-Command 'arduino-cli'
$Python = Require-Command 'python'
$Compiler = if ($env:CC) { Require-Command $env:CC } elseif (Get-Command 'gcc' -ErrorAction SilentlyContinue) { 'gcc' } elseif (Get-Command 'clang' -ErrorAction SilentlyContinue) { 'clang' } else { throw 'missing required C compiler: install GCC or Clang, or set CC' }
$Esptool = @('esptool.exe', 'esptool', 'esptool.py') |
  ForEach-Object { Get-Command $_ -ErrorAction SilentlyContinue } |
  Select-Object -First 1
if (-not $Esptool) { throw 'missing required command: esptool (install esptool)' }

if ($ModelKind -eq 'tinystories') {
  Invoke-Step 'generate vocab.h' $Uv @('run', '--no-project', '--with', 'tokenizers==0.23.1', 'python', "$Sketch/tools/generate_vocab.py", '--tokenizer', $Tokenizer, '--out', "$Sketch/generated/vocab.h")
  Invoke-Step 'generate TinyStories tokenizer asset' $Uv @('run', '--no-project', 'python', 'firmware/esp32_barista/tools/generate_tokenizer_header.py', '--tokenizer', $Tokenizer, '--out', "$Sketch/generated/tokenizer_encoder.h")
} else {
  Invoke-Step 'generate Barista word tables' $Uv @('run', '--no-project', 'python', "$Sketch/tools/generate_vocab_headers.py", '--vocab', $Vocab, '--layout', $Layout, '--out-dir', "$Sketch/generated")
  Invoke-Step 'generate Barista tokenizer asset' $Uv @('run', '--no-project', 'python', "$Sketch/tools/generate_tokenizer_header.py", '--tokenizer', $Tokenizer, '--out', "$Sketch/generated/tokenizer_encoder.h")
}

$RunDir = Join-Path ([IO.Path]::GetTempPath()) ("esp32ai-deploy-" + [guid]::NewGuid())
$GateDir = Join-Path $RunDir 'gates'
$BuildDir = Join-Path $RunDir 'build'
New-Item -ItemType Directory -Force -Path $GateDir, $BuildDir | Out-Null
try {
  if (Test-Path -LiteralPath $Golden -PathType Leaf) {
    $Verify = Join-Path $GateDir 'llm_verify.exe'
    Invoke-Step 'host verify: int4 path' $Compiler @('-O3', '-Wall', '-Wextra', '-o', $Verify, 'runtime/host_verify/verify.c', '-lm')
    Invoke-Step 'host verify: PyTorch golden' $Verify @($Model, $Golden)
  } else {
    Write-Host "=== host verify: SKIPPED, no golden at $Golden ==="
  }

  $Staging = Join-Path $GateDir 'llm_staging.exe'
  Invoke-Step 'host verify: int8 staging' $Compiler @('-O3', '-Wall', '-Wextra', '-DLLM_INT8_ACT=1', '-o', $Staging, 'runtime/host_verify/staging_verify.c', '-lm')
  Invoke-Step 'host verify: int8 staging run' $Staging @($Model)

  if ($ModelKind -eq 'barista') {
    Invoke-Step 'host verify: Barista tokenizer' $Uv @('run', '--no-project', '--with', 'tokenizers==0.23.1', 'python', "$Sketch/tools/verify_tokenizer.py", '--tokenizer', $Tokenizer, '--vocab', $Vocab, '--layout', $Layout)
  }

  $Fqbn = 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=info'
  Invoke-Step "compile $Sketch" $ArduinoCli @('compile', '--fqbn', $Fqbn, '--build-property', 'compiler.optimization_flags=-O3', '--build-path', $BuildDir, $Sketch)

  $Fingerprint = & $Python -c 'from functools import reduce; import sys; h=reduce(lambda h,b: ((h ^ b) * 16777619) & 0xffffffff, open(sys.argv[1], "rb").read(), 2166136261); print(f"{h:08x}")' $Model
  if ($LASTEXITCODE -ne 0) { throw 'failed: model fingerprint' }
  Write-Host "=== about to flash $ModelKind to $Port (fp=$Fingerprint) ==="
  Invoke-Step 'flash model' $Esptool.Source @('--chip', 'esp32s3', '--port', $Port, '--baud', '921600', 'write_flash', '0x110000', $Model)
  Invoke-Step 'upload firmware' $ArduinoCli @('upload', '-p', $Port, '--fqbn', $Fqbn, '--input-dir', $BuildDir, $Sketch)
} finally {
  Remove-Item -LiteralPath $RunDir -Recurse -Force -ErrorAction SilentlyContinue
}
