@echo off
setlocal
:: MUST run as Administrator (right-click - Run as administrator).
:: Installs the DLL to Program Files (readable by prevhost's low-integrity process) and registers it in HKLM.
set CLSID={7f3c5a1e-2b4d-4e6a-9c8b-1d5f7a3e9c2d}
set CAT={8895b1c6-b41f-4c1c-a562-0d564250836f}
set SRC=%~dp0src\DspackPreview\bin\Release\DspackPreview.dll
set DIR=C:\Program Files\DSH-PackForge\dspack-preview
set DST=%DIR%\DspackPreview.dll

echo Installing handler to "%DIR%" ...
mkdir "%DIR%" 2>nul
copy /y "%SRC%" "%DST%" >nul
if errorlevel 1 goto :fail

set CB=%DST:\=/%
set CB=file:///%CB%

echo CLSID    = %CLSID%
echo DLL      = %DST%
echo CodeBase = %CB%
echo.

:: 1) .dspack file association -> preview handler category
reg add "HKLM\Software\Classes\.dspack\shellex\%CAT%" /ve /d "%CLSID%" /f || goto :fail

:: 2) CLSID -> managed .NET server (mscoree shim)
reg add "HKLM\Software\Classes\CLSID\%CLSID%\InprocServer32" /ve /d "mscoree.dll" /f || goto :fail
reg add "HKLM\Software\Classes\CLSID\%CLSID%\InprocServer32" /v ThreadingModel /d "Both" /f || goto :fail
reg add "HKLM\Software\Classes\CLSID\%CLSID%\InprocServer32" /v Class /d "DspackPreview.DspackPreviewHandler" /f || goto :fail
reg add "HKLM\Software\Classes\CLSID\%CLSID%\InprocServer32" /v Assembly /d "DspackPreview, Version=0.1.0.0, Culture=neutral, PublicKeyToken=null" /f || goto :fail
reg add "HKLM\Software\Classes\CLSID\%CLSID%\InprocServer32" /v RuntimeVersion /d "v4.0.30319" /f || goto :fail
reg add "HKLM\Software\Classes\CLSID\%CLSID%\InprocServer32" /v CodeBase /d "%CB%" /f || goto :fail

echo.
echo OK. Restarting Explorer...
taskkill /f /im prevhost.exe >nul 2>&1
taskkill /f /im explorer.exe >nul 2>&1
start explorer.exe
echo.
echo DONE. Open Explorer, press Alt+P, click a .dspack file.
echo Debug log: %TEMP%\dspack-preview.log
pause
exit /b 0

:fail
echo.
echo FAILED - did you run this as Administrator? (right-click - Run as administrator)
pause
exit /b 1