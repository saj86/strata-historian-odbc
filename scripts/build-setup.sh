#!/bin/sh
# Builds the Windows setup program: dist/strata-historian-odbc-setup-<version>.exe
#
# Runs on macOS, Linux or Windows with the .NET 10 SDK. It cross-compiles the driver DLL (scripts/cross-check-windows.sh),
# packs it with the licence, documents and examples as payload.zip, and publishes setup/StrataHistorian.Setup as one
# self-contained win-x64 exe with that zip embedded. The exe needs no .NET on the target machine. Nothing is signed.
#
# usage: ZIGCXX="python3 -m ziglang c++" scripts/build-setup.sh      (make setup sets ZIGCXX for you)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOTNET=${DOTNET:-dotnet}
VERSION=$(awk '/STRATA_VERSION_MAJOR/{a=$3} /STRATA_VERSION_MINOR/{b=$3} /STRATA_VERSION_PATCH/{c=$3} END{print a"."b"."c}' "$ROOT/src/odbc/version.h")
DLL=${DLL:-$ROOT/build/cross-windows/strataodbc.dll}

if [ -z "${SKIP_DLL_BUILD:-}" ]; then
  OUT="$ROOT/build/cross-windows" "$ROOT/scripts/cross-check-windows.sh" >/dev/null
fi
[ -f "$DLL" ] || { echo "No driver DLL at $DLL" >&2; exit 1; }

# A new temporary folder each time, so nothing old is carried over and nothing needs deleting.
STAGE=$(mktemp -d)
mkdir -p "$STAGE/payload"
cp "$DLL" "$STAGE/payload/strataodbc.dll"
printf '%s\n' "$VERSION" > "$STAGE/payload/VERSION.txt"
cp "$ROOT/installer/License.txt" "$STAGE/payload/License.txt"
(cd "$ROOT" && git archive HEAD docs samples | tar -x -C "$STAGE/payload")
(cd "$STAGE/payload" && zip -qr ../payload.zip .)

"$DOTNET" publish "$ROOT/setup/StrataHistorian.Setup" -c Release -r win-x64 --self-contained true \
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true \
  -p:DebugType=None -p:DebugSymbols=false -p:Version="$VERSION" -p:SetupPayload="$STAGE/payload.zip" \
  --artifacts-path "$STAGE/artifacts" -o "$STAGE/out" >/dev/null

mkdir -p "$ROOT/dist"
EXE="$ROOT/dist/strata-historian-odbc-setup-$VERSION.exe"
cp "$STAGE/out/StrataSetup.exe" "$EXE"
(cd "$ROOT/dist" && shasum -a 256 "$(basename "$EXE")" > "$(basename "$EXE").sha256")
echo "PAYLOAD=$STAGE/payload.zip"
echo "Setup program ready: $EXE ($(du -m "$EXE" | cut -f1) MB, unsigned)"
