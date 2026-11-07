<#
.SYNOPSIS  Silent install, upgrade, repair and uninstall with signature verification. Idempotent.
.EXAMPLE   .\Install-StrataDriver.ps1 -Action Install -MsiPath .\strataodbc-<version>-x64.msi -ExpectedThumbprint AB12...
.NOTES     Gateway nodes: drain first. Upgrading replaces a DLL the mashup containers may hold loaded, so this
           stops the gateway service for the operation unless -NoServiceRestart is given.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][ValidateSet('Install', 'Upgrade', 'Repair', 'Uninstall')][string]$Action,
    [string]$MsiPath,
    [string]$ExpectedThumbprint,
    [switch]$AllowUnsigned,   # lab only; never on a gateway node
    [string]$GatewayService = 'PBIEgwService',
    [switch]$NoServiceRestart,
    [string]$LogDirectory = "$env:ProgramData\StrataHistorianOdbc\logs"
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated.' }
if (-not [Environment]::Is64BitOperatingSystem) { throw 'x64 Windows is required.' }

if ($Action -ne 'Uninstall' -or $MsiPath) {
    if (-not $MsiPath -or -not (Test-Path $MsiPath)) { throw "MSI not found: $MsiPath" }
    $sig = Get-AuthenticodeSignature -FilePath $MsiPath
    if ($sig.Status -ne 'Valid') {
        if (-not $AllowUnsigned) { throw "Signature check failed: $($sig.Status). Refusing to install." }
        Write-Warning "Unsigned package accepted by -AllowUnsigned: $($sig.Status)"
    } elseif ($ExpectedThumbprint -and $sig.SignerCertificate.Thumbprint -ne $ExpectedThumbprint) {
        throw "Signer thumbprint $($sig.SignerCertificate.Thumbprint) does not match the expected one."
    }
}

New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
$log = Join-Path $LogDirectory ("{0}-{1:yyyyMMdd-HHmmss}.log" -f $Action, (Get-Date))
$msiArgs = switch ($Action) {
    'Install'   { "/i `"$MsiPath`" /qn /norestart /l*v `"$log`"" }
    'Upgrade'   { "/i `"$MsiPath`" /qn /norestart /l*v `"$log`"" }
    'Repair'    { "/fa `"$MsiPath`" /qn /norestart /l*v `"$log`"" }
    'Uninstall' { if ($MsiPath) { "/x `"$MsiPath`" /qn /norestart /l*v `"$log`"" } else { throw 'Uninstall needs -MsiPath of the installed version.' } }
}

$svc = Get-Service -Name $GatewayService -ErrorAction SilentlyContinue
$restart = $svc -and $svc.Status -eq 'Running' -and -not $NoServiceRestart
if ($PSCmdlet.ShouldProcess($env:COMPUTERNAME, "$Action Strata Historian ODBC Driver")) {
    if ($restart) { Stop-Service -Name $GatewayService -Force }
    try {
        $p = Start-Process msiexec.exe -ArgumentList $msiArgs -Wait -PassThru
        # 0 success, 3010 reboot required (reported, never performed here), 1605 not installed on uninstall.
        if ($p.ExitCode -notin 0, 3010, 1605) { throw "msiexec exit code $($p.ExitCode). Log: $log" }
        if ($p.ExitCode -eq 3010) { Write-Warning 'A restart is required to finish. This script will not restart the host.' }
    } finally {
        if ($restart) { Start-Service -Name $GatewayService }
    }
}
[pscustomobject]@{ Action = $Action; Computer = $env:COMPUTERNAME; Log = $log; GatewayRestarted = [bool]$restart }
