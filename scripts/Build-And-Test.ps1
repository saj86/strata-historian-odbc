<#
.SYNOPSIS  Clean build, unit tests, MSI build and native ODBC smoke test on Windows x64.
.EXAMPLE   .\scripts\Build-And-Test.ps1
.NOTES     Needs Visual Studio 2022 C++ tools, Windows SDK, CMake 3.21+ and the .NET SDK (for WiX).
           Installing the MSI and writing System DSNs requires an elevated session.
#>
[CmdletBinding()]
param(
    [string]$Version,  # defaults to src\odbc\version.h
    [switch]$SkipInstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $Version) {
    $header = Get-Content (Join-Path $root 'src\odbc\version.h') -Raw
    $Version = (('MAJOR', 'MINOR', 'PATCH') | ForEach-Object { [regex]::Match($header, "STRATA_VERSION_$_ (\d+)").Groups[1].Value }) -join '.'
}
if (-not [Environment]::Is64BitProcess) { throw 'Run from a 64-bit PowerShell: the ODBC driver manager must match the x64 driver.' }

cmake --preset windows-x64-release
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build --preset windows-x64-release
if ($LASTEXITCODE) { throw 'build failed' }

$bin = Join-Path $root 'build\windows-x64\Release'
& (Join-Path $bin 'core_tests.exe')
if ($LASTEXITCODE) { throw 'core_tests failed' }

& "$PSScriptRoot\Build-Installer.ps1" -Version $Version -DllPath (Join-Path $bin 'strataodbc.dll')
$msi = Join-Path $root "dist\strataodbc-$Version-x64.msi"

if ($SkipInstall) { return }

& "$PSScriptRoot\Install-StrataDriver.ps1" -Action Install -MsiPath $msi -AllowUnsigned
& "$PSScriptRoot\Provision-StrataDsn.ps1" -Name StrataFixture -Backend fixture
& "$PSScriptRoot\Provision-StrataDsn.ps1" -Name StrataFixturePreview -Backend fixture -NavigatorPreview

$trace = Join-Path $env:TEMP 'strata-smoke-trace.jsonl'
Remove-Item $trace -ErrorAction SilentlyContinue
& (Join-Path $bin 'odbc_smoke.exe') StrataFixture StrataFixturePreview 'DRIVER={Strata Historian ODBC Driver};' (Join-Path $root 'fixtures\expected_pressure_window.csv') $trace
if ($LASTEXITCODE) { throw 'odbc_smoke failed' }

& "$PSScriptRoot\Test-DriverWithMock.ps1"
if ($LASTEXITCODE) { throw 'Test-DriverWithMock failed' }
Write-Host 'Build, unit tests, ODBC smoke test and mock PI Web API test passed.'
