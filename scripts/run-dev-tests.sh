#!/bin/sh
# Portable contract tests against a local unixODBC driver manager. This is NOT a Windows or
# Power BI result. usage: ODBC_PREFIX=/path/to/unixodbc CMAKE=cmake NINJA=ninja scripts/run-dev-tests.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${ODBC_PREFIX:?set ODBC_PREFIX to a unixODBC install prefix}"
CMAKE=${CMAKE:-cmake}
BUILD="${BUILD:-$ROOT/build/dev}"
"$CMAKE" -S "$ROOT" -B "$BUILD" -G Ninja ${NINJA:+-DCMAKE_MAKE_PROGRAM="$NINJA"} -DCMAKE_BUILD_TYPE=Debug -DODBC_PREFIX="$ODBC_PREFIX" ${SANITIZE:+-DCMAKE_CXX_FLAGS="-fsanitize=$SANITIZE -fno-omit-frame-pointer"} ${SANITIZE:+-DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=$SANITIZE"} ${SANITIZE:+-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$SANITIZE"} >/dev/null
"$CMAKE" --build "$BUILD"
"$BUILD/core_tests"
case "$(uname)" in Darwin) EXT=dylib;; *) EXT=so;; esac
LIB="$BUILD/strataodbc.$EXT"
[ -f "$LIB" ] || LIB="$BUILD/strataodbc.so"
CFG="$BUILD/odbcini"; mkdir -p "$CFG"
cat > "$CFG/odbcinst.ini" <<EOF
[Strata Historian ODBC Driver]
Driver=$LIB
UsageCount=1
EOF
cat > "$CFG/odbc.ini" <<EOF
[StrataFixture]
Driver=Strata Historian ODBC Driver
Backend=fixture
[StrataFixturePreview]
Driver=Strata Historian ODBC Driver
Backend=fixture
NavigatorPreview=1
EOF
TRACE="$BUILD/trace.jsonl"; rm -f "$TRACE"
ODBCSYSINI="$CFG" ODBCINI="$CFG/odbc.ini" ODBCINSTINI=odbcinst.ini "$BUILD/odbc_smoke" StrataFixture StrataFixturePreview "DRIVER={Strata Historian ODBC Driver};" "$ROOT/fixtures/expected_pressure_window.csv" "$TRACE"
