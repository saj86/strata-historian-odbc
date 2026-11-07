<#
.SYNOPSIS  Runs the driver's PI Web API path end to end against the loopback mock: HTTP, paging, retries, errors, timeouts.
.NOTES     No PI server and nothing to install beyond the driver. Proves the driver's HTTP path works, and says nothing about real PI.
           Exit code 0 only if every check passes.
#>
[CmdletBinding()]
param([string]$Driver = 'Strata Historian ODBC Driver')
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Desktop') { Add-Type -AssemblyName System.Data }

$probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$probe.Start(); $port = $probe.LocalEndpoint.Port; $probe.Stop()
$exe = if ($PSVersionTable.PSEdition -eq 'Desktop') { Join-Path $PSHOME 'powershell.exe' } else { (Get-Command pwsh -CommandType Application | Select-Object -First 1).Source }
$mock = Start-Process -FilePath $exe -NoNewWindow -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'MockPiWebApi.ps1'), '-Port', "$port")

$script:failures = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Host "  PASS $name" } else { $script:failures++; Write-Host "  FAIL $name $detail" }
}
function Stop-Mock {
    try {
        $stop = [Net.Sockets.TcpClient]::new('127.0.0.1', $port)
        $w = [IO.StreamWriter]::new($stop.GetStream()); $w.Write("GET /__stop HTTP/1.1`r`nHost: x`r`n`r`n"); $w.Flush()
        $null = [IO.StreamReader]::new($stop.GetStream()).ReadToEnd(); $stop.Dispose()
    } catch { }
}
function Connect([string]$extra = '') {
    $cs = "DRIVER={$Driver};Backend=piwebapi;BaseUrl=http://127.0.0.1:$port/piwebapi;DataServer=MOCK-SERVER;Auth=Anonymous;MaxRetries=1;$extra"
    $c = [System.Data.Odbc.OdbcConnection]::new($cs)
    $c.Open()
    $c
}
function Rows($connection, [string]$sql, [int]$timeout = 30) {
    $cmd = $connection.CreateCommand()
    $cmd.CommandText = $sql
    $cmd.CommandTimeout = $timeout
    $reader = $cmd.ExecuteReader()
    try {
        while ($reader.Read()) {
            $row = [ordered]@{}
            for ($i = 0; $i -lt $reader.FieldCount; $i++) { $row[$reader.GetName($i)] = if ($reader.IsDBNull($i)) { $null } else { $reader.GetValue($i) } }
            [pscustomobject]$row
        }
    } finally { $reader.Dispose(); $cmd.Dispose() }
}
function StateOf([scriptblock]$action) {
    try { & $action | Out-Null; 'no error' }
    catch {
        $e = $_.Exception
        while ($e -and $e -isnot [System.Data.Odbc.OdbcException]) { $e = $e.InnerException }
        if ($e) { $e.Errors[0].SQLState } else { "other: $($_.Exception.Message)" }
    }
}
function Window([string]$by, [string]$from = '08:00:00', [string]$to = '09:00:00') {
    "SELECT EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, DigitalCode, IsGood, SourceBackend, PointId FROM RecordedValues WHERE $by AND EventUtc >= {ts '2026-01-15 $from'} AND EventUtc < {ts '2026-01-15 $to'}"
}

