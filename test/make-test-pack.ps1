<#
  Build a test .dspack from test/fixtures/.
  .dspack = 标准 ZIP（根含 manifest.json + dspack.json），无前导魔数字节。
  Usage: .\make-test-pack.ps1 [-OutFile test-preview-1.0.0.dspack]
#>
param(
    [string]$OutFile = "test-preview-1.0.0.dspack"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$fixtures = Join-Path $ScriptDir "fixtures"

if (-not (Test-Path (Join-Path $fixtures "manifest.json"))) {
    throw "fixtures/manifest.json not found under $fixtures"
}
if (-not (Test-Path (Join-Path $fixtures "dspack.json"))) {
    throw "fixtures/dspack.json not found under $fixtures"
}

$outPath = Join-Path $ScriptDir $OutFile
if (Test-Path $outPath) { Remove-Item $outPath -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($fixtures, $outPath)
Write-Host "OK -> $outPath   ($((Get-Item $outPath).Length) bytes, plain zip)"