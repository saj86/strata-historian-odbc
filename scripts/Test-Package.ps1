<#
.SYNOPSIS  One-command test of the unsigned Windows test package: install, create fixture DSNs, run both test programs.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Get-ChildItem -Path $root -Recurse -File | Unblock-File

Write-Host '--- file hashes'
foreach ($line in Get-Content (Join-Path $root 'SHA256SUMS.txt')) {
    $hash, $file = $line -split '\s+', 2
    $actual = (Get-FileHash (Join-Path $root $file) -Algorithm SHA256).Hash
    if ($actual -ne $hash.ToUpper()) { throw "Hash mismatch: $file" }
}

Write-Host '--- install'
& "$PSScriptRoot\Install-Portable.ps1"
& "$PSScriptRoot\Provision-StrataDsn.ps1" -Name StrataFixture -Backend fixture
& "$PSScriptRoot\Provision-StrataDsn.ps1" -Name StrataFixturePreview -Backend fixture -NavigatorPreview

Write-Host '--- core_tests'
& (Join-Path $root 'bin\core_tests.exe')
if ($LASTEXITCODE) { throw "core_tests failed with exit code $LASTEXITCODE" }

Write-Host '--- odbc_smoke (through the Windows ODBC Driver Manager)'
$trace = Join-Path $env:TEMP 'strata-smoke-trace.jsonl'
Remove-Item $trace -ErrorAction SilentlyContinue
& (Join-Path $root 'bin\odbc_smoke.exe') StrataFixture StrataFixturePreview 'DRIVER={Strata Historian ODBC Driver};' (Join-Path $root 'fixtures\expected_pressure_window.csv') $trace
if ($LASTEXITCODE) { throw "odbc_smoke failed with exit code $LASTEXITCODE" }

Write-Host '--- PI Web API path against a local mock (not a real PI server)'
& "$PSScriptRoot\Test-DriverWithMock.ps1"
if ($LASTEXITCODE) { throw "Test-DriverWithMock failed with exit code $LASTEXITCODE" }

Write-Host 'ALL PASSED'
