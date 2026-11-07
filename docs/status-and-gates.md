# Status and gates

Last updated 2026-11-07. Statuses are Passed, Failed or Not run, with actual results only. "Passed" below means on the stated environment, and no further.

## Gates

| Gate | Requirement | Status | What is missing |
|---|---|---|---|
| A | Clean Windows, MSI, fixture DSN, Power BI imports the exact rows | **Not run** | A Windows x64 machine with Power BI Desktop. Run [powerbi-acceptance.md](powerbi-acceptance.md) |
| B | Live read verified against a trusted reader; denied stays denied | **Not run** | PSA answer, PI Web API URL and version, auth scheme, test identity, authorized and denied points, reference reader: [backend-decision.md](backend-decision.md) |
| C | Same version on each gateway node, unattended refresh, boundary counts compared | **Not run** | Gate B plus gateway nodes and a workspace |
| D | Full reference feature matrix with evidence | **Not run** | Named reference product and version. Coverage is in [compatibility-matrix.md](compatibility-matrix.md) |
| E | Restart, failover, rotation, upgrade, rollback, incremental refresh trace, separate DirectQuery result | **Not run** | Gate C. DirectQuery: **Not run**, unsupported |
| F | Fabric pilot: source, staging, curated; replay; failure; amendment | **Not run** (pending tenant). Artifacts published; rules model Passed | A Fabric tenant. Pipeline Copy: Not run. Dataflow Gen2: Not run |

## What has been tested

| Check | Environment | Status | Result |
|---|---|---|---|
| Portable core tests | macOS arm64, Apple clang 21 | Passed | 212 checks, 0 failures |
| Native ODBC smoke, including exact golden rows | macOS, unixODBC 2.3.14 driver manager | Passed | 782 checks, 0 failures |
| Both under ASan + UBSan | same | Passed | 0 findings |
| Both under TSan | same | Passed | 0 findings |
| Windows x64 DLL and test executables compile and link | zig 0.17.1 mingw-w64 cross build | Passed | 0 warnings |
| PE exports and imports | `pefile` | Passed | x64, 50 exports match `.def`, OS DLLs only |
| C# sample through .NET `OdbcConnection` | macOS, .NET 10, unixODBC | Passed | 7 rows, refusals, schema probe, mid-read cancel |
| PowerShell sample through `OdbcConnection` | macOS, PowerShell 7.6, unixODBC | Passed | 7 rows, refusal, CSV export, chunked windows |
| `make build`, `test`, `install`, `uninstall`, `package-windows` | macOS | Passed | Clean rebuild; queried through the installed driver |
| Fabric rules model | Python 3.14 | Passed | 29 tests |
| Power BI project consistency | Python 3.14 | Passed | 8 tests: visual fields, model columns, SQL and driver catalog agree |
| Power BI project JSON | Microsoft's published schemas | Passed | 17 files, 0 errors. A deliberately broken copy was rejected |
| Power BI project model | Microsoft's TMDL parser (`Microsoft.AnalysisServices` 19.84.1) | Passed | 4 tables, 8 measures, 2 relationships. A broken copy was rejected |
| Power BI project SQL through the installed driver | macOS, unixODBC | Passed | 2, 7 and 4 rows |
| Mutation check (run before the 2026-11-07 fixes, not repeated) | macOS | Passed | 16 mutants: 14 detected, 2 equivalent |
| PowerShell scripts parse | PowerShell 7.6 on macOS | Passed | 0 parse errors. **Syntax only** |
| JSON artifacts parse | Python | Passed | |
| MSI build | macOS | **Failed** | WiX supports only Windows; treat MSI as unverified |
| MSVC build, `ctest` on Windows | | Not run | |
| Driver DLL loaded by the Windows driver manager | | Not run | |
| WinHTTP transport, TLS, cancel on real sockets, `Negotiate`, `ConfigDSNW` | | Not run | Compiled only; this code has never executed |
| PowerShell behavior | | Not run | |
| Power Query M, `.mez` | | Not run | |
| Power BI project opened in Desktop, DAX measures, M queries, page layout | | Not run | No schema or parser here evaluates these |
| PySpark notebooks, `curated_tables.sql` | | Not run | No Java on this host. Syntax parsed only |
| Samples and test package on Windows | | Not run | |
| GitHub Actions workflow | | Not run | |
| Any PI server | | Not run | |

## Known limitations

- Recorded values only. That makes this an early release, and it doesn't close full coverage.
- OIDC authentication is not implemented.
- `PointId IN (...)` reads points one at a time.
- `ORDER BY` other than natural order buffers rows in memory, up to `MaxRows`.
- More events at one timestamp than `PageSize` raises an error. Raise `PageSize`.
- `ConfigDSNW` has no dialog, by design. Use the provisioning script.
- Pipeline correlation with driver traces is by point, window and time, not a shared ID.
- `curated_af_attributes` and `curated_event_frames` are table definitions only, because no backend can fill them yet.
- The driver version is set by hand in `src/odbc/version.h`; `Build-And-Test.ps1 -Version` only sets the MSI version, so change both together.

## Open questions

Blocking:

1. **Does the site hold a PSA licence that covers this driver?**
2. Is PI Web API deployed? Version? Which authentication?
3. Which reference product and version define "full coverage"?
4. What trusted reader is approved for Gate B?

Decisions for later:

5. Final driver name, before Gate C.
6. Correction horizon for late and amended history.
7. Gateway identity and audience: one model for everyone, or row-level security?
8. Provider-specific query migration: are there existing queries to port?
9. Do write, admin or COM compatibility need to be in scope? Currently blocked by design.

Technical, answered only by running: see "Known unknowns" in [powerbi-acceptance.md](powerbi-acceptance.md), the pilot list in [fabric-design.md](fabric-design.md), and low-confidence rows in [evidence-log.md](evidence-log.md), notably `IsSystem`.
