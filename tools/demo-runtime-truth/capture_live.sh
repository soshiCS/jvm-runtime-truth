#!/usr/bin/env bash
# capture_live.sh — Run demo-runtime-truth under the custom JVM, fire the
# V5 decrypt request, and capture live runtime_targets.jsonl + artifacts.
#
# Usage:
#   ./capture_live.sh [run_dir]
#
# Outputs:
#   <run_dir>/runtime_targets.jsonl
#   <run_dir>/artifacts/*.class
#   <run_dir>/stdout.txt
#   <run_dir>/stderr.txt

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JAR="$SCRIPT_DIR/target/demo-runtime-truth-1.0.0.jar"

if [ ! -f "$JAR" ]; then
  echo "ERROR: JAR not found at $JAR — run 'mvn package -DskipTests' first" >&2
  exit 1
fi

# Custom JVM
JAVA="/Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java"

# Run directory
RUN_DIR="${1:-/tmp/demo_runtime_truth_live}"
mkdir -p "$RUN_DIR/artifacts"

echo "[capture] run_dir = $RUN_DIR"
echo "[capture] jar     = $JAR"
echo "[capture] java    = $JAVA"

# Export env
export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_RUNTIME_RECOVERY=1
export SOROUSH_TRACE_INDY=1
export SOROUSH_TRACE_REFLECTION=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1
export SOROUSH_BYTECODE_DUMP_DIR="$RUN_DIR/artifacts"
export SOROUSH_EXPORT_RUNTIME_TARGETS="$RUN_DIR/runtime_targets.jsonl"
export SOROUSH_REWRITER_PHASE5_PREFIX="com/example/truth"
export SOROUSH_USER_PREFIXES="com/example/truth"

# INSTANCE_TOKEN override: pin to a value that selects transform.037 deterministically.
# Verified: composite="550e8400-e29b-41d4-a716-446655440000:3014540657:4" -> index=37
DEMO_INSTANCE_TOKEN="${DEMO_INSTANCE_TOKEN:-4}"

# Start Spring Boot in background
echo "[capture] starting Spring Boot server (demo.instance.token=$DEMO_INSTANCE_TOKEN)..."
"$JAVA" -Xverify:all -Xint \
  "-Ddemo.instance.token=$DEMO_INSTANCE_TOKEN" \
  -jar "$JAR" \
  --server.port=18080 \
  --logging.level.root=WARN \
  --logging.level.com.example.truth=INFO \
  > "$RUN_DIR/stdout.txt" 2> "$RUN_DIR/stderr.txt" &
SERVER_PID=$!
echo "[capture] server PID = $SERVER_PID"

# Wait for Spring Boot to be ready — poll the TCP port (nc), not a successful HTTP response
echo "[capture] waiting for server to be ready..."
MAX_WAIT=120
WAITED=0
while ! nc -z localhost 18080 2>/dev/null; do
  sleep 2
  WAITED=$((WAITED + 2))
  if [ $WAITED -ge $MAX_WAIT ]; then
    echo "[capture] ERROR: server did not start within ${MAX_WAIT}s" >&2
    kill $SERVER_PID 2>/dev/null || true
    exit 1
  fi
  echo "[capture]   ...still waiting (${WAITED}s)"
done
echo "[capture] server is ready after ${WAITED}s"

# Fire the V5 test request
echo "[capture] sending V5 decrypt request..."
RESPONSE=$(curl -s -X POST \
  "http://localhost:18080/api/decrypt?sessionKey=550e8400-e29b-41d4-a716-446655440000" \
  -H "Content-Type: application/json" \
  -d '{"ciphertext":"2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37"}')
echo "[capture] response: $RESPONSE"
echo "$RESPONSE" > "$RUN_DIR/v5_response.json"

# Give the JVM a moment to finish any async export work
sleep 1

# Graceful shutdown (Spring Boot should flush on SIGTERM)
echo "[capture] shutting down server (SIGTERM -> SIGKILL after 15s)..."
kill -TERM $SERVER_PID 2>/dev/null || true
for i in $(seq 1 15); do
  sleep 1
  kill -0 $SERVER_PID 2>/dev/null || break
done
kill -KILL $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# Report
echo "[capture] done"
echo "[capture] runtime_targets.jsonl: $(wc -l < "$RUN_DIR/runtime_targets.jsonl" 2>/dev/null || echo 'MISSING') lines"
echo "[capture] artifacts: $(ls "$RUN_DIR/artifacts" 2>/dev/null | wc -l) files"
echo ""
echo "=== V5 API Response ==="
cat "$RUN_DIR/v5_response.json"
echo ""
echo ""
echo "=== Record type breakdown ==="
python3 -c "
import json, sys
counts = {}
try:
    with open('$RUN_DIR/runtime_targets.jsonl') as f:
        for line in f:
            r = json.loads(line.strip())
            t = r.get('record', 'unknown')
            counts[t] = counts.get(t, 0) + 1
    for k, v in sorted(counts.items(), key=lambda x: -x[1]):
        print(f'  {v:5d}  {k}')
except Exception as e:
    print(f'  error: {e}')
"
