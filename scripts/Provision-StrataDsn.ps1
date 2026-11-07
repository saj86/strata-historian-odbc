<#
.SYNOPSIS  Idempotent x64 System DSN provisioning. Secrets are refused by construction: no UID/PWD parameters exist.
.EXAMPLE   .\Provision-StrataDsn.ps1 -Name StrataFixture -Backend fixture
.EXAMPLE   .\Provision-StrataDsn.ps1 -Name StrataPI -Backend piwebapi -BaseUrl https://piweb.example.local/piwebapi -DataServer PISRV01 -Auth Basic
.EXAMPLE   .\Provision-StrataDsn.ps1 -Name StrataPI -Remove
.NOTES     Keep production and fixture DSNs under different names. Leave -NavigatorPreview off on gateway nodes.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Name,
    [ValidateSet('fixture', 'piwebapi')][string]$Backend,
    [string]$BaseUrl,
    [string]$DataServer,
    [ValidateSet('Anonymous', 'Basic', 'Negotiate')][string]$Auth,
    [switch]$NavigatorPreview,
    [ValidateRange(1, 3660)][int]$MaxWindowDays = 31,
    [ValidateRange(1, 150000)][int]$PageSize = 1000,
    [ValidateRange(1, 1000)][int]$MaxPointsPerQuery = 16,
    [ValidateRange(1, 64)][int]$MaxConcurrency = 4,
    [ValidateRange(1000, 3600000)][int]$RequestTimeoutMs = 30000,
    [ValidateRange(0, 10)][int]$MaxRetries = 3,
    [string]$TraceFile,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$driver = 'Strata Historian ODBC Driver'
$dsnKey = "HKLM:\SOFTWARE\ODBC\ODBC.INI\$Name"
$listKey = 'HKLM:\SOFTWARE\ODBC\ODBC.INI\ODBC Data Sources'
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell; 32-bit writes to the WOW6432Node DSN hive, which x64 clients do not read.' }

if ($Remove) {
    if ($PSCmdlet.ShouldProcess($Name, 'Remove System DSN')) {
        Remove-Item -Path $dsnKey -Recurse -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $listKey -Name $Name -ErrorAction SilentlyContinue
    }
    return
}
if (-not $Backend) { throw '-Backend is required; the driver has no default backend.' }
if ($Backend -eq 'piwebapi') {
    if (-not $BaseUrl -or -not $DataServer) { throw 'piwebapi needs -BaseUrl and -DataServer.' }
    if ($BaseUrl -notmatch '^https://' -and $BaseUrl -notmatch '^http://(127\.0\.0\.1|localhost)([:/]|$)') { throw 'BaseUrl must use https://.' }
    if ($BaseUrl -match '@') { throw 'BaseUrl must not contain credentials.' }
}
$driverPath = (Get-ItemProperty "HKLM:\SOFTWARE\ODBC\ODBCINST.INI\$driver" -ErrorAction SilentlyContinue).Driver
if (-not $driverPath) { throw "Driver '$driver' is not registered. Install the MSI first." }

$values = [ordered]@{
    Driver            = $driverPath
    Backend           = $Backend
    NavigatorPreview  = if ($NavigatorPreview) { '1' } else { '0' }
    MaxWindowDays     = "$MaxWindowDays"
    PageSize          = "$PageSize"
    MaxPointsPerQuery = "$MaxPointsPerQuery"
    MaxConcurrency    = "$MaxConcurrency"
    RequestTimeoutMs  = "$RequestTimeoutMs"
    MaxRetries        = "$MaxRetries"
}
if ($BaseUrl)    { $values.BaseUrl = $BaseUrl }
if ($DataServer) { $values.DataServer = $DataServer }
if ($Auth)       { $values.Auth = $Auth }
if ($TraceFile)  { $values.TraceFile = $TraceFile }

if ($PSCmdlet.ShouldProcess($Name, 'Create or update System DSN')) {
    if (-not (Test-Path $listKey)) { New-Item -Path $listKey -Force | Out-Null }
    New-Item -Path $dsnKey -Force | Out-Null
    # Drop anything not in the desired state, including stray UID/PWD from an older manual setup.
    foreach ($p in (Get-Item $dsnKey).Property) { if (-not $values.Contains($p)) { Remove-ItemProperty -Path $dsnKey -Name $p } }
    foreach ($k in $values.Keys) { New-ItemProperty -Path $dsnKey -Name $k -Value $values[$k] -PropertyType String -Force | Out-Null }
    New-ItemProperty -Path $listKey -Name $Name -Value $driver -PropertyType String -Force | Out-Null
}
[pscustomobject]@{ Dsn = $Name; Backend = $Backend; NavigatorPreview = [bool]$NavigatorPreview; Driver = $driverPath }
