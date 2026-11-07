<#
.SYNOPSIS  Read-only check of a PI Web API server: is it there, which version, how does it sign in, and does it match what the driver expects.
.DESCRIPTION
    Sends GET requests only, so it changes nothing. Certificate checks stay on. Prints server, version and field names, but no point values.
    Get approval from your PI administrator before pointing it at a production server.
.EXAMPLE   .\Test-PiWebApi.ps1 -BaseUrl https://piweb.example.local/piwebapi
           Without a login: is it reachable, and which sign-in methods does it offer?
.EXAMPLE   .\Test-PiWebApi.ps1 -BaseUrl https://piweb.example.local/piwebapi -UseWindowsAuth
.EXAMPLE   .\Test-PiWebApi.ps1 -BaseUrl https://piweb.example.local/piwebapi -Credential (Get-Credential) -DataServer PISRV01 -TagName SOME.TAG
           Also reads up to 5 events of one tag from the last 7 days and compares the response fields with the driver's expectations.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BaseUrl,
    [pscredential]$Credential,
    [switch]$UseWindowsAuth,
    [string]$DataServer,
    [string]$TagName,
    [datetime]$Start = (Get-Date).ToUniversalTime().AddDays(-7),
    [datetime]$End = (Get-Date).ToUniversalTime()
)
$ErrorActionPreference = 'Stop'
if ($Credential -and $UseWindowsAuth) { throw 'Use -Credential or -UseWindowsAuth, not both.' }
if ($BaseUrl -notmatch '^https://' -and $BaseUrl -notmatch '^http://(127\.0\.0\.1|localhost)([:/]|$)') { throw 'BaseUrl must use https://.' }
$BaseUrl = $BaseUrl.TrimEnd('/')
if ($PSVersionTable.PSEdition -eq 'Desktop') { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 }

function Get-Api([string]$path, [bool]$signIn = $true) {
    $request = [Net.HttpWebRequest][Net.WebRequest]::Create($BaseUrl + $path)
    $request.Method = 'GET'
    $request.Accept = 'application/json'
    $request.Timeout = 20000
    if ($signIn -and $UseWindowsAuth) { $request.UseDefaultCredentials = $true }
    if ($signIn -and $Credential) {
        $pair = $Credential.UserName + ':' + $Credential.GetNetworkCredential().Password
        $request.Headers['Authorization'] = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pair))
    }
    $response = $null
    try {
        try { $response = $request.GetResponse() }
        catch [Net.WebException] {
            if (-not $_.Exception.Response) { return [pscustomobject]@{ Status = 0; Error = $_.Exception.Message; Schemes = @(); Body = $null } }
            $response = $_.Exception.Response
        }
        catch {
            $inner = $_.Exception
            while ($inner -and $inner -isnot [Net.WebException]) { $inner = $inner.InnerException }
            if (-not $inner -or -not $inner.Response) { return [pscustomobject]@{ Status = 0; Error = $_.Exception.Message; Schemes = @(); Body = $null } }
            $response = $inner.Response
        }
        $reader = [IO.StreamReader]::new($response.GetResponseStream())
        $text = $reader.ReadToEnd(); $reader.Dispose()
        # .NET can merge several WWW-Authenticate headers into one comma-separated value, and a scheme can carry
        # parameters (Basic realm="x"), so take the first word of each part and skip parts that are only parameters.
        $schemes = @($response.Headers.GetValues('WWW-Authenticate') | ForEach-Object { $_ -split ',' } | ForEach-Object { (($_.Trim()) -split ' ')[0] } | Where-Object { $_ -and $_ -notmatch '=' })
        $body = $null
        try { $body = $text | ConvertFrom-Json } catch { }
        [pscustomobject]@{ Status = [int]$response.StatusCode; Error = $null; Schemes = $schemes; Body = $body }
    } finally { if ($response) { $response.Dispose() } }
}
function Say([string]$label, [string]$value) { Write-Host ('{0,-24} {1}' -f $label, $value) }

Write-Host "Checking $BaseUrl (GET requests only)`n"

