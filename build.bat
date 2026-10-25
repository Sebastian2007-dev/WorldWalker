@echo off
rem Configures and builds WorldWalker (Release by default) with MSVC + Ninja.
setlocal
set ROOT=%~dp0.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSPATH=
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if not defined VSPATH (echo Visual Studio C++ Build Tools not found & exit /b 1)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined VULKAN_SDK for /d %%d in (C:\VulkanSDK\*) do set VULKAN_SDK=%%d
set PATH=%ProgramFiles%\CMake\bin;%PATH%
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
cmake -S "%ROOT%" -B "%ROOT%\build\%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%ROOT%\build\%CONFIG%" || exit /b 1
echo.
echo Build OK: %ROOT%\build\%CONFIG%\bin\WorldWalker.exe
