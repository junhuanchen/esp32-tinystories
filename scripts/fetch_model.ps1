[CmdletBinding()]
param(
  [Parameter(Mandatory)]
  [ValidateSet('tinystories', 'barista')]
  [string]$ModelKind
)

# Download released inference assets on Windows. Nothing is installed until all
# files and metadata agree with the pinned hashes below.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

switch ($ModelKind) {
  'tinystories' {
    $Repository = 'slvDev/esp32-ai-tinystories'
    $Pinned = @(
      @{ Name = 'model.bin'; Sha256 = '1d8326c05c383ccfa615f5455575802817cb453dbc7ab28875d41a9dbb45477e'; Bytes = 14912348 },
      @{ Name = 'tokenizer.json'; Sha256 = '4e28163669f2249af31a528a54fc25064dcbd0a34edbfa7bedb16d2d600ec7ae'; Bytes = 1788896 }
    )
  }
  'barista' {
    $Repository = 'slvDev/esp32-ai-barista'
    $Pinned = @(
      @{ Name = 'model.bin'; Sha256 = '1359a1cb74de4143d630c2c192990de814cd47255bcdfa9cc135f07ef0a39fc4'; Bytes = 4600186 },
      @{ Name = 'tokenizer.json'; Sha256 = '0ad085811c949f35c5f5f15b555f2ff2d46ec1ec94a1650552416d06aaa19ee2'; Bytes = 491735 },
      @{ Name = 'vocab.json'; Sha256 = '5a16d6224abf03265d69ebcccf121c8f8d2c222bfedd8274acbc0bbbe13e4eb7'; Bytes = 50494 },
      @{ Name = 'layout.json'; Sha256 = '15036c5ee2b23b9b35404ef6422cb788bbede8cf2c269bae35d4dbf9a48a0b90'; Bytes = 5142 }
    )
  }
}

$Staging = Join-Path ([IO.Path]::GetTempPath()) ("esp32ai-fetch-" + [guid]::NewGuid())
$Destination = Join-Path $Root (Join-Path 'artifacts' $ModelKind)
$Backup = "$Destination.backup-" + [guid]::NewGuid()
$Installed = $false
New-Item -ItemType Directory -Path $Staging | Out-Null
try {
  $Names = @($Pinned | ForEach-Object Name) + 'metadata.json'
  foreach ($Name in $Names) {
    $Uri = "https://huggingface.co/$Repository/resolve/main/${Name}?download=true"
    Write-Host "=== download $Name ==="
    Invoke-WebRequest -Uri $Uri -OutFile (Join-Path $Staging $Name)
  }

  Write-Host '=== verify ==='
  foreach ($File in $Pinned) {
    $Path = Join-Path $Staging $File.Name
    $Bytes = (Get-Item -LiteralPath $Path).Length
    $Hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Bytes -ne $File.Bytes) {
      throw "$($File.Name) size $Bytes, expected $($File.Bytes)"
    }
    if ($Hash -ne $File.Sha256) {
      throw "$($File.Name) SHA-256 $Hash, expected $($File.Sha256)"
    }
    Write-Host ("  {0,-16} ok  {1} B" -f $File.Name, $Bytes)
  }

  $Metadata = Get-Content -LiteralPath (Join-Path $Staging 'metadata.json') -Raw |
    ConvertFrom-Json
  foreach ($File in $Pinned) {
    $Record = $Metadata.files.($File.Name)
    if ($null -eq $Record -or $Record.sha256 -ne $File.Sha256 -or
        [int64]$Record.bytes -ne $File.Bytes) {
      throw "metadata.json disagrees with the pinned $($File.Name)"
    }
  }
  Write-Host '  metadata.json  ok  agrees with pinned files'

  $Incoming = Join-Path (Split-Path -Parent $Destination) (".$ModelKind.incoming-" + [guid]::NewGuid())
  New-Item -ItemType Directory -Path $Incoming | Out-Null
  foreach ($Name in $Names) {
    Copy-Item -LiteralPath (Join-Path $Staging $Name) -Destination (Join-Path $Incoming $Name)
  }
  if (Test-Path -LiteralPath $Destination) {
    [IO.Directory]::Move($Destination, $Backup)
  }
  try {
    [IO.Directory]::Move($Incoming, $Destination)
  } catch {
    if (Test-Path -LiteralPath $Backup) { [IO.Directory]::Move($Backup, $Destination) }
    throw
  }
  if (Test-Path -LiteralPath $Backup) { Remove-Item -LiteralPath $Backup -Recurse -Force }
  $Installed = $true
  Write-Host "installed into artifacts/$ModelKind/"
} finally {
  if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
  if (-not $Installed -and (Test-Path -LiteralPath $Backup)) {
    Write-Warning "previous artifacts were preserved at $Backup"
  }
}
