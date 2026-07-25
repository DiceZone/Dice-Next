# Dice!Next cross-platform build entry point.
# Docker Linux builds use the parent workspace as context because the server
# CMake project depends on the sibling onedice-cpp-lib repository.
param(
    [ValidateSet('windows-amd64', 'windows-arm64', 'all', 'docker-linux')]
    [string]$Platform = 'windows-amd64',
    [switch]$BuildOnly,
    [switch]$PackageOnly,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$ScriptDir = $PSScriptRoot

if ($Help) {
    Write-Host @'
Usage: .\cross-compile.ps1 [-Platform windows-amd64|windows-arm64|all|docker-linux] [-BuildOnly|-PackageOnly]

windows-amd64 / windows-arm64  Build and package Windows releases.
docker-linux                   Build and package Linux AMD64 and ARM64 with Docker.
BuildOnly                      Compile only.
PackageOnly                    Package existing output only.
'@
    exit 0
}

function Invoke-WindowsBuild {
    param([ValidateSet('amd64', 'arm64')] [string]$Architecture)

    if (-not $PackageOnly) {
        & (Join-Path $ScriptDir 'cross-compile\scripts\build-windows.ps1') -Architecture $Architecture
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    if (-not $BuildOnly) {
        & (Join-Path $ScriptDir 'cross-compile\scripts\package-windows.ps1') -Architecture $Architecture
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

if ($Platform -eq 'docker-linux') {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw 'Docker Desktop is required for Linux cross-builds.'
    }

    Push-Location (Join-Path $ScriptDir 'web')
    try { npm run build } finally { Pop-Location }

    $workspaceRoot = Split-Path -Parent $ScriptDir
    if (-not (Test-Path (Join-Path $workspaceRoot 'onedice-cpp-lib'))) {
        throw "Missing sibling dependency: $(Join-Path $workspaceRoot 'onedice-cpp-lib')"
    }
    Push-Location $workspaceRoot
    try {
        docker build -t dice-next:linux-amd64 -f dice-next/cross-compile/docker/Dockerfile.linux-amd64 .
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        docker build -t dice-next:linux-arm64 -f dice-next/cross-compile/docker/Dockerfile.linux-arm64 .
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        docker run --rm -v "${workspaceRoot}:/workspace" dice-next:linux-amd64 bash -c 'cd /workspace/dice-next && cross-compile/scripts/build-linux.sh amd64 && cross-compile/scripts/package-linux.sh amd64'
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        docker run --rm -v "${workspaceRoot}:/workspace" dice-next:linux-arm64 bash -c 'cd /workspace/dice-next && cross-compile/scripts/build-linux.sh arm64 && cross-compile/scripts/package-linux.sh arm64'
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
    exit 0
}

if (-not $env:VCPKG_ROOT) {
    throw 'VCPKG_ROOT is required for Windows builds.'
}

switch ($Platform) {
    'windows-amd64' { Invoke-WindowsBuild -Architecture amd64 }
    'windows-arm64' { Invoke-WindowsBuild -Architecture arm64 }
    'all' {
        Invoke-WindowsBuild -Architecture amd64
        Invoke-WindowsBuild -Architecture arm64
    }
}
