@echo off
setlocal
chcp 65001 >nul

set "ROOT=%~dp0"
set "APP=%ROOT%build\icc-switch.exe"
set "BUILD=%ROOT%build.cmd"
set "WT=%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe"

if not exist "%WT%" (
  for /f "delims=" %%I in ('where wt.exe 2^>nul') do if not defined WT_FOUND set "WT_FOUND=%%I"
  if defined WT_FOUND set "WT=%WT_FOUND%"
)

if not exist "%WT%" (
  echo [ERROR] Windows Terminal wt.exe was not found.
  exit /b 1
)

if not exist "%APP%" (
  echo [INFO] Building ICC Switch...
  call "%BUILD%"
  if errorlevel 1 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
  )
)

start "" "%WT%" -w new new-tab --title "ICC Switch" -d "%ROOT%" powershell.exe -NoLogo -NoExit -Command "$Host.UI.RawUI.WindowTitle='ICC Switch'; & '.\build\icc-switch.exe' list; Write-Host ''; & '.\build\icc-switch.exe' --help; Write-Host ''; Write-Host 'Example: .\build\icc-switch.exe set profile.icc 1' -ForegroundColor Cyan"
exit /b 0
