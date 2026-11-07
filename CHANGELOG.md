# Changelog

Versions are numbered `major.minor.patch`. The version lives in `src/odbc/version.h`, and every file name uses it.

## 0.3.0 (2026-11-07)

**Setup program**
- `strata-historian-odbc-setup-<version>.exe`: an English setup wizard, one self-contained file that needs nothing else installed.
- Installs, updates, sets up data sources, and uninstalls. Registers the driver with Windows, adds it to Programs and Features and the Start menu, and keeps a copy for later changes.
- Sets up a system data source and an OLE DB data link (`.udl`) for it, and tests the connection through ODBC and OLE DB. Test credentials are never saved.
- Can stop the on-premises data gateway during setup and start it again, even when setup fails.
- Refuses to take over a driver installed by the MSI or a script.
- `make setup` builds it on macOS, Linux or Windows. `--demo` shows every page without changing anything.

**Not yet run on Windows:** the setup program. Its install logic is tested against a simulated Windows.

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