$anonymous = Get-Api '/' $false
if ($anonymous.Status -eq 0) {
    Say 'Reachable' "NO: $($anonymous.Error)"
    Write-Host "`nNext: check the address, the network path and the certificate chain from this machine. A certificate error must be fixed on this machine; the driver can't skip it either."
    exit 2
}
if ($anonymous.Status -eq 404) {
    Say 'Reachable' 'A web server answered, but with HTTP 404. PI Web API may not be at this address.'
    exit 2
}
Say 'Reachable' "yes (HTTP $($anonymous.Status) without signing in)"
if ($anonymous.Schemes.Count) {
    Say 'Sign-in offered' (($anonymous.Schemes | Select-Object -Unique) -join ', ')
    $offered = $anonymous.Schemes
    if ($offered -contains 'Basic') { Say 'Fabric Pipeline Copy' 'possible: Basic is offered' }
    else { Say 'Fabric Pipeline Copy' 'unlikely: no Basic. Plan on Dataflow Gen2, which also supports Windows' }
    if (($offered -contains 'Bearer') -and -not ($offered -contains 'Basic' -or $offered -contains 'Negotiate' -or $offered -contains 'NTLM')) { Say 'Driver support' 'NOT YET: only Bearer (OIDC) is offered, and the driver has no OIDC support' }
} elseif ($anonymous.Status -eq 200) {
    Say 'Sign-in offered' 'none required for this address'
}
if (-not $Credential -and -not $UseWindowsAuth) {
    Write-Host "`nRun again with -UseWindowsAuth or -Credential (Get-Credential) to see the version and data servers."
    exit 0
}

Write-Host ''
$system = Get-Api '/system'
if ($system.Status -eq 200 -and $system.Body) { Say 'Product' "$($system.Body.ProductTitle) $($system.Body.ProductVersion)" }
elseif ($system.Status -in 401, 403) { Say 'Signed in' "NO: HTTP $($system.Status). This identity was not accepted."; exit 3 }
else { Say 'Product' "not read (HTTP $($system.Status))" }

$servers = Get-Api '/dataservers?selectedFields=Items.Name;Items.WebId'
if ($servers.Status -eq 200) {
    $names = @($servers.Body.Items | ForEach-Object Name)
    Say 'Data servers' $(if ($names.Count) { $names -join ', ' } else { 'none visible to this identity' })
} else { Say 'Data servers' "not read (HTTP $($servers.Status))" }

if (-not $DataServer -or -not $TagName) {
    Write-Host "`nAdd -DataServer and -TagName to check one tag's data format."
    exit 0
}

Write-Host ''
$server = Get-Api ('/dataservers?name=' + [Uri]::EscapeDataString($DataServer) + '&webIdType=IDOnly&selectedFields=WebId;Name')
if ($server.Status -ne 200 -or -not $server.Body.WebId) { Say 'Data server' "NOT FOUND (HTTP $($server.Status))"; exit 4 }
$found = Get-Api ('/dataservers/' + $server.Body.WebId + '/points?nameFilter=' + [Uri]::EscapeDataString($TagName) + '&maxCount=5&startIndex=0&webIdType=IDOnly&selectedFields=Items.WebId;Items.Name;Items.PointType')
$point = @($found.Body.Items | Where-Object { $_.Name -eq $TagName }) | Select-Object -First 1
if (-not $point) { Say 'Tag' 'not found, or not visible to this identity'; exit 4 }
Say 'Tag' "found, type $($point.PointType)"

$from = [Uri]::EscapeDataString($Start.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
$to = [Uri]::EscapeDataString($End.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
$recorded = Get-Api ('/streams/' + $point.WebId + "/recorded?startTime=$from&endTime=$to&boundaryType=Inside&maxCount=5&selectedFields=Items.Timestamp;Items.Value;Items.UnitsAbbreviation;Items.Good;Items.Questionable;Items.Substituted;Items.Annotated")
if ($recorded.Status -ne 200) { Say 'Recorded read' "FAILED (HTTP $($recorded.Status))"; exit 4 }
$items = @($recorded.Body.Items)
Say 'Recorded read' "OK, $($items.Count) events in the window (values not shown)"
if (-not $items.Count) { Write-Host "`nNo events to compare. Try a wider -Start and -End."; exit 0 }

$missing = @()
foreach ($field in 'Timestamp', 'Value', 'Good', 'Questionable', 'Substituted', 'Annotated') {
    if ($items[0].PSObject.Properties.Name -notcontains $field) { $missing += $field }
}
Say 'Fields driver requires' $(if ($missing) { "MISSING: $($missing -join ', '). Please report this." } else { 'all present' })
$objects = @($items | Where-Object { $_.Value -is [pscustomobject] })
if ($objects.Count) {
    $names = $objects[0].Value.PSObject.Properties.Name
    $absent = @('Name', 'Value', 'IsSystem' | Where-Object { $names -notcontains $_ })
    Say 'Digital/system format' $(if ($absent) { "DIFFERENT: lacks $($absent -join ', '). The driver will refuse these rows. Please report this." } else { 'as expected (Name, Value, IsSystem)' })
} else {
    Say 'Digital/system format' 'not checked: no such event here. Try a digital tag to confirm it.'
}
