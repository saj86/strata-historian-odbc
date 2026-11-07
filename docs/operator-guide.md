# Operator guide

All procedures below are **Not run** on Windows. Use them as the Gate A, C and E test script and correct anything that does not match.

## Build

Prerequisites: VS 2022 Build Tools (`Desktop development with C++`, MSVC v143, Windows 10 or 11 SDK), CMake ≥ 3.21, .NET SDK, `dotnet tool install --global wix --version 5.0.2`.

```powershell
git clean -xfd                                                       # clean build; review what it removes first
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
wix build installer\strataodbc.wxs -arch x64 -d ProductVersion=0.1.0 -d "DriverPath=build\windows-x64\Release\strataodbc.dll" -o build\strataodbc-0.1.0-x64.msi
```

The static CRT in `CMakeLists.txt` means no VC++ redistributable. Or run `scripts\Build-And-Test.ps1`, or the provided `.github/workflows/windows-build.yml` (also never yet executed).

**Sign** the DLL before the MSI build and the MSI after, with the organization's certificate, then verify:

```powershell
signtool sign /fd SHA256 /tr <your-timestamp-url> /td SHA256 /sha1 <thumbprint> build\windows-x64\Release\strataodbc.dll
# ...build the MSI...
signtool sign /fd SHA256 /tr <your-timestamp-url> /td SHA256 /sha1 <thumbprint> build\strataodbc-0.1.0-x64.msi
Get-AuthenticodeSignature build\strataodbc-0.1.0-x64.msi
```

Bump `ProductVersion` for every release: `MajorUpgrade` keys on it. Keep each released signed MSI for rollback. Change `src/odbc/version.h` and `project(VERSION)` in `CMakeLists.txt` together: the DLL's file version and `SQL_DRIVER_VER` come from that header.

## Install, upgrade, repair, uninstall

```powershell
.\scripts\Install-StrataDriver.ps1 -Action Install   -MsiPath <msi> -ExpectedThumbprint <thumbprint>
.\scripts\Install-StrataDriver.ps1 -Action Upgrade   -MsiPath <new-msi> -ExpectedThumbprint <thumbprint>
.\scripts\Install-StrataDriver.ps1 -Action Repair    -MsiPath <msi> -ExpectedThumbprint <thumbprint>
.\scripts\Install-StrataDriver.ps1 -Action Uninstall -MsiPath <installed-msi>
```

The script stops and restarts `PBIEgwService` if running, because mashup processes can hold the DLL. It never reboots a host; exit code 3010 is reported.

**Rollback:** downgrade is blocked by design, so uninstall the new version and install the previous signed MSI, then run the health check. DSNs survive because the MSI does not own them.

## DSN provisioning

```powershell
.\scripts\Provision-StrataDsn.ps1 -Name StrataPI -Backend piwebapi -BaseUrl https://piweb.example.local/piwebapi -DataServer PISRV01 -Auth Basic
```

It is idempotent, targets the x64 hive, removes settings not in the desired state (including stray `UID`/`PWD`), and takes no credentials. Fixture and production DSNs use different names. Settings:

| Setting | Default | Purpose |
|---|---|---|
| `Backend` | none: required | `fixture` or `piwebapi` |
| `BaseUrl`, `DataServer` | | PI Web API; `https` required, `http` only on loopback |
| `Auth` | Basic when `UID` given, else Anonymous | `Anonymous`, `Basic`, `Negotiate` |
| `MaxWindowDays`, `MaxRows`, `MaxPointsPerQuery` | 31, 2,000,000, 16 | Query bounds |
| `PageSize` | 1000 | Keep below the server limit |
| `RequestTimeoutMs`, `MaxRetries` | 30000, 3 | Per request |
| `MaxConcurrency` | 4 | Upstream requests per process, per `BaseUrl` |
| `NavigatorPreview`, `PreviewWindowHours`, `PreviewRows` | 0, 24, 100 | See SQL contract |
| `TraceFile` | off | JSON-lines; must be writable by the process identity |

Limits on retries: transient failures (429, 502, 503, 504, link failure) back off from 0.5 s, doubling, up to `MaxRetries`. `Retry-After`, in seconds or as an HTTP date, is honored up to 60 s; longer, and the driver fails rather than wait. Everything else fails at once. The cap on load across a whole cluster is `nodes × processes × MaxConcurrency`: mashup processes can multiply, so **measure before tightening or relaxing**.

## Hosts

