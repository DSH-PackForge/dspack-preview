# Verify the registered .dspack preview handler can be created via COM.
# Must run under Windows PowerShell 5.1 (.NET Framework): powershell.exe -NoProfile -File register-test.ps1
param([string]$Clsid = "{7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d}")
try {
    $t = [Type]::GetTypeFromCLSID($Clsid)
    $o = [Activator]::CreateInstance($t)
    Write-Output ("COM create OK: " + $o.GetType().FullName)
}
catch {
    Write-Output ("COM create FAIL: " + $_.Exception.GetType().Name + " - " + $_.Exception.Message)
}