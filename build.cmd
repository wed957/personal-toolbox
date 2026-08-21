@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "TOOLCHAIN=C:\Users\Administrator\scoop\apps\mingw\current\bin"
if defined TOOLBOX_TOOLCHAIN set "TOOLCHAIN=%TOOLBOX_TOOLCHAIN%"
set "CXX=%TOOLCHAIN%\g++.exe"

if not exist "%CXX%" (
  set "CXX=g++.exe"
  where g++.exe >nul 2>nul
  if errorlevel 1 (
    echo MinGW g++.exe was not found.
    exit /b 1
  )
)

if not exist "%ROOT%dist\tools" mkdir "%ROOT%dist\tools"
if not exist "%ROOT%build" mkdir "%ROOT%build"

echo [1/6] Building ICC Switch...
pushd "%ROOT%components\icc-switch"
call .\build.cmd
if errorlevel 1 (popd & exit /b 1)
popd

echo [2/6] Building MUX...
pushd "%ROOT%components\mux-display-switcher"
call .\build.cmd
if errorlevel 1 (popd & exit /b 1)
popd

echo [3/6] Building IYX Fast Launcher...
pushd "%ROOT%components\iyx-fast-launcher"
call .\build.cmd
if errorlevel 1 (popd & exit /b 1)
popd

echo [4/6] Compiling launcher resources...
pushd "%ROOT%src"
"%TOOLCHAIN%\windres.exe" --codepage=65001 toolbox.rc -O coff -o "%ROOT%build\toolbox-res.o"
if errorlevel 1 (popd & exit /b 1)
popd

echo [5/6] Compiling unified launcher...
"%CXX%" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wno-cast-function-type ^
  -municode -mwindows -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX ^
  -finput-charset=UTF-8 "%ROOT%src\toolbox_launcher.cpp" "%ROOT%build\toolbox-res.o" ^
  -o "%ROOT%dist\Toolbox.exe" -lgdiplus -ldwmapi -lshell32 -luser32 ^
  -static-libgcc -static-libstdc++
if errorlevel 1 exit /b 1

echo [6/6] Collecting tools...
copy /y "%ROOT%components\icc-switch\build\icc-switch-gui.exe" "%ROOT%dist\tools\icc-switch-gui.exe" >nul || exit /b 1
copy /y "%ROOT%components\icc-switch\build\icc-switch.exe" "%ROOT%dist\tools\icc-switch-cli.exe" >nul || exit /b 1
copy /y "%ROOT%components\mux-display-switcher\dist\MUX.exe" "%ROOT%dist\tools\MUX.exe" >nul || exit /b 1
copy /y "%ROOT%components\mux-display-switcher\dist\MUX-cli.exe" "%ROOT%dist\tools\MUX-cli.exe" >nul || exit /b 1
copy /y "%ROOT%components\iyx-fast-launcher\IYX.exe" "%ROOT%dist\tools\IYX.exe" >nul || exit /b 1
if exist "%ROOT%dist\tools\keyboard-check.exe" del /q "%ROOT%dist\tools\keyboard-check.exe"

if not exist "%ROOT%dist\Toolbox.exe" exit /b 1
echo Build complete: %ROOT%dist
endlocal
