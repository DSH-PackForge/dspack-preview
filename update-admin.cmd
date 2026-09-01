@echo off
setlocal
:: MUST run as Administrator. Only refreshes the DLL in Program Files (registration unchanged).
set SRCDLL=%~dp0src\DspackPreviewNative\DspackPreviewNative.dll
set DST=C:\Program Files\DSH-PackForge\dspack-preview\DspackPreviewNative.dll
taskkill /f /im prevhost.exe >nul 2>&1
copy /y "%SRCDLL%" "%DST%" >nul || (echo FAILED - right-click and Run as administrator & pause & exit /b 1)
echo.
echo Updated. Now open Explorer, press Alt+P, click a .dspack file.
echo Debug log: %USERPROFILE%\AppData\LocalLow\dspack-preview-native.log
pause
exit /b 0