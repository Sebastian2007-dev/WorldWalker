@echo off
rem Builds WorldWalker incrementally and starts it straight from the build folder.
rem   DEV.bat                      -> Release, Start am Feldberg
rem   DEV.bat --lat 46.55 --lon 7.98   -> eigene Startposition (weitere Argumente werden durchgereicht)
rem   set WW_CONFIG=Debug ^& DEV.bat --validation   -> Debug-Build mit Vulkan-Validation
setlocal
set ROOT=%~dp0.
if "%WW_CONFIG%"=="" set WW_CONFIG=Release

call "%ROOT%\build.bat" %WW_CONFIG%
if errorlevel 1 (
    echo.
    echo Build fehlgeschlagen.
    pause
    exit /b 1
)

set EXE=%ROOT%\build\%WW_CONFIG%\bin\WorldWalker.exe
echo.
echo Starte %EXE% %*
pushd "%ROOT%\build\%WW_CONFIG%\bin"
"%EXE%" %*
set RC=%errorlevel%
popd
if not "%RC%"=="0" (
    echo.
    echo WorldWalker wurde mit Code %RC% beendet.
    pause
)
exit /b %RC%
