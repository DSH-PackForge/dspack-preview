<#
  Build a dshhome test .dspack from test/fixtures-dshhome/ (manifest v5 + dspack.json v3).
  Usage: .\make-dshhome-pack.ps1 [-OutFile whale-studio-1.0.0.dspack]
#>
param(
    [string]$OutFile = "whale-studio-1.0.0.dspack"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$fixtures = Join-Path $ScriptDir "fixtures-dshhome"

if (-not (Test-Path (Join-Path $fixtures "manifest.json"))) {
    throw "fixtures-dshhome/manifest.json not found under $fixtures"
}
if (-not (Test-Path (Join-Path $fixtures "dspack.json"))) {
    throw "fixtures-dshhome/dspack.json not found under $fixtures"
}

$outPath = Join-Path $ScriptDir $OutFile
if (Test-Path $outPath) { Remove-Item $outPath -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($fixtures, $outPath)
Write-Host "OK -> $outPath   ($((Get-Item $outPath).Length) bytes, plain zip)"
