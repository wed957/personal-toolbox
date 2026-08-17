@echo off
setlocal

set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"

if not exist "%CSC%" (
    echo .NET Framework x64 compiler was not found.
    exit /b 1
)

for %%F in (FastLauncher.cs Payload.zip sdk.js IYX.ico) do (
    if not exist "%%F" (
        echo Required file is missing: %%F
        exit /b 1
    )
)

"%CSC%" ^
    /nologo ^
    /target:winexe ^
    /platform:x64 ^
    /optimize+ ^
    /win32icon:IYX.ico ^
    /out:IYX.exe ^
    /resource:Payload.zip,IYX.Payload.zip ^
    /resource:sdk.js,IYX.Sdk.js ^
    /reference:System.dll ^
    /reference:System.Core.dll ^
    /reference:System.Windows.Forms.dll ^
    /reference:System.Drawing.dll ^
    /reference:System.Net.Http.dll ^
    /reference:System.IO.Compression.dll ^
    /reference:System.IO.Compression.FileSystem.dll ^
    /reference:System.Management.dll ^
    FastLauncher.cs

if errorlevel 1 exit /b %errorlevel%

echo Built: %CD%\IYX.exe
