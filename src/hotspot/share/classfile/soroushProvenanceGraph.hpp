/*
 * Unified provenance graph v1 (Soroush custom JVM).
 *
 * Observational, append-only, fail-safe layer that ties together the JVM's
 * separate runtime provenance signals (ENTER/EXIT execution, runtime-generated
 * class recovery, invokedynamic linkage, reflection/MethodHandle linkage, final
 * bytecode capture) into one in-memory graph.
 *
 * Invariants:
 *  - Disabled unless SOROUSH_PROVENANCE_GRAPH=1; all entry points are no-ops
 *    when disabled.
 *  - NEVER affects verification, rewriting, or program behavior. On any internal
 *    failure (OOM, capacity exceeded) it silently drops data.
 *  - Thread-safe via a single internal lock; observational only.
 *
 * See SOROUSH_JVM_SPEC.md section "Unified provenance graph" for the schema and
 * runtime-identity rules.
 */
#ifndef SHARE_CLASSFILE_SOROUSHPROVENANCEGRAPH_HPP
#define SHARE_CLASSFILE_SOROUSHPROVENANCEGRAPH_HPP

#include <stdint.h>

extern "C" {

// Node types (stable integer ids appear in diagnostics).
enum SoroushGraphNodeType {
  SG_NODE_CLASS = 1,
  SG_NODE_METHOD = 2,
  SG_NODE_EXECUTION = 3,
  SG_NODE_GENERATED_CLASS = 4,
  SG_NODE_INDY_CALLSITE = 5,
  SG_NODE_BOOTSTRAP_METHOD = 6,
  SG_NODE_REFLECTION_INVOKE = 7,
  SG_NODE_METHODHANDLE_LINKAGE = 8,
  SG_NODE_BYTECODE_ARTIFACT = 9,
  // Async / cross-thread causality (v1 async extension).
  SG_NODE_ASYNC_TASK = 10,  // a submitted unit of async work (keyed by task identity)
  SG_NODE_THREAD = 11,      // a thread (keyed by stable OS thread id)
  SG_NODE_EXECUTOR = 12,    // an executor / thread-pool (keyed by executor identity)
  // MethodHandle / LambdaForm execution tracing.
  SG_NODE_MH_ADAPTER = 13,      // a MethodHandle in a chain (kind from class + LambdaForm)
  SG_NODE_LAMBDAFORM_EXEC = 14  // a LambdaForm (keyed by its vmentry class.method)
};

// Edge types.
enum SoroushGraphEdgeType {
  SG_EDGE_EXECUTES = 1,       // Execution      -> Method
  SG_EDGE_CALLS = 2,          // Execution      -> Execution (parent -> child)
  SG_EDGE_GENERATED_FROM = 3, // GeneratedClass -> Class
  SG_EDGE_CREATED_BY = 4,     // GeneratedClass -> IndyCallSite / Bootstrap / Reflection
  SG_EDGE_LINKS_TO = 5,       // IndyCallSite/MHLinkage/Reflection -> Method
  SG_EDGE_HAS_BYTECODE = 6,   // Class          -> BytecodeArtifact
  SG_EDGE_REWRITTEN_FROM = 7, // BytecodeArtifact(final) -> BytecodeArtifact(original)
  // Async / cross-thread causality (v1 async extension).
  SG_EDGE_SCHEDULES = 8,      // Execution -> AsyncTask / Execution -> Thread (scheduling cause)
  SG_EDGE_SUBMITTED_TO = 9,   // AsyncTask -> Executor (where the task was submitted)
  SG_EDGE_EXECUTES_ASYNC = 10,// AsyncTask/Thread -> Execution (worker frame that ran the work)
  SG_EDGE_CONTINUES_ON = 11,  // Execution(submitter) -> Execution(worker first frame)
  // MethodHandle / LambdaForm execution tracing.
  SG_EDGE_MH_INVOKES = 12,    // Execution -> MHAdapter (a call site invokes a MethodHandle)
  SG_EDGE_ADAPTS_TO = 13,     // MHAdapter -> MHAdapter (adapter wraps an inner target; reserved)
  SG_EDGE_BINDS_TO = 14,      // MHAdapter(bound) -> Method/MHAdapter (bound receiver/target; reserved)
  SG_EDGE_INVOKE_BASIC = 15,  // MHAdapter -> LambdaFormExec (invokeBasic into the MH's LambdaForm)
  SG_EDGE_RESOLVES_TO = 16    // MHAdapter/LambdaFormExec -> Method (final resolved target)
};

bool soroush_graph_enabled();

// EXACT ENTER event from the runtime execution graph — the SOLE execution-node
// creation path (method-token trace ABI; the legacy best-effort string path was
// removed). Creates an Execution node (+ Method/Class), an EXECUTES edge, and a
// CALLS edge from the parent execution; keys the Method node by the *exact*
// descriptor and uses the loader/hidden recorded for the instrumented method at
// rewrite time — **no "?" descriptor and no loader_id=0 fallback**. dotted_class is
// the trace form ('.'-separated). Returns the Method node id (0 if the graph is
// disabled, the node could not be created due to capacity/OOM, or descriptor is
// empty), so the caller can emit a [JVM GRAPH IDENTITY] diagnostic. When
// out_method_is_new is non-null it is set true iff the Method node was newly
// created on this call (so the caller can log the token->node mapping once per
// distinct method rather than per invocation). descriptor must be non-null/empty.
uint64_t soroush_graph_execution_exact(uint64_t exec_id, uint64_t parent_exec_id,
                                       const char* dotted_class, const char* method,
                                       const char* descriptor, uint64_t loader_id,
                                       int hidden, const void* thread_id,
                                       bool* out_method_is_new);

// ---------------------------------------------------------------------------
// Method-token registry (exact execution identity).
//
// During PHASE3/PHASE5 rewriting each instrumented method is assigned a stable
// 32-bit token that is baked into the injected bytecode (an Integer constant)
// and passed to System.soroushTraceEnter/Exit(int). The token resolves here to
// the method's exact identity. The registry is ALWAYS active when instrumentation
// runs (independent of SOROUSH_PROVENANCE_GRAPH) so the runtime/human trace can
// resolve tokens too. Append-only, immortal entries (strings never freed),
// guarded by an internal mutex, fail-safe (returns 0 / false on OOM or overflow).
// ---------------------------------------------------------------------------

// Register a method's exact identity and return its new token (>= 1). dotted_class
// is the '.'-separated class name (the graph normalizes to slashes). loader_id is
// the defining ClassLoaderData pointer (known at rewrite time). artifact_crc is the
// original (pre-rewrite) classfile crc, 0 if unknown. Returns 0 on failure (the
// caller must then NOT emit an unresolvable token — see the rewriter).
uint32_t soroush_method_token_register(const char* dotted_class, const char* method,
                                       const char* descriptor, uint64_t loader_id,
                                       int hidden, uint32_t artifact_crc);

// Resolve a token to its identity. Returns true and fills the (non-null) out
// params on success; false if the token is 0 / out of range. The returned string
// pointers are immortal (safe to use after return).
bool soroush_method_token_lookup(uint32_t token, const char** dotted_class,
                                 const char** method, const char** descriptor,
                                 uint64_t* loader_id, int* hidden, uint32_t* artifact_crc);

// Runtime-generated class recovery (klassFactory dump). Creates a
// GeneratedClass node, GENERATED_FROM its (recovered) Class, and CREATED_BY an
// IndyCallSite when indy_trace_id > 0.
void soroush_graph_generated_class(const char* internal_name, int hidden,
                                   const char* generated_by, const char* source_trigger,
                                   const char* provenance_kind, int indy_trace_id,
                                   uint32_t crc, uint64_t loader_id);

// invokedynamic linkage (systemDictionary bootstrap). Creates/enriches an
// IndyCallSite node (keyed by trace_id) + BootstrapMethod node + LINKS_TO.
void soroush_graph_indy(int trace_id, const char* caller, const char* descriptor,
                        const char* bootstrap);

// Capture exact source callsite identity for an invokedynamic linkage event.
// Called from systemDictionary.cpp after soroush_graph_indy(), while the
// interpreter frame is still valid.  src_method/src_desc are null when the
// invokedynamic executes in compiled code (frame_captured=false); src_bci is
// -1 in that case.  All string parameters are sg_strdup'd immediately; the
// caller's ResourceMark scope may end after this call returns.
// No-op when SOROUSH_PROVENANCE_GRAPH=1 is absent.  Fail-safe on OOM.
void soroush_graph_indy_callsite(int trace_id,
                                 const char* src_class,    // slash-form caller class
                                 uint64_t src_loader_id,   // caller class CLD pointer
                                 const char* src_method,   // caller method name (null=unknown)
                                 const char* src_desc,     // caller descriptor (null=unknown)
                                 int src_bci,              // BCI of invokedynamic (-1=unknown)
                                 int src_bss_index,        // bootstrap specifier CP index
                                 const char* indy_name,    // invokedynamic site name
                                 const char* indy_sig,     // invokedynamic site signature
                                 const char* bootstrap,    // bootstrap method identity
                                 const char* lmf_impl_cls, // LMF impl class  (null if not LMF)
                                 const char* lmf_impl_mth, // LMF impl method (null if not LMF)
                                 const char* lmf_impl_dsc, // LMF impl desc   (null if not LMF)
                                 bool frame_captured);     // false → compiled frame

// Exact source callsite record for reflection and MethodHandle invocations.
//
// Categories: "reflection_method_invoke", "reflection_constructor_newInstance",
//             "methodhandle_invokeExact", "methodhandle_invoke".
//
// source_exact=true means src_class/method/desc/bci/opcode_byte/cp_index were
// read from a live interpreter or compiled frame; false means BCI was not
// available and a diagnostic record is emitted instead of a callsite_target.
//
// target_exact=true means target_class/method/desc/loader_id are the resolved
// final target; false means only a partial target was visible at hook time and
// a diagnostic record is emitted instead of a callsite_target.
//
// opcode_byte is the raw Java opcode (e.g. 0xb6=invokevirtual) at src_bci.
// cp_index is the ORIGINAL constant pool index from the class file (-1 if the
// conversion from the CP-cache index failed or is unavailable).
//
// Deduplicates by (category, src_class, src_method, src_desc, src_bci); first
// invocation wins. Fail-safe on OOM. All strings are sg_strdup'd immediately.
// No-op when SOROUSH_PROVENANCE_GRAPH=1 is absent.
//
// Returns true if stored as a callsite_target record, false if diagnostic/dropped.
bool soroush_graph_generic_callsite(
    const char* category,
    const char* src_class, uint64_t src_loader_id,
    const char* src_method, const char* src_desc,
    int src_bci, int opcode_byte, int cp_index,
    const char* target_class, uint64_t target_loader_id,
    const char* target_method, const char* target_desc,
    bool source_exact, bool target_exact,
    const char* diagnostic_reason);

// reflection / MethodHandle linkage (methodHandles membername resolution).
// node_type is SG_NODE_REFLECTION_INVOKE or SG_NODE_METHODHANDLE_LINKAGE.
// loader_id is the ClassLoaderData pointer of the target method's holder (0 if
// unknown) so the target Class/Method nodes are loader-precise.
void soroush_graph_linkage(int node_type, const char* source,
                           const char* internal_class, const char* method,
                           const char* descriptor, uint64_t loader_id);

// Final executable bytecode capture (klassFactory). Creates a BytecodeArtifact
// node, HAS_BYTECODE from its (loader-specific) Class, and (when transformed)
// REWRITTEN_FROM the recorded original artifact for that class+loader. The
// artifact and the Class node it attaches to are both keyed by loader_id so
// LoaderA's bytecode never links to LoaderB's Class node.
void soroush_graph_bytecode(const char* internal_class, uint32_t crc, int size,
                            int transformed, const char* load_kind, uint64_t loader_id,
                            int hidden);

// ---------------------------------------------------------------------------
// Async / cross-thread causality (v1 async extension)
//
// All functions below are observational, fail-safe, and no-ops unless the graph
// is enabled. They are the integration spine for cross-thread provenance: a
// scheduling/submitting thread records context, and a worker thread that picks
// up the work links its execution back across the thread boundary.
// ---------------------------------------------------------------------------

// Intern (find-or-create) an Execution node by its runtime exec id. Returns the
// node id (0 if exec_id == 0 / graph off). Used so async edges can attach to an
// Execution that already exists (created at its ENTER) or be created lazily.
uint64_t soroush_graph_execution_node(uint64_t exec_id);

// Intern a Thread node keyed by stable OS thread id. Returns node id.
uint64_t soroush_graph_thread_node(uint64_t tid, const char* name, int is_daemon);

// Record that the execution running on a parent thread started a child thread
// (Thread.start causality). Creates parent/child Thread nodes, a SCHEDULES edge
// from the parent Execution (if any) to the child Thread, and returns the child
// Thread node id (so the worker side can later emit EXECUTES_ASYNC/CONTINUES_ON).
uint64_t soroush_graph_thread_start(uint64_t parent_tid, const char* parent_name,
                                    uint64_t parent_exec_id,
                                    uint64_t child_tid, const char* child_name,
                                    int child_is_daemon);

// Executor-based submission: a task with identity task_hash was submitted by the
// execution submitter_exec_id (on submitter_tid) to the executor executor_hash.
// Creates an AsyncTask node, an Executor node, a SCHEDULES edge (submitter
// Execution -> AsyncTask) and a SUBMITTED_TO edge (AsyncTask -> Executor), and
// remembers the submitter context keyed by task_hash for the later run.
void soroush_graph_async_submit(uint32_t task_hash, uint32_t executor_hash,
                                uint64_t submitter_exec_id, uint64_t submitter_tid,
                                const char* executor_label);

// Executor-based execution: a worker thread (worker_tid) is about to run the
// task identified by task_hash. Resolves the remembered submitter context and
// returns (via out params) the submitter Execution's exec id and the AsyncTask
// node id, so the caller can stash them as the worker's pending async context
// (consumed at the worker's next ENTER). Returns true if a submitter was found.
bool soroush_graph_async_run(uint32_t task_hash, uint64_t worker_tid,
                             uint64_t* out_submitter_exec_id,
                             uint64_t* out_async_task_node);

// At the worker's first instrumented ENTER after a handoff: link the pending
// async context to the freshly-created worker Execution (exec id). Emits a
// CONTINUES_ON edge (submitter Execution -> worker Execution) and, when
// owner_node != 0, an EXECUTES_ASYNC edge (AsyncTask/Thread -> worker Execution).
void soroush_graph_async_continue(uint64_t cause_exec_id, uint64_t owner_node,
                                  uint64_t worker_exec_id);

// ---------------------------------------------------------------------------
// MethodHandle / LambdaForm execution tracing
// ---------------------------------------------------------------------------

// Records the structure of a MethodHandle observed at an invocation linkage
// site (read-only walk; see methodHandles/linkResolver). Creates a MHAdapter
// node (kind = MH class + LambdaForm), a LambdaFormExec node (lf_name), and
// edges: MH_INVOKES (caller Execution -> MHAdapter, when caller_exec_id != 0),
// INVOKE_BASIC (MHAdapter -> LambdaFormExec), and RESOLVES_TO (MHAdapter ->
// final Method) when a final target is known. Observational; no-op if graph off.
void soroush_graph_mh_chain(uint64_t caller_exec_id, const char* mh_kind,
                            const char* type_desc, const char* lf_name,
                            const char* final_class, const char* final_method,
                            const char* final_desc, int is_bound,
                            uint64_t final_loader_id);

// Tags an actually-executing MethodHandle/LambdaForm-internal method (observed
// at its instrumented ENTER) as a LambdaFormExec node and links the runtime
// Execution to it (INVOKE_BASIC). This is how the live adapter chain (the
// LambdaForm bytecode executing) is surfaced as first-class MH nodes. No-op if
// graph off. lf_name is the executing "<class>.<method>".
void soroush_graph_lambdaform_exec(uint64_t exec_id, const char* lf_name);

// Implemented in jvm.cpp: the calling thread's current top-of-stack execution
// id from the runtime execution graph (0 if none / runtime graph off). Lets the
// MH tracer attach an MH_INVOKES edge to the invoking execution.
uint64_t soroush_runtime_current_exec_id();

// Per-type node/edge counts; called once at VM shutdown.
void soroush_graph_dump_summary();

// One target entry passed to soroush_graph_target_set_callsite().
// All string pointers are transient (ResourceMark-allocated or static);
// the callee sg_strdup's them immediately.
struct SgMhTargetEntry {
  const char* klass;       // slash-form class name (null if unknown)
  uint64_t    loader_id;   // CLD pointer (0 if unknown)
  const char* method;      // method name (null if unknown)
  const char* descriptor;  // JVM descriptor (null if unknown)
  const char* role;        // semantic role: "direct_target", "test",
                           //   "true_target", "false_target",
                           //   "try_target", "handler"
  bool        valid;       // true = klass/loader/method/descriptor all exact
};

// Record a multi-target MH callsite (guardWithTest, catchException, …).
// adapter_shape: "GWT" | "GWC" (used verbatim in the export record).
// adapter_class: outermost MH class name (slash-form, may be null).
// lf_kind: LambdaForm.Kind.name() string (ResourceMark-allocated, may be null).
// aux_info: exception class name for GWC; null for GWT.
// source_ / opcode / cp fields: same semantics as soroush_graph_generic_callsite.
// targets[0..n_targets-1]: semantic targets in role order.
// Deduplicates by (category, src_class, src_method, src_desc, src_bci).
// Returns true if stored; false if deduped or dropped (OOM / graph off).
// No-op when SOROUSH_PROVENANCE_GRAPH=1 is absent.
bool soroush_graph_target_set_callsite(
    const char* category, const char* adapter_shape,
    const char* adapter_class, const char* lf_kind,
    const char* aux_info,
    const char* src_class, uint64_t src_loader_id,
    const char* src_method, const char* src_desc,
    int src_bci, int opcode_byte, int cp_index,
    const SgMhTargetEntry* targets, int n_targets);

// One node entry passed to soroush_graph_adapter_graph_callsite().
// All string pointers are transient (ResourceMark-allocated or static);
// the callee sg_strdup's them immediately.
struct SgAdapterNodeEntry {
  int         id;          // sequential from 0 (argL-index order)
  const char* role;        // "adapted_target", "primary_target", "secondary_component", …
  const char* from_desc;   // outer adapter type descriptor (type_conversion only; null otherwise)
  const char* to_desc;     // inner component's own descriptor (null if unavailable)
  const char* klass;       // exact target class slash-form (null if not exact)
  uint64_t    loader_id;   // CLD pointer (0 if not exact)
  const char* method;      // exact target method name (null if not exact)
  const char* descriptor;  // exact target descriptor (null if not exact)
  bool        exact;       // true = klass/loader_id/method/descriptor all exact
};

// Record an adapter graph callsite (asType, filterArguments, foldArguments,
// tryFinally, asCollector, etc.).
// adapter_class: outermost BMH species class name (slash-form, may be null).
// adapter_kind: structural kind ("type_conversion", "dual_target", "multi_target",
//               "bound_data", "try_finally", "collector", "loop", "table_switch").
// lf_kind: LambdaForm.Kind.name() string (may be "GENERIC"; null handled).
// outer_desc: MH type descriptor as presented to the caller (null if unavailable).
// source_/opcode/cp fields: same semantics as soroush_graph_generic_callsite.
// nodes[0..n_nodes-1]: adapter component nodes in argL-index order.
// all_exact: true iff every node with a target is exactly resolved.
// Deduplicates by (category, src_class, src_method, src_desc, src_bci).
// Returns true if stored; false if deduped or dropped (OOM / graph off).
// No-op when SOROUSH_PROVENANCE_GRAPH=1 is absent.
bool soroush_graph_adapter_graph_callsite(
    const char* category,
    const char* adapter_class, const char* adapter_kind, const char* lf_kind,
    const char* outer_desc,
    const char* src_class, uint64_t src_loader_id,
    const char* src_method, const char* src_desc,
    int src_bci, int opcode_byte, int cp_index,
    const SgAdapterNodeEntry* nodes, int n_nodes,
    bool all_exact);

// Semantic JSONL export of runtime target revelation records.
// Called at VM shutdown from before_exit when SOROUSH_EXPORT_RUNTIME_TARGETS
// is set. path must be non-null. Fail-safe: errors are reported to stderr;
// the VM is never aborted.
void soroush_graph_export_runtime_targets(const char* path);

} // extern "C"

#endif // SHARE_CLASSFILE_SOROUSHPROVENANCEGRAPH_HPP
