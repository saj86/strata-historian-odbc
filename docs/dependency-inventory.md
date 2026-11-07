# Dependency inventory

## Shipped in the MSI

Only `strataodbc.dll`. No vendor binaries, and no third-party code is compiled in.

## OS-provided at runtime

`kernel32`, `user32`, `odbccp32` (ODBC installer API), `winhttp`. The MSVC build links the CRT statically. The mingw cross-check build imports the UCRT, which modern Windows includes; that build is not the release build.

The Windows ODBC Driver Manager (`odbc32`) loads the driver; the driver doesn't link to it.

## The setup program

`strata-historian-odbc-setup-<version>.exe` is self-contained: it carries the .NET 10 runtime and Windows Forms (open source, from Microsoft), and the `System.Data.Odbc`, `System.Data.OleDb` and `System.ServiceProcess.ServiceController` packages, all MIT-licensed. Its tests use xUnit (open source). OLE DB data links use Microsoft's OLE DB Provider for ODBC Drivers, which is part of Windows.

## Build-time

| Component | Kind | Used for |
|---|---|---|
| Visual Studio 2022 C++ tools, Windows SDK | Site-provided (licensed by Microsoft) | Release build. Check that your Visual Studio licence fits; Build Tools licensing follows it |
| CMake, Ninja | Open source | Build |
| WiX Toolset 5 | Open source; **its licensing terms should be checked for your organization** | MSI |
| .NET SDK | Open source | Runs WiX |
| Code-signing certificate, `signtool` | Site-provided | Signing |

## Test-only, not shipped

| Component | Kind | Note |
|---|---|---|
| unixODBC | Open source (LGPL) | Portable driver-manager tests only |
| zig (mingw-w64 target), `pefile` | Open source | Cross-compile check and PE inspection on non-Windows hosts |
| Python 3 standard library | Open source | Mock server, Fabric rules and Power BI project tests |
| `jsonschema`, `referencing` | Open source | Power BI project schema checks |
| `Microsoft.AnalysisServices.NetCore.retail.amd64` | Microsoft NuGet package; check its licence terms before redistributing | Parses TMDL in `tools/tmdlcheck`. Downloaded at check time, never shipped |

## Site-provided, for later gates

Power BI Desktop, on-premises data gateway, PI Web API and any licence it needs including PSA ([backend-decision.md](backend-decision.md)), a trusted reference reader, Fabric capacity, Spark and Delta in Fabric. None of this is in the repository.

## Independence check

`strataodbc.dll` imports were listed from the PE file: no PI, OSIsoft, AVEVA or OLE DB DLLs. PI Web API responses are consumed as JSON over HTTP, so no vendor client libraries are involved.
