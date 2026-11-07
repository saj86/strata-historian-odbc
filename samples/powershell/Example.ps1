<#
.SYNOPSIS  End-to-end example against the fixture DSN: list points, read a window, handle refusals, export CSV.
.EXAMPLE   .\Example.ps1
.EXAMPLE   .\Example.ps1 -Dsn StrataPI -PointId <point id from Points> -Start 2026-01-15T00:00:00Z -End 2026-01-16T00:00:00Z -Credential (Get-Credential)
#>
[CmdletBinding()]
param(
    [string]$Dsn = 'StrataFixture',
    [string]$PointId = 'PT-PRESSURE-001',
    [string]$Start = '2026-01-15T08:00:00Z',
    [string]$End = '2026-01-15T09:00:00Z',
    [pscredential]$Credential,
    [string]$CsvPath = (Join-Path ([IO.Path]::GetTempPath()) 'strata-recorded.csv')
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Strata.psm1') -Force

$from = [datetimeoffset]::Parse($Start, [cultureinfo]::InvariantCulture).UtcDateTime
$to = [datetimeoffset]::Parse($End, [cultureinfo]::InvariantCulture).UtcDateTime

$connection = New-StrataConnection -Dsn $Dsn -Credential $Credential
try {
    Write-Host "`n--- Points visible to this identity"
    Get-StrataPoint -Connection $connection | Format-Table PointId, TagName, PointType, EngUnits, SourceBackend -AutoSize | Out-Host

    Write-Host '--- Recorded values'
    $rows = @(Get-StrataRecordedValue -Connection $connection -PointId $PointId -Start $from -End $to)
    $rows | Format-Table EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, IsGood -AutoSize | Out-Host
    Write-Host "Rows: $($rows.Count)"

    $rows | Export-Csv -Path $CsvPath -NoTypeInformation -Encoding UTF8
    Write-Host "CSV:  $CsvPath"

    Write-Host "`n--- An unbounded read is refused before reaching the source"
    $command = $connection.CreateCommand()
    $command.CommandText = 'SELECT * FROM RecordedValues'
    try {
        $command.ExecuteReader().Dispose()
        Write-Host 'Unexpected: the read was allowed.'
    } catch [System.Management.Automation.MethodInvocationException] {
        $odbc = $_.Exception.InnerException
        Write-Host "SQLSTATE $($odbc.Errors[0].SQLState): $($odbc.Errors[0].Message)"
    } finally { $command.Dispose() }
} finally {
    $connection.Dispose()
}
