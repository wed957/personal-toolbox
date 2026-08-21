@echo off
setlocal
set "GXX=%USERPROFILE%\scoop\apps\mingw\current\bin\g++.exe"
set "WINDRES=%USERPROFILE%\scoop\apps\mingw\current\bin\windres.exe"
if not exist "%GXX%" (
  echo MinGW g++.exe not found.
  exit /b 1
)
if not exist "%WINDRES%" (
  echo MinGW windres.exe not found.
  exit /b 1
)
if not exist "%~dp0build" mkdir "%~dp0build"
"%GXX%" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wno-cast-function-type -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX "%~dp0src\main.cpp" -o "%~dp0build\icc-switch.exe" -luser32 -static-libgcc -static-libstdc++
if errorlevel 1 exit /b 1
"%WINDRES%" --target=pe-x86-64 "%~dp0src\app.rc" -O coff -o "%~dp0build\app-resources.o"
if errorlevel 1 exit /b 1
"%GXX%" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wno-cast-function-type -municode -mwindows -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX "%~dp0src\gui.cpp" "%~dp0build\app-resources.o" -o "%~dp0build\icc-switch-gui.exe" -luser32 -lgdi32 -lcomdlg32 -lgdiplus -static-libgcc -static-libstdc++
exit /b %errorlevel%