try {
    $ready = $false
    foreach ($i in 1..100) {
        $t = [Net.Sockets.TcpClient]::new()
        try { $t.Connect('127.0.0.1', $port); $ready = $true } catch { Start-Sleep -Milliseconds 100 } finally { $t.Dispose() }
        if ($ready) { break }
    }
    if (-not $ready) { throw 'The mock server did not start.' }

    Write-Host '--- driver to mock PI Web API over real HTTP'
    $c = Connect
    $points = @(Rows $c 'SELECT PointId, TagName, EngUnits, DigitalSetName, IsStep FROM Points')
    Check 'lists points' ($points.Count -eq 2)
    Check 'empty digital set becomes NULL' ($null -eq $points[0].DigitalSetName)
    Check 'exact tag filter' (@(Rows $c "SELECT PointId FROM Points WHERE TagName = 'MOCK.RUNSTATE'").Count -eq 1)

    $byTag = @(Rows $c (Window "TagName = 'mock.pressure.pv'"))
    Check 'reads a window by tag, any case' ($byTag.Count -eq 7) "rows=$($byTag.Count)"
    Check 'rows are labelled piwebapi' ($byTag[0].SourceBackend -eq 'piwebapi')
    Check 'tag resolves to the WebId' ($byTag[0].PointId -eq 'P1PRESSURE')
    Check 'a real zero stays 0' ($byTag[1].NumericValue -eq 0 -and $null -ne $byTag[1].NumericValue)
    Check 'a bad state has no number' ($null -eq $byTag[4].NumericValue -and $byTag[4].StateText -eq 'Bad Input' -and $byTag[4].ValueKind -eq 'SYSTEM' -and -not $byTag[4].IsGood)
    Check 'repeated timestamps stay apart' ($byTag[2].EventUtcRaw -eq $byTag[3].EventUtcRaw -and $byTag[3].EventSeq -eq 1 -and $byTag[3].NumericValue -eq 102.5)
    Check 'reads by PointId too' (@(Rows $c (Window "PointId = 'P1PRESSURE'")).Count -eq 7)
    Check 'end bound is exclusive' (@(Rows $c (Window "PointId = 'P1PRESSURE'" '08:00:00' '08:00:10')).Count -eq 1)
    Check 'empty window is success with no rows' (@(Rows $c (Window "PointId = 'P1PRESSURE'" '09:00:00' '10:00:00')).Count -eq 0)

    $digital = @(Rows $c (Window "TagName = 'MOCK.RUNSTATE'"))
    Check 'digital state' ($digital.Count -eq 2 -and $digital[0].ValueKind -eq 'DIGITAL' -and $digital[0].StateText -eq 'Running' -and $digital[0].DigitalCode -eq 1)

    Check 'denied point stays denied' ((StateOf { Rows $c (Window "PointId = 'P1DENIED'") }) -eq '42000')
    Check 'unknown point is not found' ((StateOf { Rows $c (Window "PointId = 'P1NOPE'") }) -eq '42S02')
    Check 'unknown tag is not found' ((StateOf { Rows $c (Window "TagName = 'MOCK.NOPE'") }) -eq '42S02')
    Check 'unbounded read is refused, not sent' ((StateOf { Rows $c 'SELECT * FROM RecordedValues' }) -eq '42000')
    Check 'retries a 503 and honors Retry-After' (@(Rows $c (Window "PointId = 'P1FLAKY'")).Count -eq 7)
    Check 'truncated response is an error' ((StateOf { Rows $c (Window "PointId = 'P1TRUNC'") }) -eq '08S01')
    $timedOut = StateOf { Rows $c (Window "PointId = 'P1SLOW'") 1 }
    Check 'query timeout interrupts a stalled read' ($timedOut -eq 'HYT00') "got $timedOut"
    Start-Sleep -Seconds 4
    $c.Dispose()

    Write-Host '--- paging'
    foreach ($size in 3, 4, 7, 1000) {
        $p = Connect "PageSize=$size;"
        $paged = @(Rows $p (Window "PointId = 'P1PRESSURE'"))
        $fingerprint = { param($rows) ($rows | ForEach-Object { "$($_.EventUtcRaw)/$($_.EventSeq)/$($_.NumericValue)" }) -join ',' }
        Check "PageSize=$size returns the same rows" ((& $fingerprint $paged) -eq (& $fingerprint $byTag))
        $p.Dispose()
    }
    # Two events share a timestamp, so a page of 2 can fill with them. The driver must say so, not loop or drop one.
    $tiny = Connect 'PageSize=2;'
    Check 'PageSize=2 reports it cannot progress past a repeated timestamp' ((StateOf { Rows $tiny (Window "PointId = 'P1PRESSURE'") }) -eq 'HY000')
    $tiny.Dispose()

    Write-Host '--- connection loss'
    $d = Connect
    Stop-Mock
    Start-Sleep -Milliseconds 500
    Check 'a lost server is an error, never an empty result' ((StateOf { Rows $d (Window "PointId = 'P1PRESSURE'") }) -eq '08S01')
    $d.Dispose()
    Check 'connecting to a dead server fails' ((StateOf { Connect }) -eq '08S01')
} finally {
    Stop-Mock
    if (-not $mock.WaitForExit(3000)) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
}
if ($script:failures) { Write-Host "Test-DriverWithMock: $script:failures FAILED"; exit 1 }
Write-Host 'Test-DriverWithMock: all passed'
