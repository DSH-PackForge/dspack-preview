<#
  Build a test .dspack from test/fixtures/ (byte-level, no text literals with non-ASCII).

  Output layout: 8-byte "DSPK" + uint32 LE(=2) + standard ZIP (contents of fixtures/).
  Usage: .\make-test-pack.ps1 [-OutFile test-preview-1.0.0.dspack]
#>
param(
    [string]$OutFile = "test-preview-1.0.0.dspack"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$fixtures = Join-Path $ScriptDir "fixtures"
$zipPath  = Join-Path $env:TEMP ("dspack-zip-" + [guid]::NewGuid().ToString("N") + ".zip")

if (-not (Test-Path (Join-Path $fixtures "manifest.json"))) {
    throw "fixtures/manifest.json not found under $fixtures"
}

try {
    [System.IO.Compression.ZipFile]::CreateFromDirectory($fixtures, $zipPath)

    $zipBytes = [System.IO.File]::ReadAllBytes($zipPath)
    $magic = [System.Text.Encoding]::ASCII.GetBytes("DSPK")
    $ver   = [System.BitConverter]::GetBytes([uint32]2)

    $out = New-Object byte[] ($magic.Length + $ver.Length + $zipBytes.Length)
    [Array]::Copy($magic, 0, $out, 0, $magic.Length)
    [Array]::Copy($ver, 0, $out, $magic.Length, $ver.Length)
    [Array]::Copy($zipBytes, 0, $out, $magic.Length + $ver.Length, $zipBytes.Length)

    $outPath = Join-Path $ScriptDir $OutFile
    [System.IO.File]::WriteAllBytes($outPath, $out)
    Write-Host "OK -> $outPath   ($($out.Length) bytes)"
}
finally {
    Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
}