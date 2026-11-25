@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 2
cd /d "%~dp0"
cl /nologo /std:c++17 /O2 /EHsc /MD /W4 /WX /DNOMINMAX /guard:cf %* tests\LifecycleTests.cpp /Fo:build\LifecycleTests.obj /Fe:build\LifecycleTests.exe /link /guard:cf bcrypt.lib
if errorlevel 1 exit /b 2
build\LifecycleTests.exe
set "first_result=%errorlevel%"
build\LifecycleTests.exe 601
if not "%first_result%"=="0" exit /b %first_result%
exit /b %errorlevel%
