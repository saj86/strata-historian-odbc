# Strata Historian ODBC Driver

An independent, read-only ODBC 3.52 driver in original C++20. It is built for Power BI Import through a
Windows x64 System DSN and the on-premises gateway, with a Fabric ingestion path designed but not yet
piloted. PI OLE DB, PI SQL Client and their DLLs are not runtime dependencies. "Strata" is a working
name: it appears in the DLL name, the registry entries and the catalog name, so settle it before Gate C.

**Current state: fixture-backed first release. Nothing has run on Windows, in Power BI or against a PI
server yet.** Read [docs/status-and-gates.md](docs/status-and-gates.md) first.
The licensing question in [docs/backend-decision.md](docs/backend-decision.md) can change the budget assumption.

## What you get

| Artifact | Path | Status |
|---|---|---|
| Native x64 ODBC driver DLL (source) | `src/` | Compiled for Windows with mingw-w64 cross tools; MSVC build Not run |
| Offline fixture backend | `src/backend/fixture_backend.cpp` | Passed through a real driver manager (unixODBC on macOS) |
| PI Web API backend + WinHTTP transport | `src/backend/piwebapi_*`, `winhttp_transport.cpp` | Mapping Passed against scripted responses; never run against a server |
| WiX MSI source | `installer/` | Not run |
| PowerShell runbooks | `scripts/*.ps1` | Parsed by PowerShell 7 with no errors; never executed |
| Built-in connector M queries | `powerquery/examples.pq` | Not run |
| Optional `.mez` source (separate artifact) | `powerquery/StrataHistorian/` | Not run; not needed for Import |
| Fabric design, schemas, notebooks, rules model | `fabric/`, `docs/fabric-design.md` | Rules model Passed; PySpark notebooks and tenant pilot Not run |

An application calling ODBC, a DSN, and a Power Query `.mez` are distinct things. This repository delivers
the driver and a script that creates DSNs. The basic gateway path uses the **built-in** ODBC connector with no `.mez`.

## Quick start on Windows x64

Prerequisites: Visual Studio 2022 Build Tools with "Desktop development with C++" and a Windows 10/11 SDK,
CMake 3.21+, and `dotnet tool install --global wix --version 5.0.2`. From an **elevated** 64-bit PowerShell:

```powershell
.\scripts\Build-And-Test.ps1
```

This builds the DLL and tests, builds the MSI, installs it, creates the fixture DSNs and runs the native
smoke test. Then follow [docs/powerbi-acceptance.md](docs/powerbi-acceptance.md) for Gate A.

## Portable tests on macOS or Linux

These prove the driver contract, not PI connectivity or Power BI behavior.

```sh
ODBC_PREFIX=/path/to/unixodbc scripts/run-dev-tests.sh      # needs cmake and ninja
python3 -B -m unittest discover -s fabric/tests
```

## Examples

| Language | Path | Tested |
|---|---|---|
| PowerShell module and script | `samples/powershell/` | Ran on macOS against the fixture; Not run on Windows |
| C# (.NET 10, `System.Data.Odbc`) | `samples/csharp/StrataSample/` | Ran on macOS against the fixture; Not run on Windows |
| Power BI project (`.pbip`), M queries and DAX | `samples/powerbi/` | Project passed Microsoft's schemas and TMDL parser; not opened in Power BI Desktop |

On macOS or Linux, `make install` installs the driver and fixture DSNs under `~/.local/strata-odbc`, and `make test` and `make query` cover the rest. `make package` builds one complete zip with the Windows binaries, documents, examples and source. See the top of the `Makefile`.

## Documentation

| Topic | Document |
|---|---|
| How to install and use it, with examples | [usage-guide.md](docs/usage-guide.md) |
| Rollout for a team, and the licence question to ask | [team-rollout.md](docs/team-rollout.md) |
| Gates, test results, open questions | [status-and-gates.md](docs/status-and-gates.md) |
| Recommended backend, evidence, costs to verify | [backend-decision.md](docs/backend-decision.md) |
| SQL subset, prepared-query example, Navigator preview | [sql-contract.md](docs/sql-contract.md) |
| Typed data contract and PI semantics | [data-contract.md](docs/data-contract.md) |
| Feature inventory and coverage | [compatibility-matrix.md](docs/compatibility-matrix.md) |
| Live-server lab protocol, evidence log | [lab-protocol.md](docs/lab-protocol.md), [evidence-log.md](docs/evidence-log.md) |
| Power BI acceptance and report page | [powerbi-acceptance.md](docs/powerbi-acceptance.md) |
| Build, install, gateway deployment, rollback | [operator-guide.md](docs/operator-guide.md) |
| Security and identity | [security.md](docs/security.md) |
| Fabric design | [fabric-design.md](docs/fabric-design.md) |
| Dependencies | [dependency-inventory.md](docs/dependency-inventory.md) |
| What changed in each version | [CHANGELOG.md](CHANGELOG.md) |
