$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo 'dist'

function Assert-Path([string] $path) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing required path: $path"
    }
}

Assert-Path (Join-Path $repo 'components\icc-switch\src\main.cpp')
Assert-Path (Join-Path $repo 'components\icc-switch\src\gui.cpp')
Assert-Path (Join-Path $repo 'components\mux-display-switcher\src\display_manager.cpp')
Assert-Path (Join-Path $repo 'components\mux-display-switcher\src\display_manager.hpp')
Assert-Path (Join-Path $repo 'components\iyx-fast-launcher\FastLauncher.cs')
Assert-Path (Join-Path $repo 'components\iyx-fast-launcher\Payload.zip')
Assert-Path (Join-Path $repo 'components\keyboard-check\键盘检查.exe')

$keyboard = Join-Path $repo 'components\keyboard-check\键盘检查.exe'
$hash = (Get-FileHash -LiteralPath $keyboard -Algorithm SHA256).Hash
if ($hash -ne '2EF56DF4F0A3D5A53FB790794A55F199165B120A0D22BD2E6ADFA9AD516B3517') {
    throw "Keyboard-check hash mismatch: $hash"
}

Assert-Path (Join-Path $dist 'Toolbox.exe')
foreach ($name in @('icc-switch-gui.exe', 'icc-switch-cli.exe', 'MUX.exe', 'MUX-cli.exe', 'IYX.exe', 'keyboard-check.exe')) {
    Assert-Path (Join-Path $dist "tools\$name")
}

$toolboxRun = Start-Process -FilePath (Join-Path $dist 'Toolbox.exe') -ArgumentList '--check' -Wait -PassThru
if ($toolboxRun.ExitCode -ne 0) { throw "Toolbox integrity check failed: $($toolboxRun.ExitCode)" }

& (Join-Path $dist 'tools\icc-switch-cli.exe') '--help'
if ($LASTEXITCODE -ne 0) { throw "ICC CLI help failed: $LASTEXITCODE" }

& (Join-Path $dist 'tools\MUX-cli.exe') 'self-test'
if ($LASTEXITCODE -ne 0) { throw "MUX read-only self-test failed: $LASTEXITCODE" }

& (Join-Path $dist 'tools\IYX.exe') '--verify-patches'
if ($LASTEXITCODE -ne 0) { throw "IYX patch verification failed: $LASTEXITCODE" }

Write-Output 'All toolbox tests passed.'
