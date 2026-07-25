# Windows build helper for AMD64 and ARM64.
param(
    [ValidateSet('amd64', 'arm64')]
    [string]$Architecture = 'amd64'
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$serverDir = Join-Path $projectRoot 'server'

if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is required.' }

$triplet = 'x64-windows-dynamic'
$buildDir = 'build'
if ($Architecture -eq 'arm64') {
    $triplet = 'arm64-windows-dynamic'
    $buildDir = 'build-arm64'
}

& (Join-Path $env:VCPKG_ROOT 'vcpkg.exe') install `
    'drogon[sqlite3]' nlohmann-json sqlite-orm spdlog yaml-cpp zstd quickjs-ng lua `
    "--triplet=$triplet"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Push-Location $serverDir
try {
    $cmakeArgs = @('-B', $buildDir, '-S', '.', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake", "-DVCPKG_TARGET_TRIPLET=$triplet")
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & cmake --build $buildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
