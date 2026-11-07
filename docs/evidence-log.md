# Evidence log

Confidence reflects source quality, not how likely the claim feels. Dates are 2026-11-06 and 2026-11-07.

## Sources actually read

| Source | How read | Result |
|---|---|---|
| Fabric ODBC overview, connection, Copy source pages | Fetched | Authentication table, connection fields, query settings |
| Fabric on-premises gateway page | Fetched | Minimum gateway version 3000.214.2; outbound `*.frontend.clouddatahub.net`; "Local access ... isn't allowed in pipelines" |
| Gateway custom connectors page | Fetched | Kerberos limitation, `TestConnection` requirement, one custom data source in DirectQuery |
| Power Query ODBC connector page | Fetched | Authentication kinds; attributes after a DSN are not included |
| Power Query ODBC extension page | Partly fetched | Guidelines; capability override details not read in full |
| AVEVA PI System schedule v1.5 PDF | Read directly | §3.3(b)(iv) PSA, §3.5 PI Integrators |
| AVEVA GTC v3.0 PDF | Text extracted | §3, §3.1, §3.2 |
| AVEVA PI Web API reference | **Page body not retrievable (JavaScript-rendered)** | Only search excerpts: empty result is success with empty `Items`; boundary type affects returned times; 409 covers unsupported method and similar |
| `github.com/mdrexel-osi/gowebapi` | Source read through the Go module proxy | Third party, generated, 2018. Routes, parameters, model field names |
| Microsoft "the ODBC solution", Lakehouse Copy destination | **Not read** | Re-check before the Fabric pilot |

## PI Web API field contract

Everything below is secondary or partial evidence. Gate B is what turns it into a verified contract.

| Implemented behavior | Source | Confidence | Test that could disprove it |
|---|---|---|---|
| `GET /dataservers?name=` returns one server with `WebId` | gowebapi `DataServerGetByName` | Medium | L1 |
| `GET /dataservers/{webId}/points` with `maxCount`, `startIndex`, `nameFilter`, `webIdType`, `selectedFields` | gowebapi `DataServerGetPoints` | Medium | L2, L3 |
| `GET /points/{webId}` | gowebapi route | Medium | L3 |
| `GET /streams/{webId}/recorded` with `startTime`, `endTime`, `boundaryType`, `maxCount`, `selectedFields` | gowebapi `StreamGetRecorded`; AVEVA excerpt | Medium | L4 |
| Item fields `Timestamp`, `Value`, `UnitsAbbreviation`, `Good`, `Questionable`, `Substituted`, `Annotated`, `Errors` | gowebapi models | Medium | L4 |
| Point fields `WebId`, `Name`, `Descriptor`, `PointClass`, `PointType`, `EngineeringUnits`, `DigitalSetName`, `Step` | gowebapi models | Medium | L2 |
| Empty window returns success with empty `Items` | AVEVA excerpt | Medium-high | L5 |
| Digital or system `Value` is an object with `Name`, `Value`, **`IsSystem`** | **`IsSystem` confirmed by no source I could read**; from prior knowledge | **Low** | L4, L7. The parser errors if absent, so failure is loud |
| `IDOnly` WebIds stay valid if an item moves | Search excerpt of a PI World lab | Medium | L10 rename test |
| Default `maxCount` | **Sources conflict (1000 vs 10000)**. The driver always sends it explicitly | n/a | L6 |
| A full page means there may be more; none of the routes used has a continuation token | Inference from the parameter list | Low-medium | L6 |

Because the parser requires each field and rejects anything else (`Unexpected PI Web API response ... No value was
inferred`), a wrong assumption above produces an error and never a wrong value.

## Observations run on this host

macOS arm64, Apple clang 21, unixODBC 2.3.14 built privately, zig 0.17.1 (mingw-w64 target). Driver version 00.01.0000.

