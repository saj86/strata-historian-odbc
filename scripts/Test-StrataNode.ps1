<#
.SYNOPSIS  Per-node health check for a workstation or gateway member. Emits one JSON record and exits non-zero on failure.
           Compare records across cluster members to detect drift (-Baseline).
.PARAMETER SmokeSql  Bounded query run through the DSN. It must name a point and a time window.
.EXAMPLE   .\Test-StrataNode.ps1 -Dsn StrataFixture -SmokeSql "SELECT EventUtc FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}" -ExpectedRows 7
.NOTES     Credentials are read from a PSCredential and never written to the output.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Dsn,
    [string]$SmokeSql,
    [int]$ExpectedRows = -1,
    [pscredential]$Credential,
    [string]$ExpectedVersion,
    [string]$ExpectedThumbprint,
    [string]$Baseline
)
$ErrorActionPreference = 'Stop'
$result = [ordered]@{ computer = $env:COMPUTERNAME; timeUtc = (Get-Date).ToUniversalTime().ToString('o'); dsn = $Dsn; checks = [ordered]@{} }
function Add-Check($name, $ok, $detail) { $result.checks[$name] = [ordered]@{ ok = [bool]$ok; detail = "$detail" } }

$driverKey = 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\Strata Historian ODBC Driver'
$driverPath = (Get-ItemProperty $driverKey -ErrorAction SilentlyContinue).Driver
Add-Check 'driverRegistered' ([bool]$driverPath) $driverPath
if ($driverPath -and (Test-Path $driverPath)) {
    $item = Get-Item $driverPath
    $result.driverFileVersion = $item.VersionInfo.FileVersion
    $result.driverSha256 = (Get-FileHash $driverPath -Algorithm SHA256).Hash
    Add-Check 'driverPresent' $true $driverPath
    if ($ExpectedVersion) { Add-Check 'driverVersion' ($item.VersionInfo.FileVersion -eq $ExpectedVersion) $item.VersionInfo.FileVersion }
    $sig = Get-AuthenticodeSignature $driverPath
    $result.driverSignature = "$($sig.Status)"
    if ($ExpectedThumbprint) { Add-Check 'driverSigner' ($sig.Status -eq 'Valid' -and $sig.SignerCertificate.Thumbprint -eq $ExpectedThumbprint) $sig.Status }
} else {
    Add-Check 'driverPresent' $false 'driver file missing'
}

$dsnKey = "HKLM:\SOFTWARE\ODBC\ODBC.INI\$Dsn"
$dsn = Get-ItemProperty $dsnKey -ErrorAction SilentlyContinue
Add-Check 'dsnResolves' ([bool]$dsn) $dsnKey
if ($dsn) {
    Add-Check 'noSecretsInDsn' (-not ($dsn.PSObject.Properties.Name | Where-Object { $_ -in 'UID', 'PWD', 'Password', 'User' })) 'UID/PWD must not be stored in the DSN'
    $result.backend = $dsn.Backend
    $result.dsnSettings = ($dsn.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ';'
    if ($dsn.BaseUrl) {
        $uri = [Uri]$dsn.BaseUrl
        $port = if ($uri.IsDefaultPort) { if ($uri.Scheme -eq 'https') { 443 } else { 80 } } else { $uri.Port }
        $tcp = New-Object Net.Sockets.TcpClient
        try {
            $tcp.Connect($uri.Host, $port)
            Add-Check 'network' $true "$($uri.Host):$port"
            if ($uri.Scheme -eq 'https') {
                # Validation callback left null: the OS default chain and name checks apply.
                $ssl = New-Object Net.Security.SslStream($tcp.GetStream(), $false)
                try {
                    $ssl.AuthenticateAsClient($uri.Host)
                    $cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2($ssl.RemoteCertificate)
                    Add-Check 'certificateChain' $true "subject=$($cert.Subject) expires=$($cert.NotAfter.ToUniversalTime().ToString('o')) tls=$($ssl.SslProtocol)"
                    $result.certificateThumbprint = $cert.Thumbprint
                } catch { Add-Check 'certificateChain' $false $_.Exception.Message }
            }
        } catch { Add-Check 'network' $false $_.Exception.Message } finally { $tcp.Dispose() }
    }
}

if ($SmokeSql) {
    if ($SmokeSql -notmatch '(?i)\bPointId\b' -and $SmokeSql -notmatch '(?i)\bPoints\b') { throw 'SmokeSql must be bounded.' }
    Add-Type -AssemblyName System.Data
    $cs = "DSN=$Dsn"
    if ($Credential) { $cs += ";UID=$($Credential.UserName);PWD={$($Credential.GetNetworkCredential().Password.Replace('}', '}}'))}" }
    $con = New-Object System.Data.Odbc.OdbcConnection($cs)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $con.Open()
        $cmd = $con.CreateCommand(); $cmd.CommandText = $SmokeSql; $cmd.CommandTimeout = 60
        $r = $cmd.ExecuteReader(); $n = 0
        while ($r.Read()) { $n++ }
        $r.Dispose()
        $ok = ($ExpectedRows -lt 0) -or ($n -eq $ExpectedRows)
        Add-Check 'boundedQuery' $ok "rows=$n expected=$ExpectedRows ms=$($sw.ElapsedMilliseconds) driverVer=$($con.ServerVersion)"
        $result.smokeRows = $n
    } catch { Add-Check 'boundedQuery' $false $_.Exception.Message } finally { $con.Dispose() }
}

$result.healthy = -not ($result.checks.Values | Where-Object { -not $_.ok })
if ($Baseline) {
    $base = Get-Content $Baseline -Raw | ConvertFrom-Json
    $drift = @()
    foreach ($f in 'driverFileVersion', 'driverSha256', 'dsnSettings', 'backend', 'certificateThumbprint') {
        if ("$($base.$f)" -ne "$($result[$f])") { $drift += $f }
    }
    $result.driftFrom = $base.computer
    $result.drift = $drift
    if ($drift) { $result.healthy = $false }
}
$result | ConvertTo-Json -Depth 5
if (-not $result.healthy) { exit 1 }
