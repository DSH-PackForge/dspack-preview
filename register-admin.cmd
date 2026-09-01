@echo off
setlocal
:: MUST run as Administrator (right-click - Run as administrator).
:: Registers the NATIVE C++ preview handler (DspackPreviewNative.dll) machine-wide.
set CLSID={7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d}
set CAT={8895b1c6-b41f-4c1c-a562-0d564250836f}
set SRCDLL=%~dp0src\DspackPreviewNative\DspackPreviewNative.dll
set DIR=C:\Program Files\DSH-PackForge\dspack-preview
set DST=%DIR%\DspackPreviewNative.dll

echo Installing native handler to "%DIR%" ...

:: 1) clean old registration (HKLM + HKCU leftovers from the C# prototype / local tests)
reg delete "HKLM\Software\Classes\CLSID\%CLSID%" /f >nul 2>&1
reg delete "HKLM\Software\Classes\.dspack\shellex\%CAT%" /f >nul 2>&1
reg delete "HKLM\Software\Classes\DspackPreview.PreviewHandler" /f >nul 2>&1
reg delete "HKCU\Software\Classes\CLSID\%CLSID%" /f >nul 2>&1
reg delete "HKCU\Software\Classes\.dspack\shellex\%CAT%" /f >nul 2>&1
reg delete "HKCU\Software\Classes\DspackPreview.PreviewHandler" /f >nul 2>&1

:: 2) copy native DLL to Program Files, then register it (DllRegisterServer writes HKLM)
mkdir "%DIR%" 2>nul
copy /y "%SRCDLL%" "%DST%" >nul || goto :fail
%WINDIR%\System32\regsvr32.exe /s "%DST%"
if errorlevel 1 goto :fail

echo.
echo OK. Restarting Explorer...
taskkill /f /im prevhost.exe >nul 2>&1
taskkill /f /im explorer.exe >nul 2>&1
start explorer.exe
echo.
echo DONE. Open Explorer, press Alt+P, click a .dspack file.
echo Debug log: %USERPROFILE%\AppData\LocalLow\dspack-preview-native.log
pause
exit /b 0

:fail
echo.
echo FAILED - did you run this as Administrator? (right-click - Run as administrator)
pause
exit /b 1