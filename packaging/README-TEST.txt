Strata Historian ODBC Driver: UNSIGNED TEST PACKAGE

For a test machine only. Do not use on a gateway or production host.
Cross-compiled with mingw-w64, not the MSVC release build. Synthetic fixture data only.
This build has never been run on Windows, so a failure here is a useful result: please keep the output.

Needs: 64-bit Windows 10 or 11. Python and Visual Studio are not required.

1. Unzip the folder, for example to C:\strata-test.
2. Open 64-bit PowerShell as Administrator, then:
     cd C:\strata-test
     powershell -ExecutionPolicy Bypass -File .\scripts\Test-Package.ps1
   It unblocks the files, installs the driver, creates fixture DSNs and runs both test programs.
   Expected last lines: "core_tests: ... 0 failures", "odbc_smoke: ... 0 failures", "ALL PASSED".
3. Power BI Desktop: follow docs\powerbi-acceptance.md (Gate A), from "Procedure".
4. Remove everything:
     powershell -ExecutionPolicy Bypass -File .\scripts\Install-Portable.ps1 -Uninstall

The driver is installed in "C:\Program Files\Strata Historian ODBC". The DSNs are StrataFixture and
StrataFixturePreview.
