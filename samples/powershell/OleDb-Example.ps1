<#
.SYNOPSIS  Experimental: reads through OLE DB, using Microsoft's built-in OLE DB Provider for ODBC Drivers (MSDASQL).
.NOTES     Run in Windows PowerShell 5.1, which includes System.Data.OleDb. Not yet tested: MSDASQL may need ODBC
           features this driver doesn't have, and any such error is useful to report. For new work, prefer ODBC directly.
.EXAMPLE   powershell -File .\OleDb-Example.ps1 -Dsn StrataFixture
#>
param([string]$Dsn = 'StrataFixture', [string]$TagName = 'SYN.PRESSURE.PV')
$ErrorActionPreference = 'Stop'
$connection = New-Object System.Data.OleDb.OleDbConnection "Provider=MSDASQL;DSN=$Dsn"
$connection.Open()
try {
    Write-Host "Provider: $($connection.Provider), driver version $($connection.ServerVersion)"
    $command = $connection.CreateCommand()
    $command.CommandText = "SELECT EventUtc, NumericValue, StateText, IsGood FROM RecordedValues WHERE TagName = ? AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}"
    [void]$command.Parameters.Add('Tag', [System.Data.OleDb.OleDbType]::VarWChar, 256)
    $command.Parameters[0].Value = $TagName
    $table = New-Object System.Data.DataTable
    $table.Load($command.ExecuteReader())
    $table | Format-Table -AutoSize | Out-Host
    Write-Host "Rows: $($table.Rows.Count) (the fixture should give 7)"
} finally {
    $connection.Dispose()
}
