@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 2
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++latest /I"..\deps\safetyhook\include" /O2 /EHsc /MD /W4 /WX /DNOMINMAX /guard:cf tests\UnifiedTargetingTests.cpp /Fo:build\UnifiedTargetingTests.obj /Fe:build\UnifiedTargetingTests.exe /link /BASE:0x180000000 /guard:cf psapi.lib "..\deps\lib\safetyhook.lib" "..\deps\lib\Zydis.lib" "..\deps\lib\Zycore.lib"
if errorlevel 1 exit /b 2
build\UnifiedTargetingTests.exe %*
exit /b %errorlevel%

