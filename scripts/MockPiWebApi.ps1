<#
.SYNOPSIS  Loopback mock of the PI Web API routes the driver uses, in plain PowerShell with nothing to install.
.NOTES     SYNTHETIC. This is not PI, says nothing about a real server, and must never be cited as Gate B evidence.
           Field names follow docs/evidence-log.md. Used by Test-DriverWithMock.ps1. Stop it with GET /__stop.
#>
param(
    [Parameter(Mandatory)][int]$Port,
    [switch]$RequireAuth
)
$ErrorActionPreference = 'Stop'
$inv = [cultureinfo]::InvariantCulture

function Item($time, $value, $good = 'true', $questionable = 'false', $substituted = 'false', $annotated = 'false', $units = 'kPa') {
    [pscustomobject]@{
        Time = [datetimeoffset]::Parse($time, $inv)
        Json = "{`"Timestamp`":`"$time`",`"Value`":$value,`"UnitsAbbreviation`":`"$units`",`"Good`":$good,`"Questionable`":$questionable,`"Substituted`":$substituted,`"Annotated`":$annotated}"
    }
}
$pressure = @(
    Item '2026-01-15T08:00:00.0000000Z' '101.5'
    Item '2026-01-15T08:00:10.0000000Z' '0'
    Item '2026-01-15T08:00:10.0000153Z' '102.25'
    Item '2026-01-15T08:00:10.0000153Z' '102.5'
    Item '2026-01-15T08:05:00.0000000Z' '{"Name":"Bad Input","Value":307,"IsSystem":true}' 'false'
    Item '2026-01-15T08:10:00.0000000Z' '103' 'true' 'true'
    Item '2026-01-15T08:15:00.0000000Z' '104' 'true' 'false' 'true' 'true'
    Item '2026-01-15T10:00:00.0000000Z' '105'
)
$run = @(
    Item '2026-01-15T08:00:00.0000000Z' '{"Name":"Running","Value":1,"IsSystem":false}' 'true' 'false' 'false' 'false' ''
    Item '2026-01-15T08:12:00.0000000Z' '{"Name":"Stopped","Value":0,"IsSystem":false}' 'true' 'false' 'false' 'false' ''
)
$points = @(
    '{"WebId":"P1PRESSURE","Name":"MOCK.PRESSURE.PV","Descriptor":"Mock pressure","PointClass":"classic","PointType":"Float64","EngineeringUnits":"kPa","DigitalSetName":"","Step":false}'
    '{"WebId":"P1RUN","Name":"MOCK.RUNSTATE","Descriptor":"Mock run state","PointClass":"classic","PointType":"Digital","EngineeringUnits":"","DigitalSetName":"RunState","Step":true}'
)
$names = @{ P1PRESSURE = 0; P1RUN = 1 }
$hiddenPoint = '{"WebId":"HIDDEN","Name":"MOCK.HIDDEN"}'
$flakyFailed = $false

function Respond($stream, [int]$status, [string]$body, [string]$extraHeaders = '', [int]$declaredLength = -1) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($body)
    if ($declaredLength -lt 0) { $declaredLength = $bytes.Length }
    $head = "HTTP/1.1 $status Mock`r`nContent-Type: application/json`r`nContent-Length: $declaredLength`r`nConnection: close`r`n$extraHeaders`r`n"
    $headBytes = [Text.Encoding]::ASCII.GetBytes($head)
    $stream.Write($headBytes, 0, $headBytes.Length)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
$listener.Start()
try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII)
            $requestLine = $reader.ReadLine()
            $hasAuth = $false
            while ($line = $reader.ReadLine()) { if ($line -match '^Authorization:') { $hasAuth = $true } }
            $target = ($requestLine -split ' ')[1]
            $path, $queryText = $target -split '\?', 2
            $path = [Uri]::UnescapeDataString($path)
            $query = @{}
            if ($queryText) {
                foreach ($pair in $queryText -split '&') {
                    $k, $v = $pair -split '=', 2
                    $query[$k] = [Uri]::UnescapeDataString("$v")
                }
            }
            if ($path -eq '/__stop') { Respond $stream 200 '{}'; break }
            if ($RequireAuth -and -not $hasAuth) {
                Respond $stream 401 '{"Errors":["Authentication required"]}' "WWW-Authenticate: Negotiate`r`nWWW-Authenticate: Basic realm=`"mock`"`r`n"
                continue
            }
            switch -Regex ($path) {
                '^/piwebapi/?$' { Respond $stream 200 '{"Links":{"Self":"mock","DataServers":"mock","System":"mock"}}'; break }
                '^/piwebapi/system$' { Respond $stream 200 '{"ProductTitle":"MOCK PI Web API, not a real server","ProductVersion":"0.0.0"}'; break }
                '^/piwebapi/dataservers$' {
                    if ($query['name']) { Respond $stream 200 "{`"WebId`":`"F1DSMOCK`",`"Name`":`"$($query['name'])`"}" }
                    else { Respond $stream 200 '{"Items":[{"WebId":"F1DSMOCK","Name":"MOCK-SERVER"}]}' }
                    break
                }
                '^/piwebapi/dataservers/F1DSMOCK/points$' {
                    $filter = if ($query['nameFilter']) { $query['nameFilter'] } else { '*' }
                    $matched = @($points | Where-Object { ($_ | ConvertFrom-Json).Name -like $filter })
                    $start = [int]$query['startIndex']
                    $page = @($matched | Select-Object -Skip $start -First ([int]$query['maxCount']))
                    Respond $stream 200 ('{"Items":[' + ($page -join ',') + ']}')
                    break
                }
                '^/piwebapi/points/([^/]+)$' {
                    $id = $Matches[1]
                    if ($id -eq 'P1DENIED') { Respond $stream 403 '{"Errors":["denied"]}' }
                    elseif ($names.ContainsKey($id)) { Respond $stream 200 $points[$names[$id]] }
                    elseif ($id -in 'P1FLAKY', 'P1TRUNC', 'P1SLOW') { Respond $stream 200 ($points[0].Replace('P1PRESSURE', $id)) }
                    else { Respond $stream 404 '{"Errors":["not found"]}' }
                    break
                }
                '^/piwebapi/streams/([^/]+)/recorded$' {
                    $id = $Matches[1]
                    if ($id -eq 'P1FLAKY' -and -not $flakyFailed) {
                        $flakyFailed = $true
                        Respond $stream 503 '{"Errors":["busy"]}' "Retry-After: 0`r`n"
                        break
                    }
                    if ($id -eq 'P1TRUNC') { Respond $stream 200 '{"Items":[{"Timestamp":"2026-' '' 5000; break }
                    if ($id -eq 'P1SLOW') { Start-Sleep -Seconds 4 }
                    $source = if ($id -eq 'P1RUN') { $run } else { $pressure }
                    $from = [datetimeoffset]::Parse($query['startTime'], $inv)
                    $to = [datetimeoffset]::Parse($query['endTime'], $inv)
                    $items = @($source | Where-Object { $_.Time -ge $from -and $_.Time -le $to } | Select-Object -First ([int]$query['maxCount']))
                    Respond $stream 200 ('{"Items":[' + (($items | ForEach-Object Json) -join ',') + ']}')
                    break
                }
                default { Respond $stream 404 '{"Errors":["not found"]}' }
            }
        } catch {
            # A client that went away, for example after a cancel, must not stop the mock.
        } finally {
            $client.Close()
        }
    }
} finally {
    $listener.Stop()
}
