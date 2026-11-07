<#
.SYNOPSIS  Clean build, unit tests, MSI build and native ODBC smoke test on Windows x64.
.EXAMPLE   .\scripts\Build-And-Test.ps1 -Version 0.1.0
.NOTES     Needs Visual Studio 2022 C++ tools, Windows SDK, CMake 3.21+. MSI needs: dotnet tool install --global wix --version 5.*
           Installing the MSI and writing System DSNs requires an elevated session.
#>
[CmdletBinding()]
param(
    [string]$Version = '0.1.0',
    [switch]$SkipInstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not [Environment]::Is64BitProcess) { throw 'Run from a 64-bit PowerShell: the ODBC driver manager must match the x64 driver.' }

cmake --preset windows-x64-release
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build --preset windows-x64-release
if ($LASTEXITCODE) { throw 'build failed' }

$bin = Join-Path $root 'build\windows-x64\Release'
& (Join-Path $bin 'core_tests.exe')
if ($LASTEXITCODE) { throw 'core_tests failed' }

$dll = Join-Path $bin 'strataodbc.dll'
$msi = Join-Path $root "build\strataodbc-$Version-x64.msi"
wix build installer\strataodbc.wxs -arch x64 -d "ProductVersion=$Version" -d "DriverPath=$dll" -o $msi
if ($LASTEXITCODE) { throw 'MSI build failed' }
Write-Host "MSI: $msi"

if ($SkipInstall) { return }

& "$PSScriptRoot\Install-StrataDriver.ps1" -Action Install -MsiPath $msi -AllowUnsigned
& "$PSScriptRoot\Provision-StrataDsn.ps1" -Name StrataFixture -Backend fixture
& "$PSScriptRoot\Provision-StrataDsn.ps1" -Name StrataFixturePreview -Backend fixture -NavigatorPreview

$trace = Join-Path $env:TEMP 'strata-smoke-trace.jsonl'
Remove-Item $trace -ErrorAction SilentlyContinue
& (Join-Path $bin 'odbc_smoke.exe') StrataFixture StrataFixturePreview 'DRIVER={Strata Historian ODBC Driver};' (Join-Path $root 'fixtures\expected_pressure_window.csv') $trace
if ($LASTEXITCODE) { throw 'odbc_smoke failed' }
Write-Host 'Build, unit tests and ODBC smoke test passed.'
