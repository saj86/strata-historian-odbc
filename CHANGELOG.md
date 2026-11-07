# Changelog

Versions are numbered `major.minor.patch`. The version lives in `src/odbc/version.h`, and every file name uses it.

## 0.2.0 (2026-11-07)

**Installing and setting up**
- Setup wizard: Welcome, licence, features and folder, and a finish page that can open ODBC Data Sources.
- Start menu shortcuts, documents and examples as an optional feature, and an entry in Programs and Features.
- A settings window in ODBC Data Sources (Add and Configure), with a Test button. Credentials used for the test are never saved.
- `scripts\Build-Installer.ps1` builds the MSI on Windows with only the .NET SDK.
- Experimental OLE DB access through Microsoft's OLE DB Provider for ODBC Drivers, with a data link and an example.

**Querying**
- Read `RecordedValues` by `TagName`, in any letter case, as well as by `PointId`.
- Fixed: `Points` filtered on `TagName` returned no rows when the letter case differed.

**Testing and checks**
- A PowerShell mock of PI Web API, run by `Test-Package.ps1`, exercises the driver's HTTP code with no PI server.
- `scripts\Test-PiWebApi.ps1`: a read-only check of a real PI Web API server.
- A Power BI project (`.pbip`) with a one-page report, checked against Microsoft's schemas and TMDL parser.

**Not yet run on Windows:** the installer, the settings window and the OLE DB path. See `docs/status-and-gates.md`.

## 0.1.0 (2026-11-06)

First release: the ODBC driver with the fixture and PI Web API backends, the bounded SQL contract, runbooks, Power BI and Fabric designs, and documentation.
