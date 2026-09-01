@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo vcvarsall failed & exit /b 1)
cd /d "%~dp0"

set MINIZ=third_party\miniz\miniz.c third_party\miniz\miniz_tdef.c third_party\miniz\miniz_tinfl.c third_party\miniz\miniz_zip.c

:: native preview handler DLL
cl /nologo /LD /O2 /MT /EHsc /DUNICODE /D_UNICODE /DNDEBUG /utf-8 ^
   dspack-preview-native.cpp dspack-read.cpp %MINIZ% ^
   /link /DEF:dspack-preview-native.def /OUT:DspackPreviewNative.dll /SUBSYSTEM:WINDOWS ^
   advapi32.lib ole32.lib oleaut32.lib uuid.lib
if errorlevel 1 (echo DLL BUILD FAILED & exit /b 1)

:: parse smoke test exe
cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /DNDEBUG /utf-8 ^
   "%~dp0..\..\test\parse-smoke.cpp" dspack-read.cpp %MINIZ% ^
   /link /OUT:parse-smoke.exe /SUBSYSTEM:CONSOLE advapi32.lib
if errorlevel 1 (echo SMOKE BUILD FAILED & exit /b 1)

echo BUILD OK -^> DspackPreviewNative.dll + parse-smoke.exe