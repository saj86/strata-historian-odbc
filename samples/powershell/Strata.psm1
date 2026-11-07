# Small helpers around System.Data.Odbc for the Strata Historian ODBC Driver. Windows PowerShell 5.1 or PowerShell 7, 64-bit.
function New-StrataConnection {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Dsn,
        [pscredential]$Credential   # Only needed for Auth=Basic. Never store a password in the DSN or the script.
    )
    if ($PSVersionTable.PSEdition -eq 'Desktop') { Add-Type -AssemblyName System.Data }
    $cs = "DSN=$Dsn"
    if ($Credential) {
        $password = $Credential.GetNetworkCredential().Password.Replace('}', '}}')
        $cs += ";UID=$($Credential.UserName);PWD={$password}"
    }
    $connection = [System.Data.Odbc.OdbcConnection]::new($cs)
    $connection.Open()
    $connection
}

function Get-StrataPoint {
    [CmdletBinding()]
    param([Parameter(Mandatory)][System.Data.Odbc.OdbcConnection]$Connection, [string]$TagName)
    $command = $Connection.CreateCommand()
    try {
        $command.CommandText = 'SELECT PointId, TagName, Descriptor, PointType, EngUnits, IsStep, SourceBackend FROM Points'
        if ($TagName) {
            $command.CommandText += ' WHERE TagName = ?'
            [void]$command.Parameters.Add('TagName', [System.Data.Odbc.OdbcType]::NVarChar, 256)
            $command.Parameters[0].Value = $TagName
        }
        Read-StrataRows $command
    } finally { $command.Dispose() }
}

function Get-StrataRecordedValue {
    <#
    .SYNOPSIS Reads recorded events for one point, by -TagName or -PointId. Start is inclusive, End is exclusive, both UTC.
              Windows longer than -ChunkHours are split, so MaxWindowDays is never exceeded.
    #>
    [CmdletBinding(DefaultParameterSetName = 'ByTag')]
    param(
        [Parameter(Mandatory)][System.Data.Odbc.OdbcConnection]$Connection,
        [Parameter(Mandatory, ParameterSetName = 'ByTag')][string]$TagName,
        [Parameter(Mandatory, ParameterSetName = 'ById')][string]$PointId,
        [Parameter(Mandatory)][datetime]$Start,
        [Parameter(Mandatory)][datetime]$End,
        [ValidateRange(1, 744)][int]$ChunkHours = 24,
        [int]$TimeoutSeconds = 300
    )
    $sql = 'SELECT PointId, TagName, EventUtc, EventUtcRaw, EventSeq, ValueKind, NumericValue, TextValue, DigitalCode, StateText, EngUnits, IsGood, IsQuestionable, IsSubstituted, IsAnnotated, SourceBackend ' +
           "FROM RecordedValues WHERE $(if ($TagName) { 'TagName' } else { 'PointId' }) = ? AND EventUtc >= ? AND EventUtc < ?"
    $from = $Start
    while ($from -lt $End) {
        $to = $from.AddHours($ChunkHours)
        if ($to -gt $End) { $to = $End }
        $command = $Connection.CreateCommand()
        try {
            $command.CommandText = $sql
            $command.CommandTimeout = $TimeoutSeconds
            $p1 = $command.Parameters.Add('Point', [System.Data.Odbc.OdbcType]::NVarChar, 256); $p1.Value = if ($TagName) { $TagName } else { $PointId }
            $p2 = $command.Parameters.Add('From', [System.Data.Odbc.OdbcType]::DateTime); $p2.Value = $from
            $p3 = $command.Parameters.Add('To', [System.Data.Odbc.OdbcType]::DateTime); $p3.Value = $to
            Read-StrataRows $command
        } finally { $command.Dispose() }
        $from = $to
    }
}

function Read-StrataRows([System.Data.Odbc.OdbcCommand]$Command) {
    $reader = $Command.ExecuteReader()
    try {
        while ($reader.Read()) {
            $row = [ordered]@{}
            for ($i = 0; $i -lt $reader.FieldCount; $i++) {
                # NULL stays $null. A missing or bad reading never becomes 0.
                $row[$reader.GetName($i)] = if ($reader.IsDBNull($i)) { $null } else { $reader.GetValue($i) }
            }
            [pscustomobject]$row
        }
    } finally { $reader.Dispose() }
}

Export-ModuleMember -Function New-StrataConnection, Get-StrataPoint, Get-StrataRecordedValue
