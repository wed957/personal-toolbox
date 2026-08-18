@echo off
setlocal

set "ROOT=%~dp0"
set "TOOLCHAIN=C:\Users\Administrator\scoop\apps\mingw\current\bin"
if defined MUX_TOOLCHAIN set "TOOLCHAIN=%MUX_TOOLCHAIN%"
set "CXX=%TOOLCHAIN%\g++.exe"
set "WINDRES=%TOOLCHAIN%\windres.exe"

if not exist "%CXX%" set "CXX=g++.exe"
if not exist "%WINDRES%" set "WINDRES=windres.exe"

if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%ROOT%dist" mkdir "%ROOT%dist"

set "COMMON=-std=c++20 -Os -flto -fno-rtti -DNDEBUG -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -finput-charset=UTF-8"

echo [1/6] Compiling display manager...
"%CXX%" %COMMON% -c "%ROOT%src\display_manager.cpp" -o "%ROOT%build\display_manager.o" || exit /b 1

echo [2/6] Compiling GUI...
"%CXX%" %COMMON% -c "%ROOT%src\main.cpp" -o "%ROOT%build\main.o" || exit /b 1

echo [3/6] Compiling CLI...
"%CXX%" %COMMON% -c "%ROOT%src\cli.cpp" -o "%ROOT%build\cli.o" || exit /b 1

echo [4/6] Compiling resources...
pushd "%ROOT%"
"%WINDRES%" --codepage=65001 app.rc -O coff -o "build\app.o" || (popd & exit /b 1)
popd

echo [5/6] Linking standalone executables...
"%CXX%" -Os -flto -static -s -mwindows -Wl,--gc-sections "%ROOT%build\main.o" "%ROOT%build\display_manager.o" "%ROOT%build\app.o" -o "%ROOT%dist\MUX.exe" -lcomctl32 -luser32 -lgdi32 || exit /b 1
"%CXX%" -Os -flto -static -s -municode -Wl,--gc-sections "%ROOT%build\cli.o" "%ROOT%build\display_manager.o" -o "%ROOT%dist\MUX-cli.exe" -luser32 || exit /b 1

echo [6/6] Verifying outputs...
if not exist "%ROOT%dist\MUX.exe" exit /b 1
if not exist "%ROOT%dist\MUX-cli.exe" exit /b 1

echo Build complete: %ROOT%dist
endlocal
