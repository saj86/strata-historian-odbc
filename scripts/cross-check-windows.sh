#!/bin/sh
# Compile-and-link check of the Windows x64 artifacts from a non-Windows host with zig's
# mingw-w64 toolchain. Proves the source builds against Windows headers and shows the PE
# exports and imports. It does not run the DLL, and it is not the MSVC release build.
# usage: ZIGCXX="python -m ziglang c++" OUT=dir scripts/cross-check-windows.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${ZIGCXX:?set ZIGCXX, for example: python3 -m ziglang c++}"
OUT=${OUT:-$ROOT/build/cross-windows}
mkdir -p "$OUT"
CORE="src/core/timeutil.cpp src/core/json.cpp src/core/catalog.cpp src/core/sql.cpp src/core/planner.cpp src/backend/fixture_backend.cpp src/backend/piwebapi_backend.cpp src/backend/winhttp_transport.cpp src/backend/loopback_transport.cpp src/odbc/config.cpp src/odbc/dsn_form.cpp"
FLAGS="-target x86_64-windows-gnu -std=c++20 -O1 -Wall -Wextra -fvisibility=hidden -DNOMINMAX"
cd "$ROOT"
$ZIGCXX $FLAGS -shared -o "$OUT/strataodbc.dll" $CORE src/odbc/driver.cpp src/odbc/setup_win.cpp src/odbc/strataodbc.def src/odbc/strataodbc.rc -lwinhttp -lodbccp32 -luser32
$ZIGCXX $FLAGS -o "$OUT/core_tests.exe" tests/core_tests.cpp $CORE -lwinhttp
$ZIGCXX $FLAGS -o "$OUT/odbc_smoke.exe" tests/odbc_smoke.cpp -lodbc32
ls -l "$OUT"
