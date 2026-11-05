# Builds the C++ core (Release) and the WPF GUI (Release), then copies the
# freshly built SyncControlCore.exe next to the GUI executable, matching the
# deployed layout (core beside GUI).
#
# Usage: powershell -ExecutionPolicy Bypass -File build-all.ps1 [-Configuration Release]

param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$coreBuild = Join-Path $repoRoot "core\build"
$guiProject = Join-Path $repoRoot "gui\SyncControl.GUI\SyncControl.GUI.csproj"
$guiOutput = Join-Path $repoRoot "gui\SyncControl.GUI\bin\$Configuration\net8.0-windows"
$coreExe = Join-Path $coreBuild "Release\SyncControlCore.exe"

Write-Host "== [1/3] Building core ($Configuration) ==" -ForegroundColor Cyan
cmake --build $coreBuild --config $Configuration
if ($LASTEXITCODE -ne 0) {
    Write-Error "core build failed"
    exit 1
}

Write-Host "== [2/3] Building GUI ($Configuration) ==" -ForegroundColor Cyan
dotnet build $guiProject -c $Configuration --nologo
if ($LASTEXITCODE -ne 0) {
    Write-Error "GUI build failed"
    exit 1
}

Write-Host "== [3/3] Copying core into GUI output ==" -ForegroundColor Cyan
if (-not (Test-Path $coreExe)) {
    Write-Error "core executable not found at $coreExe"
    exit 1
}
Copy-Item $coreExe $guiOutput -Force
Write-Host "Copied $coreExe -> $guiOutput"

Write-Host "Done. GUI output: $guiOutput" -ForegroundColor Green
