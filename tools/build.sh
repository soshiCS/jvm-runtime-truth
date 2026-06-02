#!/usr/bin/env bash
# Build the Runtime Truth JVM and copy libjvm into the JDK image.
# Run from the repo root.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

# Configure only if no build directory exists yet
if [ -z "$(ls build/ 2>/dev/null)" ]; then
  echo "[build] Running configure..."
  bash configure --with-debug-level=fastdebug --with-jvm-variants=server
fi

echo "[build] Building hotspot..."
make hotspot

BUILD="build/$(ls build | head -1)"
if [ "$(uname)" = Darwin ]; then
  LIB=libjvm.dylib
else
  LIB=libjvm.so
fi

SRC="$BUILD/support/modules_libs/java.base/server/$LIB"
DST="$BUILD/jdk/lib/server/$LIB"

echo "[build] Copying $LIB  →  $DST"
cp "$SRC" "$DST"

JAVA="$REPO/$BUILD/jdk/bin/java"
echo ""
echo "Done. Custom java binary:"
echo "  $JAVA"
echo ""
echo "Verify:"
echo "  $JAVA -version"
