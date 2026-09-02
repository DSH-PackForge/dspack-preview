@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo vcvarsall failed & exit /b 1)
cd /d "%~dp0"

cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE capture-test.cpp /link /OUT:capture-test.exe ole32.lib user32.lib gdi32.lib uuid.lib
if errorlevel 1 (echo CAPTURE BUILD FAILED & exit /b 1)

cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE stream-test.cpp /link /OUT:stream-test.exe ole32.lib shlwapi.lib user32.lib gdi32.lib uuid.lib
if errorlevel 1 (echo STREAM BUILD FAILED & exit /b 1)

echo BUILD OK -^> capture-test.exe + stream-test.exe