| Test | Result | What it establishes | What it does not |
|---|---|---|---|
| `core_tests` | Passed: 212 checks, 0 failures | Parser, planner, fixture semantics, PI Web API mapping against scripted responses, config rules | Any server behavior |
| `odbc_smoke` through the unixODBC driver manager | Passed: 782 checks, 0 failures | ODBC call paths, Unicode, binding, rowsets, diagnostics, cancel, concurrency, golden rows | Windows driver manager or Power BI behavior |
| Both under AddressSanitizer + UBSan | Passed: 0 findings | No memory or undefined-behavior errors on those paths | WinHTTP code, which does not run here |
| Both under ThreadSanitizer | Passed: 0 findings | No data races on those paths (8 threads, cancel thread) | Same |
| Windows x64 cross-compile (zig/mingw-w64) | Passed: 0 warnings | Source compiles and links against Windows headers, including WinHTTP and setup code | That the DLL loads or runs; MSVC behavior |
| PE inspection of `strataodbc.dll` | Passed | Machine 0x8664, version resource 0.1.0.0, 50 exports matching the `.def`; imports only `kernel32`, `user32`, `odbccp32`, `winhttp`, UCRT. **No PI or OLE DB imports** | Clean-host behavior |
| C# and PowerShell samples through .NET `OdbcConnection` | Passed | A second, independent ODBC client works on the same driver: parameters, typed reads, NULLs, errors, cancel | Behavior of .NET's Windows driver-manager path |
| Fabric rules model | Passed: 29 tests | Replay, crash, watermark, amend, delete, checksum, precision, window validation and point merge rules | Spark, Delta or Fabric |
| PowerShell scripts | 0 parse errors (PowerShell 7) | Syntax | Any behavior; they need Windows |
| `tests/mock_piwebapi.py` | Passed: manual `curl` of 3 routes | The mock responds | Anything about PI |
| WiX build | **Failed on macOS: WIX0389**; same error with a no-space name, and WiX warns it supports only Windows | Nothing. Treat MSI as Not run | |

### Mutation check

Run on 2026-11-06, before the later fixes, and not repeated.

Faults were injected into the safety behaviors to confirm the tests detect them.

16 mutants: 14 detected, 2 behaviorally equivalent on inspection. One detection (ignoring the `Retry-After` cap) showed up as a test timeout, because the mutant really did wait 9999 seconds.

| Survivor | Why |
|---|---|
| Missing field returns a null node | `needBool` and `needStr` still reject a null node, so behavior is unchanged |
| Rowset failure not stored as pending | The source retries the same continuation on the next fetch, so a persistent fault resurfaces and no row is lost. A fault that cleared by itself would just continue correctly |

The first pass found one real gap, now closed: no test covered cancel during a long backoff sleep.

### Bugs and false claims found while verifying

- `SQL_DATETIME_LITERALS` advertised SQL-92 literals the parser rejects. Now 0, with a test.
- `SQL_ODBC_API_CONFORMANCE` claimed Level 1. Now `SQL_OAC_NONE`.
- A Fabric test keyed events by whole seconds and collided on the sub-second fixture events. The test was wrong; it now keys on the raw timestamp.

## Bounded-request trace (fixture)

```
{"event":"plan","table":"RecordedValues","upstream":"none (zero-row result; no source access)"}
{"event":"plan","table":"RecordedValues","upstream":"getPoint(PT-PRESSURE-001) + readRecorded(point=PT-PRESSURE-001, start=2026-01-15T08:00:00.0000000Z, end=2026-01-15T09:00:00.0000000Z, boundary=Inside, pageSize=1000)"}
{"event":"readRecorded","backend":"fixture","point":"PT-PRESSURE-001","start":"2026-01-15T08:00:00.0000000Z","end":"2026-01-15T09:00:00.0000000Z","pageSize":1000,"rows":7,"more":false}
```

## Native protocol transactions

None. Zero observations, zero implemented fields. Hostnames, ports, message formats and capabilities are **unknown**.
