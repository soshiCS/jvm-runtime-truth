#!/usr/bin/env bash
# capture.sh — Run demo-agent-dispatch under RT, hit /run and /registry,
# capture JSONL, and show the reflection dispatch evidence.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JAR="$SCRIPT_DIR/target/demo-agent-dispatch-1.0.0.jar"
JAVA="/Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java"
RUN_DIR="${1:-/tmp/demo_agent_dispatch}"

[ -f "$JAR" ]  || { echo "ERROR: JAR missing" >&2; exit 1; }
[ -x "$JAVA" ] || { echo "ERROR: custom JVM missing" >&2; exit 1; }

mkdir -p "$RUN_DIR"

export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_RUNTIME_RECOVERY=1
export SOROUSH_TRACE_INDY=1
export SOROUSH_TRACE_REFLECTION=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1
export SOROUSH_EXPORT_RUNTIME_TARGETS="$RUN_DIR/runtime_targets.jsonl"
export SOROUSH_REWRITER_PHASE5_PREFIX="com/example/agentdemo"
export SOROUSH_USER_PREFIXES="com/example/agentdemo"

echo "[capture] starting server (port 19100)..."
"$JAVA" -Xverify:all -Xint \
  -jar "$JAR" \
  --server.port=19100 \
  --logging.level.root=WARN \
  --logging.level.com.example.agentdemo=INFO \
  > "$RUN_DIR/stdout.txt" 2> "$RUN_DIR/stderr.txt" &
SERVER_PID=$!

MAX_WAIT=120; WAITED=0
while ! nc -z localhost 19100 2>/dev/null; do
  sleep 2; WAITED=$((WAITED+2))
  [ $WAITED -ge $MAX_WAIT ] && { echo "TIMEOUT" >&2; kill $SERVER_PID 2>/dev/null; exit 1; }
done
echo "[capture] ready in ${WAITED}s"

curl -s "http://localhost:19100/registry" | python3 -m json.tool > "$RUN_DIR/registry.json"
curl -s "http://localhost:19100/run?task=onboard_user" | python3 -m json.tool > "$RUN_DIR/run.json"

sleep 2

kill -TERM $SERVER_PID 2>/dev/null || true
for i in $(seq 1 15); do sleep 1; kill -0 $SERVER_PID 2>/dev/null || break; done
kill -KILL $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== /registry ==="
cat "$RUN_DIR/registry.json"
echo ""
echo "=== /run ==="
cat "$RUN_DIR/run.json"
echo ""

python3 - << 'PYEOF'
import json, sys

JSONL = "/tmp/demo_agent_dispatch/runtime_targets.jsonl"
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

print(f"=== JSONL summary: {total} total records ===")
for k,v in sorted(by_type.items(), key=lambda x: -x[1]):
    print(f"  {v:5d}  {k}")

print()
print("=== THE KEY EVIDENCE: reflection dispatch to tool execute() ===")
seen = set()
for r in records:
    if r.get('record') not in ('callsite_target',): continue
    tm = r.get('target_method','')
    tc = r.get('target_class','')
    sc = r.get('source_class','')
    if tm != 'execute' or 'AgentTool' in tc: continue
    if 'agentdemo' not in tc and 'agentdemo' not in sc: continue
    bci = r.get('source_bci','?')
    key = (sc.split('/')[-1], r.get('source_method',''), tc.split('/')[-1], tm)
    if key in seen: continue
    seen.add(key)
    is_reflection = 'reflect' in sc.lower() or r.get('category','') in ('reflection_method_invoke',)
    tag = "[REFLECTION]" if is_reflection else "[direct]"
    print(f"  {tag} {sc.split('/')[-1]}.{r.get('source_method','')} -> {tc.split('/')[-1]}.{tm}")

print()
print("=== All callsite_target for agentdemo classes ===")
seen2 = set()
for r in records:
    if r.get('record') != 'callsite_target': continue
    sc = r.get('source_class','')
    sm = r.get('source_method','')
    tc = r.get('target_class','')
    tm = r.get('target_method','')
    if 'agentdemo' not in sc and 'agentdemo' not in tc: continue
    if 'Logger' in tc or 'logger' in tc or 'log' in tm: continue
    bci = r.get('source_bci','?')
    cat = r.get('category','')
    key = (sc,sm,tc,tm)
    if key in seen2: continue
    seen2.add(key)
    print(f"  [{cat}] {sc.split('/')[-1]}.{sm} -> {tc.split('/')[-1]}.{tm}")

PYEOF
