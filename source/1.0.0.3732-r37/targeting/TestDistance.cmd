@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 2
cd /d "%~dp0"
cl /nologo /std:c++17 /O2 /EHsc /MD /W4 /WX /DNOMINMAX tests\DistanceOwnerTests.cpp /Fo:build\DistanceOwnerTests.obj /Fe:build\DistanceOwnerTests.exe
if errorlevel 1 exit /b 2
build\DistanceOwnerTests.exe
exit /b %errorlevel%
