<#
.SYNOPSIS  Test-only install without an MSI: copies the DLL and writes the same ODBC registry entries as installer\strataodbc.wxs.
.NOTES     For test machines. Gateway and production hosts must use the signed MSI (Install-StrataDriver.ps1).
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$DllPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\strataodbc.dll'),
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run from an elevated PowerShell.' }
if (-not [Environment]::Is64BitProcess) { throw 'Run from 64-bit PowerShell; 32-bit writes to the wrong ODBC hive.' }

$driver = 'Strata Historian ODBC Driver'
$folder = Join-Path $env:ProgramFiles 'Strata Historian ODBC'
$target = Join-Path $folder 'strataodbc.dll'
$key = "HKLM:\SOFTWARE\ODBC\ODBCINST.INI\$driver"
$list = 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers'
$dsnList = 'HKLM:\SOFTWARE\ODBC\ODBC.INI\ODBC Data Sources'

if ($Uninstall) {
    if ($PSCmdlet.ShouldProcess($env:COMPUTERNAME, 'Remove driver and fixture DSNs')) {
        foreach ($dsn in 'StrataFixture', 'StrataFixturePreview') {
            Remove-Item "HKLM:\SOFTWARE\ODBC\ODBC.INI\$dsn" -Recurse -ErrorAction SilentlyContinue
            Remove-ItemProperty -Path $dsnList -Name $dsn -ErrorAction SilentlyContinue
        }
        Remove-Item $key -Recurse -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $list -Name $driver -ErrorAction SilentlyContinue
        Remove-Item $folder -Recurse -ErrorAction SilentlyContinue
    }
    Write-Host 'Removed.'
    return
}

if (-not (Test-Path $DllPath)) { throw "Driver DLL not found: $DllPath" }
if ($PSCmdlet.ShouldProcess($target, 'Install driver')) {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    Copy-Item -LiteralPath $DllPath -Destination $target -Force
    Unblock-File -LiteralPath $target
    New-Item -Path $key -Force | Out-Null
    if (-not (Test-Path $list)) { New-Item -Path $list -Force | Out-Null }
    New-ItemProperty -Path $list -Name $driver -Value 'Installed' -PropertyType String -Force | Out-Null
    $values = [ordered]@{ Driver = $target; Setup = $target; APILevel = '1'; ConnectFunctions = 'YYN'; DriverODBCVer = '03.52'; FileUsage = '0'; SQLLevel = '0' }
    foreach ($k in $values.Keys) { New-ItemProperty -Path $key -Name $k -Value $values[$k] -PropertyType String -Force | Out-Null }
    New-ItemProperty -Path $key -Name 'UsageCount' -Value 1 -PropertyType DWord -Force | Out-Null
}
[pscustomobject]@{ Driver = $driver; Path = $target; Sha256 = (Get-FileHash $target -Algorithm SHA256).Hash }