| Host | Install | Do not install |
|---|---|---|
| Analyst workstation | Power BI Desktop, signed MSI, DSNs: `StrataFixture`, `StrataPI` with `NavigatorPreview=1`, trust roots | PI OLE DB, PI SQL Client |
| Each gateway cluster member | Standard on-premises data gateway, same MSI version, `StrataPI` with `NavigatorPreview=0` and **the same DSN name**, trust roots, no credentials in the DSN | Fixture DSN in production |
| PI side | PI Web API instance, least-privilege identity and mappings. **Nothing from this project** | |
| Reporting host | None for the selected route | |

## Publish and refresh

1. In Desktop, build from bounded SQL (`powerquery/examples.pq`) using DSN `StrataPI`. Publish to the workspace.
2. Power BI service → Manage connections and gateways → New → On-premises → your cluster → type **ODBC** → connection string `dsn=StrataPI` → authentication per [security.md](security.md). The string must match what the model uses. Keep all settings in the DSN: attributes after `DSN=` are not included.
3. Semantic model → Settings → Gateway and cloud connections → map the data source to that connection.
4. **Refresh now**, then open Refresh history. Check duration, status and any row error. Compare `Events` per window with the source (next section).
5. Set the scheduled refresh; test a run while no one is watching: this is Gate C's "unattended".
6. Restart the gateway service and refresh again.
7. Failover: stop the gateway service on the member in use during a refresh. Check that the published model keeps its previous data, that history shows a failure or a retry success, and that a later refresh is complete.

## Incremental refresh

Use example 3 in `powerquery/examples.pq`. `RangeStart` is inclusive and `RangeEnd` exclusive, matching the SQL. Power BI may warn the query does not fold. That is expected: bounds are in the SQL itself, so what matters is evidence:

**Proof each partition reads only its span:** enable `TraceFile`, refresh, and check that every `readRecorded` event's `start` and `end` fall within one partition. Compare `rows` to the partition's row count in the model at both boundaries, placing a test event exactly on a boundary.

**Late, amended, deleted history:** incremental refresh keeps older partitions frozen. Set the detect-changes period to at least the site's correction horizon (ask the site), and periodically do a full reprocess of old partitions. Deletion can only be seen by re-reading a window. DST: partitions are in UTC, so they have no repeated or skipped hour.

`MaxWindowDays` must be at least the size of the largest partition.

## Performance measures

Targets are **provisional until the site supplies a baseline**. Compare across runs: rows, upstream requests, bytes, refresh duration, source query duration. `TraceFile` provides per-request `op`, `attempt`, `status`, `bytes`, `ms`, `rows`, and correlation IDs. Watch PI Web API server load during the first full load. Add monitoring on that server, since the driver can only see its own side.

## Health checks and drift

```powershell
# On one healthy member: capture a baseline
.\scripts\Test-StrataNode.ps1 -Dsn StrataPI -Credential $c -SmokeSql "<bounded query>" -ExpectedRows <n> > baseline.json
# On every member, on a schedule; non-zero exit means alert
.\scripts\Test-StrataNode.ps1 -Dsn StrataPI -Credential $c -SmokeSql "<bounded query>" -ExpectedRows <n> -Baseline baseline.json -ExpectedThumbprint <t>
```

It checks the driver's registration, hash and signature, DSN resolution, no secrets in the DSN, network, certificate chain and a bounded query. Drift compares `driverFileVersion`, `driverSha256`, `dsnSettings`, `backend` and the server certificate thumbprint, and each difference fails. Run it as a scheduled task on each member and alert on the exit code. Also run it under the gateway's service identity, as that identity's access is what counts.

Service identity access: `NT SERVICE\PBIEgwService` by default. The driver folder under Program Files is readable by default. `TraceFile`'s folder needs explicit write access, and the host's trust store must contain the PI Web API chain.

## Rolling upgrade

Keep one member healthy at all times:

1. Run the health check everywhere and pause scheduled refreshes if a member is unhealthy.
2. On a member: remove it from load (disable in cluster settings), wait for running work to drain, `Install -Action Upgrade`, health check, re-enable.
3. Run one refresh pinned to the upgraded member. Then repeat for the others.
4. Mixed versions are only for the upgrade window: the drift check will fail meanwhile, so finish the rollout or roll back.
5. Rollback is the same process with uninstall and the old MSI.

Failover during refresh must not leave a partial published partition. Power BI's refresh is expected to be transactional for this; verify in Gate E. For Fabric duplicates, see window replacement in [fabric-design.md](fabric-design.md).

## Custom connector (optional)

Not needed for Import. If it is ever used: sign the `.mez` or set the trusted-connector policy on each host; place it in a folder readable by the gateway identity (not under a user profile); enable custom connectors for the cluster; note the Kerberos limitation; `TestConnection` is implemented in the source but unverified.
