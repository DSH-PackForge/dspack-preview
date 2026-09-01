@echo off
setlocal
:: No-interactive deploy: copy DLL + regsvr32 + kill prevhost. Run elevated.
set SRCDLL=%~dp0src\DspackPreviewNative\DspackPreviewNative.dll
set DST=C:\Program Files\DSH-PackForge\dspack-preview\DspackPreviewNative.dll
taskkill /f /im prevhost.exe >nul 2>&1
copy /y "%SRCDLL%" "%DST%" >nul
if errorlevel 1 exit /b 1
%WINDIR%\System32\regsvr32.exe /s "%DST%"
exit /b 0