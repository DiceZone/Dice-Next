# Windows package entry point. Keep this wrapper so cross-compile.ps1 and
# direct callers always use the same complete Beta package definition.
param(
    [ValidateSet('amd64', 'arm64')]
    [string]$Architecture = 'amd64'
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$canonicalPackage = Join-Path $projectRoot 'package.ps1'

if (-not (Test-Path $canonicalPackage)) {
    throw "Canonical package script not found: $canonicalPackage"
}

& $canonicalPackage -Architecture $Architecture
exit $LASTEXITCODE
