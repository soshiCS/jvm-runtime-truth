#!/usr/bin/env bash
# capture_bugs.sh — Run demo-real-bugs under the custom JVM, fire Bug1/Bug2/Bug3
# endpoints, and capture live runtime_targets.jsonl for evaluation.
#
# Usage:
#   ./capture_bugs.sh [run_dir]
#
# Outputs:
#   <run_dir>/runtime_targets.jsonl
#   <run_dir>/bug1_response.json
#   <run_dir>/bug2_response.json
#   <run_dir>/bug3_response.json
#   <run_dir>/stdout.txt
#   <run_dir>/stderr.txt

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JAR="$SCRIPT_DIR/target/demo-real-bugs-1.0.0.jar"

if [ ! -f "$JAR" ]; then
  echo "ERROR: JAR not found at $JAR — run 'mvn package -DskipTests' first" >&2
  exit 1
fi

REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
JAVA="$(ls -d "$REPO_ROOT/build"/*/jdk 2>/dev/null | head -1)/bin/java"

if [ ! -x "$JAVA" ]; then
  echo "ERROR: custom JVM not found at $JAVA" >&2
  exit 1
fi

RUN_DIR="${1:-/tmp/demo_real_bugs_live}"
mkdir -p "$RUN_DIR"

echo "[capture] run_dir = $RUN_DIR"
echo "[capture] jar     = $JAR"
echo "[capture] java    = $JAVA"

export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_RUNTIME_RECOVERY=1
export SOROUSH_TRACE_INDY=1
export SOROUSH_TRACE_REFLECTION=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1
export SOROUSH_EXPORT_RUNTIME_TARGETS="$RUN_DIR/runtime_targets.jsonl"
export SOROUSH_REWRITER_PHASE5_PREFIX="com/example/realbugs"
export SOROUSH_USER_PREFIXES="com/example/realbugs"

echo "[capture] starting Spring Boot on port 19080..."
"$JAVA" -Xverify:all -Xint \
  -jar "$JAR" \
  --server.port=19080 \
  --logging.level.root=WARN \
  --logging.level.com.example.realbugs=INFO \
  > "$RUN_DIR/stdout.txt" 2> "$RUN_DIR/stderr.txt" &
SERVER_PID=$!
echo "[capture] server PID = $SERVER_PID"

echo "[capture] waiting for server to be ready..."
MAX_WAIT=120
WAITED=0
while ! nc -z localhost 19080 2>/dev/null; do
  sleep 2
  WAITED=$((WAITED + 2))
  if [ $WAITED -ge $MAX_WAIT ]; then
    echo "[capture] ERROR: server did not start within ${MAX_WAIT}s" >&2
    kill $SERVER_PID 2>/dev/null || true
    exit 1
  fi
  echo "[capture]   ...still waiting (${WAITED}s)"
done
echo "[capture] server ready after ${WAITED}s"

# Bug 1 — @Transactional self-invocation bypass
echo "[capture] firing Bug1 endpoint..."
curl -s "http://localhost:19080/bug1/process?orderId=ORDER-001" \
  | python3 -m json.tool > "$RUN_DIR/bug1_response.json" 2>/dev/null || \
  curl -s "http://localhost:19080/bug1/process?orderId=ORDER-001" \
  > "$RUN_DIR/bug1_response.json"
echo "[capture] bug1 response saved"
sleep 1

# Bug 2 — @Retryable JDK proxy vs @Transactional CGLIB proxy
echo "[capture] firing Bug2 endpoint (will retry 3x — expect ~30ms delay)..."
curl -s "http://localhost:19080/bug2/inspect" \
  | python3 -m json.tool > "$RUN_DIR/bug2_response.json" 2>/dev/null || \
  curl -s "http://localhost:19080/bug2/inspect" \
  > "$RUN_DIR/bug2_response.json"
echo "[capture] bug2 response saved"
sleep 1

# Bug 3 — Hibernate proxy substitution
echo "[capture] firing Bug3 endpoint..."
curl -s "http://localhost:19080/bug3/inspect" \
  | python3 -m json.tool > "$RUN_DIR/bug3_response.json" 2>/dev/null || \
  curl -s "http://localhost:19080/bug3/inspect" \
  > "$RUN_DIR/bug3_response.json"
echo "[capture] bug3 response saved"
sleep 2

# Graceful shutdown
echo "[capture] shutting down server..."
kill -TERM $SERVER_PID 2>/dev/null || true
for i in $(seq 1 15); do
  sleep 1
  kill -0 $SERVER_PID 2>/dev/null || break
done
kill -KILL $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo "[capture] done"
echo ""

echo "=== Bug responses ==="
for bug in bug1 bug2 bug3; do
  echo "--- $bug ---"
  cat "$RUN_DIR/${bug}_response.json" 2>/dev/null || echo "(missing)"
  echo ""
done

echo "=== runtime_targets.jsonl record breakdown ==="
JSONL="$RUN_DIR/runtime_targets.jsonl"
if [ -f "$JSONL" ]; then
  echo "  Total lines: $(wc -l < "$JSONL")"
  python3 -c "
import json, sys, collections
counts = collections.Counter()
by_record = collections.defaultdict(list)
with open('$JSONL') as f:
    for line in f:
        line = line.strip()
        if not line: continue
        try:
            r = json.loads(line)
            rt = r.get('record', 'unknown')
            counts[rt] += 1
            by_record[rt].append(r)
        except: pass

print()
for k, v in sorted(counts.items(), key=lambda x: -x[1]):
    print(f'  {v:5d}  {k}')

# Show callsite_target entries for realbugs classes
print()
print('=== callsite_target entries (com.example.realbugs) ===')
for r in by_record.get('callsite_target', []):
    src = r.get('source_class','') + '.' + r.get('source_method','')
    tgt = r.get('target_class','') + '.' + r.get('target_method','')
    if 'realbugs' in src or 'realbugs' in tgt or 'Hibernate' in tgt or 'Proxy' in tgt or 'CGLIB' in tgt:
        print(f'  {src} -> {tgt}')

# Show generated_class entries
print()
print('=== generated_class entries ===')
for r in by_record.get('generated_class', []):
    print(f'  {r}')
"
else
  echo "  MISSING — JSONL file not created"
fi
