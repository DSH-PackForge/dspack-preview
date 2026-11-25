<#
  Build a test .dspack from test/fixtures/.
  .dspack = 标准 ZIP（根含 manifest.json + dspack.json），无前导魔数字节。
  Usage: .\make-test-pack.ps1 [-OutFile test-preview-1.0.0.dspack]
#>
param(
    [string]$OutFile = "test-preview-1.0.0.dspack",
    [ValidateSet("fixtures", "fixtures-v6")][string]$Fixture = "fixtures",
    [switch]$WithoutResourceSummary
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$fixtures = Join-Path $ScriptDir $Fixture

if (-not (Test-Path (Join-Path $fixtures "manifest.json"))) {
    throw "fixtures/manifest.json not found under $fixtures"
}
if (-not (Test-Path (Join-Path $fixtures "dspack.json"))) {
    throw "fixtures/dspack.json not found under $fixtures"
}

$outPath = Join-Path $ScriptDir $OutFile
if (Test-Path $outPath) { Remove-Item $outPath -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($fixtures, $outPath)
if ($WithoutResourceSummary) {
    $archive = [System.IO.Compression.ZipFile]::Open($outPath, 'Update')
    try {
        $manifest = Get-Content -LiteralPath (Join-Path $fixtures 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $manifest.PSObject.Properties.Remove('resources')
        $archive.GetEntry('manifest.json').Delete()
        $writer = [System.IO.StreamWriter]::new($archive.CreateEntry('manifest.json').Open(), [System.Text.UTF8Encoding]::new($false))
        try { $writer.Write(($manifest | ConvertTo-Json -Depth 20)) } finally { $writer.Dispose() }
    } finally { $archive.Dispose() }
}
Write-Host "OK -> $outPath   ($((Get-Item $outPath).Length) bytes, plain zip)"
