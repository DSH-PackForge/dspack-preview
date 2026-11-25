@echo off
setlocal
for /f "usebackq tokens=*" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCINSTALL=%%V"
if not defined VCINSTALL (echo Visual Studio C++ tools not found & exit /b 1)
call "%VCINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo vcvarsall failed & exit /b 1)
cd /d "%~dp0"

cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE capture-test.cpp /link /OUT:capture-test.exe ole32.lib user32.lib gdi32.lib uuid.lib
if errorlevel 1 (echo CAPTURE BUILD FAILED & exit /b 1)

cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE stream-test.cpp /link /OUT:stream-test.exe ole32.lib shlwapi.lib user32.lib gdi32.lib uuid.lib
if errorlevel 1 (echo STREAM BUILD FAILED & exit /b 1)

echo BUILD OK -^> capture-test.exe + stream-test.exe
