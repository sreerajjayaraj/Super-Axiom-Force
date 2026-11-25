@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 2
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W4 /WX /DNOMINMAX tests\ProducerIdentityTests.cpp /Fo:build\ProducerIdentityTests.obj /Fe:build\ProducerIdentityTests.exe
if errorlevel 1 exit /b 2
build\ProducerIdentityTests.exe %*
exit /b %errorlevel%
