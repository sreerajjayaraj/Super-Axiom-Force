@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 2
cd /d "%~dp0"
cl /nologo /std:c++17 /O2 /EHsc /MD /W4 /WX /DNOMINMAX /guard:cf tests\TraceTests.cpp /Fo:build\TraceTests.obj /Fe:build\TraceTests.exe /link /guard:cf
if errorlevel 1 exit /b 2
build\TraceTests.exe
exit /b %errorlevel%

