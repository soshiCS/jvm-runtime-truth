#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run_demo.sh — Build and run the Runtime Truth Causality Demo under the custom JVM
# ---------------------------------------------------------------------------
# Usage:
#   ./run_demo.sh                    # server mode (default): keep running, curl endpoints
#   ./run_demo.sh --non-interactive  # exercise all 4 bugs then exit (CI mode)
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
APP_DIR="$SCRIPT_DIR"

CUSTOM_JAVA="$REPO_ROOT/build/macosx-aarch64-server-fastdebug/jdk/bin/java"
EXPORT_DIR="/tmp/demo-buggy-app-export"
BYTECODE_DIR="/tmp/demo-buggy-app-bytecode"

if [[ ! -x "$CUSTOM_JAVA" ]]; then
  echo "ERROR: custom JVM not found at $CUSTOM_JAVA"
  echo "  Build with: make images CONF=macosx-aarch64-server-fastdebug"
  exit 1
fi

# ---------------------------------------------------------------------------
# Step 1: Maven build
# ---------------------------------------------------------------------------
echo "=== Building demo-buggy-app ==="
cd "$APP_DIR"
mvn -q package -DskipTests

JAR=$(ls target/demo-buggy-app-*.jar 2>/dev/null | head -1)
if [[ -z "$JAR" ]]; then
  echo "ERROR: no jar found in target/"
  exit 1
fi
echo "  Built: $JAR"

# ---------------------------------------------------------------------------
# Step 2: Prepare export directories
# ---------------------------------------------------------------------------
mkdir -p "$EXPORT_DIR" "$BYTECODE_DIR"
JSONL_PATH="$EXPORT_DIR/runtime_targets.jsonl"
rm -f "$JSONL_PATH"

# ---------------------------------------------------------------------------
# Step 3: Launch Spring Boot under custom JVM with provenance mode
# ---------------------------------------------------------------------------
echo ""
echo "=== Starting demo-buggy-app on port 8090 ==="
echo "  Provenance export: $JSONL_PATH"
echo ""

SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS="$JSONL_PATH" \
SOROUSH_BYTECODE_DUMP_DIR="$BYTECODE_DIR" \
  "$CUSTOM_JAVA" \
    -Xint \
    -jar "$JAR" \
    &
APP_PID=$!

# ---------------------------------------------------------------------------
# Step 4: Wait for Spring Boot to be ready
# ---------------------------------------------------------------------------
echo "  Waiting for server to start..."
for i in $(seq 1 30); do
  if curl -sf http://localhost:8090/bug/reflection > /dev/null 2>&1; then
    echo "  Server ready (pid $APP_PID)"
    break
  fi
  sleep 1
  if [[ $i -eq 30 ]]; then
    echo "ERROR: server did not start within 30 seconds"
    kill "$APP_PID" 2>/dev/null || true
    exit 1
  fi
done

# ---------------------------------------------------------------------------
# Step 5: Exercise all 4 bug endpoints
# ---------------------------------------------------------------------------
echo ""
echo "=== Exercising bug endpoints ==="
echo ""

echo "--- Bug 1: Reflection (DELIVERY → AuditHandler) ---"
curl -s "http://localhost:8090/bug/reflection?type=DELIVERY&payload=user@example.com" | python3 -m json.tool
echo ""

echo "--- Bug 2: Proxy (INTERNATIONAL order wrong tax) ---"
curl -s "http://localhost:8090/bug/proxy?orderId=ORD-001&amount=1000&type=INTERNATIONAL" | python3 -m json.tool
echo ""

echo "--- Bug 3: Polymorphic (GUEST gets admin permissions) ---"
curl -s "http://localhost:8090/bug/polymorphic?type=GUEST&userId=u789" | python3 -m json.tool
echo ""

echo "--- Bug 4: Lambda (CLEARANCE price uses basePrice) ---"
curl -s "http://localhost:8090/bug/lambda?name=Widget&category=CLEARANCE&basePrice=100&salePrice=60" | python3 -m json.tool
echo ""

# Also exercise other request types to populate the polymorphic graph
echo "--- Populating polymorphic graph (USER + ADMIN) ---"
curl -s "http://localhost:8090/bug/polymorphic?type=USER&userId=u001" > /dev/null
curl -s "http://localhost:8090/bug/polymorphic?type=ADMIN&userId=u002" > /dev/null
echo "  done (USER + ADMIN recorded)"
echo ""

# Also exercise other notification types to populate reflection graph
echo "--- Populating reflection graph (AUDIT + ALERT) ---"
curl -s "http://localhost:8090/bug/reflection?type=AUDIT&payload=order-123" > /dev/null
curl -s "http://localhost:8090/bug/reflection?type=ALERT&payload=high-load" > /dev/null
echo "  done (AUDIT + ALERT recorded)"
echo ""

# ---------------------------------------------------------------------------
# Step 6: Non-interactive mode → shut down and show JSONL stats
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--non-interactive" ]]; then
  echo "=== Shutting down ==="
  kill "$APP_PID" 2>/dev/null || true
  wait "$APP_PID" 2>/dev/null || true

  echo ""
  echo "=== Export summary ==="
  if [[ -f "$JSONL_PATH" ]]; then
    TOTAL=$(wc -l < "$JSONL_PATH")
    REFLECT=$(grep -c '"reflection_method_invoke"' "$JSONL_PATH" || echo 0)
    TARGETS=$(grep -c '"callsite_target_set"' "$JSONL_PATH" || echo 0)
    LAMBDA=$(grep -c '"hidden_class"' "$JSONL_PATH" || echo 0)
    echo "  JSONL path:              $JSONL_PATH"
    echo "  Total records:           $TOTAL"
    echo "  reflection_method_invoke: $REFLECT"
    echo "  callsite_target_set:     $TARGETS"
    echo "  hidden_class:            $LAMBDA"
  else
    echo "  WARNING: no JSONL file found — provenance export may not have flushed"
    echo "  (try running without --non-interactive so the server has time to flush)"
  fi
  echo ""
  echo "=== Done ==="
  exit 0
fi

# ---------------------------------------------------------------------------
# Interactive mode: leave server running for manual causality exploration
# ---------------------------------------------------------------------------
echo "=== Server is running (pid $APP_PID) ==="
echo ""
echo "Try the causality API (requires Runtime Truth UI running on port 5001):"
echo "  Run Runtime Truth UI: cd tools/manycore-ui && python app.py"
echo "  Then ingest the export:"
echo "    curl -X POST http://localhost:5001/runs/ingest \\"
echo "         -H 'Content-Type: application/json' \\"
echo "         -d '{\"label\":\"demo-buggy-app\",\"run_dir\":\"$EXPORT_DIR\"}'"
echo ""
echo "Press Ctrl-C to stop the demo server."
wait "$APP_PID"
