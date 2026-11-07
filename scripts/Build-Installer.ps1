<#
.SYNOPSIS  Builds the MSI with the setup wizard. Needs Windows and the .NET SDK, but not Visual Studio.
.EXAMPLE   .\scripts\Build-Installer.ps1
           From the complete package: uses bin\strataodbc.dll (the cross-compiled test build).
.EXAMPLE   .\scripts\Build-Installer.ps1 -DllPath build\windows-x64\Release\strataodbc.dll -CertificateThumbprint AB12... -TimestampUrl <your timestamp URL>
           Release build: signs the DLL and the MSI with your organization's certificate.
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$DllPath,
    [string]$OutputDirectory,
    [string]$CertificateThumbprint,
    [string]$TimestampUrl
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# One version for everything: read it from the driver's own version header.
$header = Get-Content (Join-Path $root 'src\odbc\version.h') -Raw
$headerVersion = '{0}.{1}.{2}' -f ([regex]::Match($header, 'STRATA_VERSION_MAJOR (\d+)').Groups[1].Value), ([regex]::Match($header, 'STRATA_VERSION_MINOR (\d+)').Groups[1].Value), ([regex]::Match($header, 'STRATA_VERSION_PATCH (\d+)').Groups[1].Value)
if (-not $Version) { $Version = $headerVersion }
if ($Version -ne $headerVersion) { throw "Version $Version doesn't match src\odbc\version.h ($headerVersion). Change both together." }

if (-not $DllPath) {
    $candidates = @((Join-Path $root 'build\windows-x64\Release\strataodbc.dll'), (Join-Path $root 'bin\strataodbc.dll'))
    $DllPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $DllPath) { throw 'No driver DLL found. Build it first, or pass -DllPath.' }
}
$DllPath = (Resolve-Path $DllPath).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'dist' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

# The DLL must be x64 and carry the same version.
$bytes = [IO.File]::ReadAllBytes($DllPath)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw "$DllPath is not an x64 DLL." }
$fileVersion = (Get-Item $DllPath).VersionInfo.FileVersion
if ($fileVersion -and -not $fileVersion.StartsWith("$Version.")) { throw "The DLL is version $fileVersion, but the installer is $Version. Rebuild the DLL." }

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { throw 'The .NET SDK is required: https://dotnet.microsoft.com/download' }
if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    dotnet tool install --global wix --version 5.0.2
    if ($LASTEXITCODE) { throw 'Could not install WiX.' }
    $env:PATH += ";$env:USERPROFILE\.dotnet\tools"
}
wix extension add -g WixToolset.UI.wixext/5.0.2
if ($LASTEXITCODE) { throw 'Could not add the WiX UI extension.' }

# Work on a copy, so signing never changes the input DLL.
$stage = Join-Path ([IO.Path]::GetTempPath()) ("strata-msi-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$dll = Join-Path $stage 'strataodbc.dll'
Copy-Item $DllPath $dll

function Sign([string]$path) {
    if (-not $TimestampUrl) { throw 'Signing needs -TimestampUrl, your organization''s timestamp server.' }
    signtool sign /fd SHA256 /tr $TimestampUrl /td SHA256 /sha1 $CertificateThumbprint $path
    if ($LASTEXITCODE) { throw "Signing failed for $path" }
}
if ($CertificateThumbprint) { Sign $dll }

$msi = Join-Path $OutputDirectory "strataodbc-$Version-x64.msi"
wix build (Join-Path $root 'installer\strataodbc.wxs') -arch x64 -ext WixToolset.UI.wixext `
    -d "ProductVersion=$Version" -d "DriverPath=$dll" -d "SourceRoot=$root" -o $msi
if ($LASTEXITCODE) { throw 'MSI build failed.' }
if ($CertificateThumbprint) { Sign $msi }

$signed = if ($CertificateThumbprint) { 'signed' } else { 'UNSIGNED: for testing only' }
Write-Host "MSI: $msi ($signed)"
Write-Host "SHA256: $((Get-FileHash $msi -Algorithm SHA256).Hash)"
Write-Host "Install with a wizard: double-click it. Silently: msiexec /i `"$msi`" /qn"
