#!/usr/bin/env bash
# capture.sh — Run demo-spring-proxy-bypass under Runtime Truth, hit /compare,
# capture runtime_targets.jsonl, and print the key callsite_target evidence.
#
# Usage: ./capture.sh [run_dir]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JAR="$SCRIPT_DIR/target/demo-spring-proxy-bypass-1.0.0.jar"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
JAVA="$(ls -d "$REPO_ROOT/build"/*/jdk 2>/dev/null | head -1)/bin/java"
RUN_DIR="${1:-/tmp/demo_proxy_bypass}"

[ -f "$JAR" ]  || { echo "ERROR: JAR missing — run mvn package -DskipTests" >&2; exit 1; }
[ -x "$JAVA" ] || { echo "ERROR: custom JVM missing at $JAVA" >&2; exit 1; }

mkdir -p "$RUN_DIR"
echo "[capture] run_dir = $RUN_DIR"

export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_RUNTIME_RECOVERY=1
export SOROUSH_TRACE_INDY=1
export SOROUSH_TRACE_REFLECTION=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1
export SOROUSH_EXPORT_RUNTIME_TARGETS="$RUN_DIR/runtime_targets.jsonl"
export SOROUSH_REWRITER_PHASE5_PREFIX="com/example/proxydemo"
export SOROUSH_USER_PREFIXES="com/example/proxydemo"

echo "[capture] starting server..."
"$JAVA" -Xverify:all -Xint \
  -jar "$JAR" \
  --server.port=19090 \
  --logging.level.root=WARN \
  --logging.level.com.example.proxydemo=INFO \
  > "$RUN_DIR/stdout.txt" 2> "$RUN_DIR/stderr.txt" &
SERVER_PID=$!
echo "[capture] PID = $SERVER_PID"

echo "[capture] waiting for port 19090..."
MAX_WAIT=120; WAITED=0
while ! nc -z localhost 19090 2>/dev/null; do
  sleep 2; WAITED=$((WAITED+2))
  [ $WAITED -ge $MAX_WAIT ] && { echo "TIMEOUT" >&2; kill $SERVER_PID 2>/dev/null; exit 1; }
done
echo "[capture] ready in ${WAITED}s"

echo "[capture] hitting /compare..."
curl -s "http://localhost:19090/compare" \
  | python3 -m json.tool > "$RUN_DIR/compare_response.json" 2>/dev/null || \
  curl -s "http://localhost:19090/compare" > "$RUN_DIR/compare_response.json"

sleep 2

echo "[capture] shutting down..."
kill -TERM $SERVER_PID 2>/dev/null || true
for i in $(seq 1 15); do sleep 1; kill -0 $SERVER_PID 2>/dev/null || break; done
kill -KILL $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== /compare response ==="
cat "$RUN_DIR/compare_response.json"
echo ""

echo "=== Runtime Truth: callsite_target for saveAuditEntry ==="
python3 - << 'PYEOF'
import json, sys

JSONL = "/tmp/demo_proxy_bypass/runtime_targets.jsonl"
records = []
with open(JSONL) as f:
    for line in f:
        line = line.strip()
        if not line: continue
        try: records.append(json.loads(line))
        except: pass

total = len(records)
by_type = {}
for r in records:
    t = r.get('record','?')
    by_type[t] = by_type.get(t,0) + 1

print(f"Total records: {total}")
for k,v in sorted(by_type.items(), key=lambda x: -x[1]):
    print(f"  {v:5d}  {k}")

print()
print("=== THE KEY EVIDENCE: callsites targeting saveAuditEntry ===")
seen = set()
for r in records:
    if r.get('record') != 'callsite_target': continue
    tm = r.get('target_method','')
    if tm != 'saveAuditEntry': continue
    sc = r.get('source_class','').split('/')[-1]
    sm = r.get('source_method','')
    tc = r.get('target_class','').split('/')[-1]
    bci = r.get('source_bci','?')
    key = (sc, sm, tc, tm)
    if key in seen: continue
    seen.add(key)
    proxy = "CGLIB proxy" if "SpringCGLIB" in tc else "RAW CLASS (proxy bypassed!)"
    print(f"  [{bci}] {sc}.{sm} --> {tc}.{tm}   [{proxy}]")

print()
print("=== Generated proxy classes (realbugs/proxydemo) ===")
for r in records:
    if r.get('record') != 'generated_class': continue
    cls = r.get('class','')
    if 'proxydemo' in cls or 'SpringCGLIB' in cls:
        print(f"  {cls}  ({r.get('generated_by','?')})")
PYEOF

echo ""
echo "Run directory: $RUN_DIR"
