/*
 * Unified provenance graph v1 backend. See soroushProvenanceGraph.hpp.
 *
 * Simple append-only in-memory graph: node array + edge array + an
 * open-addressing intern table (identity key -> node). A single mutex guards
 * all mutation. Everything is best-effort: capacity overflow or OOM silently
 * drops data so the graph can NEVER perturb execution / verification.
 */
#include "precompiled.hpp"
#include "classfile/soroushProvenanceGraph.hpp"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const long SG_MAX_NODES = 1000000;
static const long SG_MAX_EDGES = 2000000;

struct SgNode {
  uint64_t id;        // 1-based; equals (index + 1)
  int      type;
  char*    key;       // owned identity key
  char*    label;     // owned human label (may be null)
  uint64_t loader_id;
  uint32_t crc;
  int      flags;
};

struct SgEdge {
  uint64_t from;
  uint64_t to;
  int      type;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static SgNode* g_nodes = nullptr;
static long    g_node_count = 0;
static long    g_node_cap = 0;

static SgEdge* g_edges = nullptr;
static long    g_edge_count = 0;
static long    g_edge_cap = 0;

// Intern table: stores (node index + 1); 0 means empty slot.
static int32_t* g_map = nullptr;
static long     g_map_size = 0;
static long     g_map_used = 0;

static volatile bool g_overflow_logged = false;

// Class-name -> first-seen loader side index, used ONLY to emit a
// loader-divergence NOTE the first time a normalized class name is observed
// under a second class loader. Direct-mapped + fail-safe: a slot collision can
// at worst miss or duplicate a (purely diagnostic) NOTE, never affecting graph
// identity. Lazily allocated; guarded by g_lock like everything else.
struct SgNameSeen {
  uint64_t name_hash;   // sg_hash of normalized (slash) class name; 0 = empty
  uint64_t loader_id;   // first loader id seen for this name
};
static const long SG_NAME_TABLE = 1 << 16; // 65536 slots
static SgNameSeen* g_name_seen = nullptr;

// ---------------------------------------------------------------------------
// Method-token registry (exact execution identity). See the header. Append-only,
// immortal entries, guarded by its OWN mutex (separate from g_lock; registration
// happens at class-load, lookup at execution). Indexed directly by token
// (token N lives at g_tokens[N]; slot 0 unused / reserved for "invalid"). Always
// active when instrumentation runs, independent of SOROUSH_PROVENANCE_GRAPH.
// ---------------------------------------------------------------------------
static char* sg_strdup(const char* s); // defined below
static void  sg_internalize(char* dst, size_t dst_len, const char* name); // defined below

struct SgMethodToken {
  char*    dotted_class; // owned, immortal
  char*    method;       // owned, immortal
  char*    descriptor;   // owned, immortal
  uint64_t loader_id;
  uint32_t artifact_crc;
  int      hidden;
};

static const uint32_t SG_TOKEN_MAX = 8u * 1024u * 1024u; // 8M instrumented methods cap
static pthread_mutex_t g_token_lock = PTHREAD_MUTEX_INITIALIZER;
static SgMethodToken* g_tokens = nullptr;
static uint32_t       g_token_count = 0;   // highest assigned token (== #registered)
static uint32_t       g_token_cap = 0;     // allocated slots (index space)
static volatile bool  g_token_overflow_logged = false;

// ---------------------------------------------------------------------------
// InvokeDynamic callsite side table.
//
// Stores exact source callsite identity (caller class/method/descriptor/BCI/
// CP-index) captured from the interpreter frame at bootstrap linkage time.
// Indexed by indy trace_id (1-based, same key space as IndyCallSite nodes).
// Active when SOROUSH_PROVENANCE_GRAPH=1; soroush_graph_indy_callsite()
// returns early otherwise.  Fail-safe: OOM or overflow silently drops entries.
// Strings are immortal (sg_strdup'd; never freed after publication).
// Guarded by g_indy_site_lock.
// ---------------------------------------------------------------------------
struct SgIndySite {
  bool     valid;
  // Source callsite identity (from the interpreter frame at linkage time)
  char*    src_class;      // slash-form caller class; null = not captured
  uint64_t src_loader_id;  // caller class CLD pointer
  char*    src_method;     // caller method name; null = not captured
  char*    src_desc;       // caller method descriptor; null = not captured
  int      src_bci;        // BCI of the invokedynamic instruction; -1 = unknown
  int      src_bss_index;  // bootstrap specifier CP index (bss_index)
  bool     frame_captured; // true=interpreted frame read; false=compiled frame
  // InvokeDynamic site identity
  char*    indy_name;      // indy call-site name (e.g. "apply")
  char*    indy_sig;       // indy call-site signature (e.g. "()Ljava/util/function/Function;")
  char*    bootstrap;      // bootstrap method identity string
  // LambdaMetafactory implementation method; all null if BSM != LambdaMetafactory
  char*    lmf_impl_cls;
  char*    lmf_impl_mth;
  char*    lmf_impl_dsc;
};

static const uint32_t  SG_INDY_SITE_MAX = 1u << 20; // 1M indy sites cap
static pthread_mutex_t g_indy_site_lock = PTHREAD_MUTEX_INITIALIZER;
static SgIndySite*     g_indy_sites     = nullptr;
static uint32_t        g_indy_site_count = 0; // highest registered trace_id (1-based)
static uint32_t        g_indy_site_cap   = 0; // allocated slots
static volatile bool   g_indy_site_overflow_logged = false;

// ---------------------------------------------------------------------------
// Generic callsite side table: reflection / MethodHandle invocation sites.
// Keyed by (category + src_class + src_method + src_desc + src_bci).
// Hash-chained, dedup on first-in-wins per unique key. Fail-safe on OOM.
// ---------------------------------------------------------------------------
struct SgGenCallsite {
  char*    category;        // "reflection_method_invoke" etc.
  char*    src_class;       // slash-form; null if not captured
  uint64_t src_loader_id;
  char*    src_method;      // null if not captured
  char*    src_desc;        // null if not captured
  int      src_bci;         // -1 if not captured
  int      opcode_byte;     // raw Java opcode (0 if unknown)
  int      cp_index;        // original CP index (-1 if unavailable)
  char*    target_class;    // slash-form; null if not captured
  uint64_t target_loader_id;
  char*    target_method;   // null if not captured
  char*    target_desc;     // null if not captured
  bool     exact;           // true → callsite_target; false → diagnostic
  char*    diag_reason;     // non-null when !exact
  uint32_t hash;
  struct SgGenCallsite* next; // hash chain
};
#define SG_GEN_BUCKETS 512u
static struct SgGenCallsite* g_gen_buckets[SG_GEN_BUCKETS];
static pthread_mutex_t       g_gen_lock   = PTHREAD_MUTEX_INITIALIZER;
static uint32_t              g_gen_count  = 0;

// ---------------------------------------------------------------------------
// Multi-target callsite side table: guardWithTest / catchException MH sites.
// Keyed by (category + src_class + src_method + src_desc + src_bci).
// Stores all semantic targets with their roles. Fail-safe on OOM.
// ---------------------------------------------------------------------------
struct SgTsTarget {
  char*    klass;
  uint64_t loader_id;
  char*    method;
  char*    descriptor;
  char*    role;
  bool     valid;
};

struct SgTsCallsite {
  char*     category;
  char*     adapter_shape;  // "GWT" or "GWC"
  char*     adapter_class;
  char*     lf_kind;
  char*     aux_info;       // GWC exception class name; null for GWT
  char*     semantic_op;    // "guard_with_test" | "catch_exception"
  bool      staticizable;
  int       n_staticization_blockers;
  char      staticization_blockers[4][64];
  char*     src_class;
  uint64_t  src_loader_id;
  char*     src_method;
  char*     src_desc;
  int       src_bci;
  int       opcode_byte;
  int       cp_index;
  int       n_targets;
  SgTsTarget targets[4];
  uint32_t  hash;
  struct SgTsCallsite* next;
};

#define SG_TS_BUCKETS 512u
static struct SgTsCallsite* g_ts_buckets[SG_TS_BUCKETS];
static pthread_mutex_t      g_ts_lock   = PTHREAD_MUTEX_INITIALIZER;
static uint32_t             g_ts_count  = 0;

// ---- Adapter graph callsite side table ----
// Populated by soroush_graph_adapter_graph_callsite() from linkResolver.cpp.
// Each entry is a GENERIC or named-kind BMH callsite whose bound MH components
// were extracted (asType, filterArguments, foldArguments, tryFinally, etc.).

#define SG_AG_BUCKETS    256u
#define SG_AG_MAX_NODES   16
#define SG_AG_MAX_EDGES   16

struct SgAgNode {
  int      id;
  char*    role;
  char*    from_desc;
  char*    to_desc;
  char*    klass;
  uint64_t loader_id;
  char*    method;
  char*    descriptor;
  char*    node_adapter_class;
  char*    node_classification;
  char*    exact_false_reason;
  char*    semantic_op;   // null if not provable
  char*    from_type;     // null unless semantic_op is a type-pair op
  char*    to_type;       // null unless semantic_op is a type-pair op
  bool     exact;
};

struct SgAgEdge {
  int   from_id;
  int   to_id;
  char  kind[32];
};

struct SgAgCallsite {
  char*    category;
  char*    adapter_class;
  char*    adapter_kind;
  char*    lf_kind;
  char*    outer_desc;
  char*    src_class;
  uint64_t src_loader_id;
  char*    src_method;
  char*    src_desc;
  int      src_bci;
  int      opcode_byte;
  int      cp_index;
  int      n_nodes;
  SgAgNode nodes[SG_AG_MAX_NODES];
  int      n_edges;
  SgAgEdge edges[SG_AG_MAX_EDGES];
  bool     all_exact;
  bool     staticizable;
  char*    staticization_blockers[8];
  int      n_staticization_blockers;
  uint32_t hash;
  struct SgAgCallsite* next;
};

static struct SgAgCallsite* g_ag_buckets[SG_AG_BUCKETS];
static pthread_mutex_t      g_ag_lock  = PTHREAD_MUTEX_INITIALIZER;
static uint32_t             g_ag_count = 0;

uint32_t soroush_method_token_register(const char* dotted_class, const char* method,
                                       const char* descriptor, uint64_t loader_id,
                                       int hidden, uint32_t artifact_crc) {
  if (dotted_class == nullptr) dotted_class = "<unknown>";
  if (method == nullptr) method = "<unknown>";
  if (descriptor == nullptr) descriptor = "()V";
  pthread_mutex_lock(&g_token_lock);

  if (g_token_count + 1 >= SG_TOKEN_MAX) {
    if (!g_token_overflow_logged) {
      fprintf(stderr, "[JVM TRACE METHOD] token registry full (%u); further methods unresolved\n",
              (unsigned)SG_TOKEN_MAX);
      g_token_overflow_logged = true;
    }
    pthread_mutex_unlock(&g_token_lock);
    return 0;
  }
  uint32_t token = g_token_count + 1; // 1-based
  if (token >= g_token_cap) {
    uint32_t new_cap = g_token_cap == 0 ? 4096 : g_token_cap * 2;
    while (token >= new_cap) new_cap *= 2;
    SgMethodToken* nt = (SgMethodToken*)realloc(g_tokens, (size_t)new_cap * sizeof(SgMethodToken));
    if (nt == nullptr) {
      pthread_mutex_unlock(&g_token_lock);
      return 0; // fail-safe: caller emits no token for this method
    }
    g_tokens = nt;
    g_token_cap = new_cap;
  }
  SgMethodToken* e = &g_tokens[token];
  e->dotted_class = sg_strdup(dotted_class);
  e->method = sg_strdup(method);
  e->descriptor = sg_strdup(descriptor);
  if (e->dotted_class == nullptr || e->method == nullptr || e->descriptor == nullptr) {
    free(e->dotted_class); free(e->method); free(e->descriptor);
    e->dotted_class = e->method = e->descriptor = nullptr;
    pthread_mutex_unlock(&g_token_lock);
    return 0;
  }
  e->loader_id = loader_id;
  e->artifact_crc = artifact_crc;
  e->hidden = hidden ? 1 : 0;
  g_token_count = token; // publish last (token <= g_token_count means fully written)
  pthread_mutex_unlock(&g_token_lock);

  fprintf(stderr, "[JVM TRACE METHOD] register token=%u class=%s method=%s desc=%s loader="
          UINT64_FORMAT " hidden=%d crc=%08x\n", (unsigned)token, dotted_class, method,
          descriptor, (uint64_t)loader_id, hidden ? 1 : 0, artifact_crc);
  return token;
}

bool soroush_method_token_lookup(uint32_t token, const char** dotted_class,
                                 const char** method, const char** descriptor,
                                 uint64_t* loader_id, int* hidden, uint32_t* artifact_crc) {
  if (token == 0) return false;
  bool found = false;
  pthread_mutex_lock(&g_token_lock);
  if (token <= g_token_count && g_tokens != nullptr) {
    SgMethodToken* e = &g_tokens[token];
    // Immortal strings: safe to hand the pointers out past the unlock.
    if (dotted_class) *dotted_class = e->dotted_class;
    if (method) *method = e->method;
    if (descriptor) *descriptor = e->descriptor;
    if (loader_id) *loader_id = e->loader_id;
    if (hidden) *hidden = e->hidden;
    if (artifact_crc) *artifact_crc = e->artifact_crc;
    found = true;
  }
  pthread_mutex_unlock(&g_token_lock);
  return found;
}

void soroush_graph_indy_callsite(int trace_id,
                                 const char* src_class, uint64_t src_loader_id,
                                 const char* src_method, const char* src_desc,
                                 int src_bci, int src_bss_index,
                                 const char* indy_name, const char* indy_sig,
                                 const char* bootstrap,
                                 const char* lmf_impl_cls, const char* lmf_impl_mth,
                                 const char* lmf_impl_dsc,
                                 bool frame_captured) {
  if (!soroush_graph_enabled() || trace_id <= 0) return;
  uint32_t tid = (uint32_t)trace_id;

  pthread_mutex_lock(&g_indy_site_lock);

  if (tid >= SG_INDY_SITE_MAX) {
    if (!g_indy_site_overflow_logged) {
      fprintf(stderr, "[JVM CALLSITE] indy site table full (%u); further sites not tracked\n",
              (unsigned)SG_INDY_SITE_MAX);
      g_indy_site_overflow_logged = true;
    }
    pthread_mutex_unlock(&g_indy_site_lock);
    return;
  }

  if (tid >= g_indy_site_cap) {
    uint32_t new_cap = g_indy_site_cap == 0 ? 256u : g_indy_site_cap * 2u;
    while (tid >= new_cap) new_cap *= 2u;
    if (new_cap > SG_INDY_SITE_MAX) new_cap = SG_INDY_SITE_MAX;
    SgIndySite* nt = (SgIndySite*)realloc(g_indy_sites,
                                          (size_t)new_cap * sizeof(SgIndySite));
    if (nt == nullptr) {
      pthread_mutex_unlock(&g_indy_site_lock);
      return; // OOM: silently drop this site
    }
    memset(nt + g_indy_site_cap, 0,
           (new_cap - g_indy_site_cap) * sizeof(SgIndySite));
    g_indy_sites    = nt;
    g_indy_site_cap = new_cap;
  }

  SgIndySite* s = &g_indy_sites[tid];
  if (s->valid) {
    // Already registered.  InvokeDynamic sites are linked exactly once per
    // trace_id; reaching here a second time is a safeguard against races.
    pthread_mutex_unlock(&g_indy_site_lock);
    return;
  }

  // Normalize src_class to slash-form (mirrors how the graph stores class names).
  char norm_src[1024] = "";
  if (src_class != nullptr) sg_internalize(norm_src, sizeof(norm_src), src_class);

  s->valid          = true;
  s->src_class      = norm_src[0] ? sg_strdup(norm_src) : nullptr;
  s->src_loader_id  = src_loader_id;
  s->src_method     = sg_strdup(src_method);
  s->src_desc       = sg_strdup(src_desc);
  s->src_bci        = src_bci;
  s->src_bss_index  = src_bss_index;
  s->frame_captured = frame_captured;
  s->indy_name      = sg_strdup(indy_name);
  s->indy_sig       = sg_strdup(indy_sig);
  s->bootstrap      = sg_strdup(bootstrap);
  s->lmf_impl_cls   = sg_strdup(lmf_impl_cls);
  s->lmf_impl_mth   = sg_strdup(lmf_impl_mth);
  s->lmf_impl_dsc   = sg_strdup(lmf_impl_dsc);

  if (g_indy_site_count < tid) g_indy_site_count = tid;
  pthread_mutex_unlock(&g_indy_site_lock);

  fprintf(stderr,
          "[JVM CALLSITE] trace_id=%d src=%s.%s%s bci=%d bss_index=%d"
          " indy=%s%s lmf_impl=%s.%s%s frame=%s\n",
          trace_id,
          s->src_class  ? s->src_class  : "?",
          s->src_method ? s->src_method : "?",
          s->src_desc   ? s->src_desc   : "",
          src_bci, src_bss_index,
          s->indy_name ? s->indy_name : "?",
          s->indy_sig  ? s->indy_sig  : "",
          s->lmf_impl_cls ? s->lmf_impl_cls : "",
          s->lmf_impl_mth ? s->lmf_impl_mth : "",
          s->lmf_impl_dsc ? s->lmf_impl_dsc : "",
          frame_captured ? "exact" : "compiled(bci=unknown)");
}

// djb2 string hash — used for the generic callsite dedup table.
static uint32_t sg_hash_str(const char* s) {
  uint32_t h = 5381u;
  if (s != nullptr) {
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
      h = (h << 5u) + h + (uint32_t)*p;
  }
  return h;
}

bool soroush_graph_generic_callsite(
    const char* category,
    const char* src_class, uint64_t src_loader_id,
    const char* src_method, const char* src_desc,
    int src_bci, int opcode_byte, int cp_index,
    const char* target_class, uint64_t target_loader_id,
    const char* target_method, const char* target_desc,
    bool source_exact, bool target_exact,
    const char* diagnostic_reason) {
  if (!soroush_graph_enabled()) return false;

  // Dedup key: category + src_class + src_method + src_desc + src_bci.
  // Dedup key: (src_class, src_method, src_desc, src_bci) — category excluded
  // so that invokeExact and invoke firings for the same BCI collapse to first-in.
  uint32_t h = sg_hash_str(src_class)
             ^ sg_hash_str(src_method) ^ sg_hash_str(src_desc)
             ^ (uint32_t)(unsigned)src_bci;
  uint32_t bucket = h & (SG_GEN_BUCKETS - 1u);

  bool exact = source_exact && target_exact;

  pthread_mutex_lock(&g_gen_lock);

  // Dedup check: walk the chain.
  // Prefer-exact rule: if the existing entry is a diagnostic (!exact) and the
  // incoming entry is exact, upgrade the existing entry in place so that the
  // diagnostic does not suppress a later-arriving target.
  for (SgGenCallsite* c = g_gen_buckets[bucket]; c != nullptr; c = c->next) {
    if (c->hash == h
        && src_bci == c->src_bci
        && (src_class == c->src_class ||
            (src_class && c->src_class && strcmp(src_class, c->src_class) == 0))
        && (src_method == c->src_method ||
            (src_method && c->src_method && strcmp(src_method, c->src_method) == 0))
        && (src_desc == c->src_desc ||
            (src_desc && c->src_desc && strcmp(src_desc, c->src_desc) == 0))) {
      if (exact && !c->exact) {
        // Upgrade diagnostic → exact in place.
        if (c->diag_reason) { free(c->diag_reason); c->diag_reason = nullptr; }
        if (c->target_class)  { free(c->target_class);  }
        if (c->target_method) { free(c->target_method); }
        if (c->target_desc)   { free(c->target_desc);   }
        c->target_class      = sg_strdup(target_class);
        c->target_loader_id  = target_loader_id;
        c->target_method     = sg_strdup(target_method);
        c->target_desc       = sg_strdup(target_desc);
        c->exact = true;
      }
      pthread_mutex_unlock(&g_gen_lock);
      return c->exact; // first-in-wins or upgraded-to-exact
    }
  }

  // OOM fail-safe: allocate the node.
  SgGenCallsite* n = (SgGenCallsite*)malloc(sizeof(SgGenCallsite));
  if (n == nullptr) {
    pthread_mutex_unlock(&g_gen_lock);
    return false;
  }
  memset(n, 0, sizeof(*n));
  n->category        = sg_strdup(category);
  n->src_class       = sg_strdup(src_class);
  n->src_loader_id   = src_loader_id;
  n->src_method      = sg_strdup(src_method);
  n->src_desc        = sg_strdup(src_desc);
  n->src_bci         = src_bci;
  n->opcode_byte     = opcode_byte;
  n->cp_index        = cp_index;
  n->target_class    = sg_strdup(target_class);
  n->target_loader_id = target_loader_id;
  n->target_method   = sg_strdup(target_method);
  n->target_desc     = sg_strdup(target_desc);
  n->exact           = exact;
  n->diag_reason     = exact ? nullptr : sg_strdup(
                         diagnostic_reason ? diagnostic_reason : "unresolved");
  n->hash            = h;
  n->next            = g_gen_buckets[bucket];
  g_gen_buckets[bucket] = n;
  g_gen_count++;
  pthread_mutex_unlock(&g_gen_lock);

  if (exact) {
    fprintf(stderr,
            "[JVM GEN CALLSITE] cat=%s src=%s.%s%s bci=%d op=0x%02x cp=%d"
            " tgt=%s.%s%s\n",
            category ? category : "?",
            src_class  ? src_class  : "?",
            src_method ? src_method : "?",
            src_desc   ? src_desc   : "",
            src_bci, (unsigned)opcode_byte, cp_index,
            target_class  ? target_class  : "?",
            target_method ? target_method : "?",
            target_desc   ? target_desc   : "");
  } else {
    fprintf(stderr,
            "[JVM GEN CALLSITE] cat=%s UNRESOLVED reason=%s"
            " src=%s.%s bci=%d tgt=%s.%s\n",
            category ? category : "?",
            diagnostic_reason ? diagnostic_reason : "unresolved",
            src_class  ? src_class  : "?",
            src_method ? src_method : "?",
            src_bci,
            target_class  ? target_class  : "?",
            target_method ? target_method : "?");
  }
  return exact;
}

bool soroush_graph_target_set_callsite(
    const char* category, const char* adapter_shape,
    const char* adapter_class, const char* lf_kind,
    const char* aux_info,
    const char* src_class, uint64_t src_loader_id,
    const char* src_method, const char* src_desc,
    int src_bci, int opcode_byte, int cp_index,
    const char* semantic_op,
    bool staticizable,
    const char** staticization_blockers, int n_blockers,
    const SgMhTargetEntry* targets, int n_targets) {
  if (!soroush_graph_enabled()) return false;
  if (targets == nullptr || n_targets <= 0 || n_targets > 4) return false;

  // Dedup key: (src_class, src_method, src_desc, src_bci) — category excluded.
  uint32_t h = sg_hash_str(src_class)
             ^ sg_hash_str(src_method) ^ sg_hash_str(src_desc)
             ^ (uint32_t)(unsigned)src_bci;
  uint32_t bucket = h & (SG_TS_BUCKETS - 1u);

  pthread_mutex_lock(&g_ts_lock);

  for (SgTsCallsite* c = g_ts_buckets[bucket]; c != nullptr; c = c->next) {
    if (c->hash == h
        && src_bci == c->src_bci
        && (src_class == c->src_class ||
            (src_class && c->src_class && strcmp(src_class, c->src_class) == 0))
        && (src_method == c->src_method ||
            (src_method && c->src_method && strcmp(src_method, c->src_method) == 0))
        && (src_desc == c->src_desc ||
            (src_desc && c->src_desc && strcmp(src_desc, c->src_desc) == 0))) {
      pthread_mutex_unlock(&g_ts_lock);
      return true; // first-in-wins (category-agnostic)
    }
  }

  SgTsCallsite* n = (SgTsCallsite*)malloc(sizeof(SgTsCallsite));
  if (n == nullptr) { pthread_mutex_unlock(&g_ts_lock); return false; }
  memset(n, 0, sizeof(*n));

  n->category      = sg_strdup(category);
  n->adapter_shape = sg_strdup(adapter_shape);
  n->adapter_class = sg_strdup(adapter_class);
  n->lf_kind       = sg_strdup(lf_kind);
  n->aux_info      = sg_strdup(aux_info);
  n->semantic_op   = sg_strdup(semantic_op);
  n->staticizable  = staticizable;
  n->n_staticization_blockers = 0;
  if (staticization_blockers != nullptr) {
    for (int bi = 0; bi < n_blockers && bi < 4; bi++) {
      if (staticization_blockers[bi]) {
        snprintf(n->staticization_blockers[n->n_staticization_blockers++],
                 sizeof(n->staticization_blockers[0]),
                 "%s", staticization_blockers[bi]);
      }
    }
  }
  n->src_class     = sg_strdup(src_class);
  n->src_loader_id = src_loader_id;
  n->src_method    = sg_strdup(src_method);
  n->src_desc      = sg_strdup(src_desc);
  n->src_bci       = src_bci;
  n->opcode_byte   = opcode_byte;
  n->cp_index      = cp_index;
  n->n_targets     = n_targets;
  for (int i = 0; i < n_targets; i++) {
    n->targets[i].klass      = sg_strdup(targets[i].klass);
    n->targets[i].loader_id  = targets[i].loader_id;
    n->targets[i].method     = sg_strdup(targets[i].method);
    n->targets[i].descriptor = sg_strdup(targets[i].descriptor);
    n->targets[i].role       = sg_strdup(targets[i].role);
    n->targets[i].valid      = targets[i].valid;
  }
  n->hash  = h;
  n->next  = g_ts_buckets[bucket];
  g_ts_buckets[bucket] = n;
  g_ts_count++;
  pthread_mutex_unlock(&g_ts_lock);

  fprintf(stderr,
          "[JVM TS CALLSITE] cat=%s shape=%s adapter=%s lf_kind=%s"
          " src=%s.%s%s bci=%d n_targets=%d\n",
          category       ? category       : "?",
          adapter_shape  ? adapter_shape  : "?",
          adapter_class  ? adapter_class  : "?",
          lf_kind        ? lf_kind        : "?",
          src_class      ? src_class      : "?",
          src_method     ? src_method     : "?",
          src_desc       ? src_desc       : "",
          src_bci, n_targets);
  return true;
}

bool soroush_graph_adapter_graph_callsite(
    const char* category,
    const char* adapter_class, const char* adapter_kind, const char* lf_kind,
    const char* outer_desc,
    const char* src_class, uint64_t src_loader_id,
    const char* src_method, const char* src_desc,
    int src_bci, int opcode_byte, int cp_index,
    const SgAdapterNodeEntry* nodes, int n_nodes,
    const SgAdapterEdgeEntry* edges, int n_edges,
    bool all_exact,
    bool staticizable,
    const char** staticization_blockers, int n_blockers) {
  if (!soroush_graph_enabled()) return false;
  if (nodes == nullptr || n_nodes <= 0 || n_nodes > SG_AG_MAX_NODES) return false;

  // Dedup key: (src_class, src_method, src_desc, src_bci) — deliberately excluding
  // category so that a direct invokeExact record and a Case-A2 re-attributed
  // invoke record for the same callsite BCI are treated as duplicates and only
  // the first-in record is kept.
  uint32_t h = sg_hash_str(src_class)
             ^ sg_hash_str(src_method) ^ sg_hash_str(src_desc)
             ^ (uint32_t)(unsigned)src_bci;
  uint32_t bucket = h & (SG_AG_BUCKETS - 1u);

  pthread_mutex_lock(&g_ag_lock);

  for (SgAgCallsite* c = g_ag_buckets[bucket]; c != nullptr; c = c->next) {
    if (c->hash == h
        && src_bci == c->src_bci
        && (src_class == c->src_class ||
            (src_class && c->src_class && strcmp(src_class, c->src_class) == 0))
        && (src_method == c->src_method ||
            (src_method && c->src_method && strcmp(src_method, c->src_method) == 0))
        && (src_desc == c->src_desc ||
            (src_desc && c->src_desc && strcmp(src_desc, c->src_desc) == 0))) {
      pthread_mutex_unlock(&g_ag_lock);
      return true; // first-in-wins (category-agnostic)
    }
  }

  SgAgCallsite* n = (SgAgCallsite*)malloc(sizeof(SgAgCallsite));
  if (n == nullptr) { pthread_mutex_unlock(&g_ag_lock); return false; }
  memset(n, 0, sizeof(*n));

  n->category      = sg_strdup(category);
  n->adapter_class = sg_strdup(adapter_class);
  n->adapter_kind  = sg_strdup(adapter_kind);
  n->lf_kind       = sg_strdup(lf_kind);
  n->outer_desc    = sg_strdup(outer_desc);
  n->src_class     = sg_strdup(src_class);
  n->src_loader_id = src_loader_id;
  n->src_method    = sg_strdup(src_method);
  n->src_desc      = sg_strdup(src_desc);
  n->src_bci       = src_bci;
  n->opcode_byte   = opcode_byte;
  n->cp_index      = cp_index;
  n->all_exact               = all_exact;
  n->staticizable            = staticizable;
  n->n_staticization_blockers = 0;
  if (staticization_blockers != nullptr) {
    int nb = (n_blockers < 8) ? n_blockers : 8;
    for (int i = 0; i < nb; i++) {
      n->staticization_blockers[i] = sg_strdup(staticization_blockers[i]);
      if (n->staticization_blockers[i]) n->n_staticization_blockers++;
    }
  }
  n->n_nodes       = (n_nodes <= SG_AG_MAX_NODES) ? n_nodes : SG_AG_MAX_NODES;

  for (int i = 0; i < n->n_nodes; i++) {
    n->nodes[i].id                 = nodes[i].id;
    n->nodes[i].role               = sg_strdup(nodes[i].role);
    n->nodes[i].from_desc          = sg_strdup(nodes[i].from_desc);
    n->nodes[i].to_desc            = sg_strdup(nodes[i].to_desc);
    n->nodes[i].klass              = sg_strdup(nodes[i].klass);
    n->nodes[i].loader_id          = nodes[i].loader_id;
    n->nodes[i].method             = sg_strdup(nodes[i].method);
    n->nodes[i].descriptor         = sg_strdup(nodes[i].descriptor);
    n->nodes[i].node_adapter_class = sg_strdup(nodes[i].node_adapter_class);
    n->nodes[i].node_classification= sg_strdup(nodes[i].node_classification);
    n->nodes[i].exact_false_reason = sg_strdup(nodes[i].exact_false_reason);
    n->nodes[i].semantic_op        = sg_strdup(nodes[i].semantic_op);
    n->nodes[i].from_type          = sg_strdup(nodes[i].from_type);
    n->nodes[i].to_type            = sg_strdup(nodes[i].to_type);
    n->nodes[i].exact              = nodes[i].exact;
  }

  n->n_edges = 0;
  if (edges != nullptr && n_edges > 0) {
    int ne = (n_edges <= SG_AG_MAX_EDGES) ? n_edges : SG_AG_MAX_EDGES;
    n->n_edges = ne;
    for (int i = 0; i < ne; i++) {
      n->edges[i].from_id = edges[i].from_id;
      n->edges[i].to_id   = edges[i].to_id;
      snprintf(n->edges[i].kind, sizeof(n->edges[i].kind), "%s",
               edges[i].kind ? edges[i].kind : "contains");
    }
  }

  n->hash = h;
  n->next = g_ag_buckets[bucket];
  g_ag_buckets[bucket] = n;
  g_ag_count++;
  pthread_mutex_unlock(&g_ag_lock);

  fprintf(stderr,
          "[JVM AG CALLSITE] cat=%s adapter=%s kind=%s lf=%s"
          " src=%s.%s bci=%d n_nodes=%d all_exact=%s\n",
          category      ? category      : "?",
          adapter_class ? adapter_class : "?",
          adapter_kind  ? adapter_kind  : "?",
          lf_kind       ? lf_kind       : "?",
          src_class     ? src_class     : "?",
          src_method    ? src_method    : "?",
          src_bci, n_nodes,
          all_exact ? "true" : "false");
  return true;
}

bool soroush_graph_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* v = ::getenv("SOROUSH_PROVENANCE_GRAPH");
    enabled = (v != nullptr && strcmp(v, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static const char* sg_node_type_name(int t) {
  switch (t) {
    case SG_NODE_CLASS: return "Class";
    case SG_NODE_METHOD: return "Method";
    case SG_NODE_EXECUTION: return "Execution";
    case SG_NODE_GENERATED_CLASS: return "GeneratedClass";
    case SG_NODE_INDY_CALLSITE: return "IndyCallSite";
    case SG_NODE_BOOTSTRAP_METHOD: return "BootstrapMethod";
    case SG_NODE_REFLECTION_INVOKE: return "ReflectionInvoke";
    case SG_NODE_METHODHANDLE_LINKAGE: return "MethodHandleLinkage";
    case SG_NODE_BYTECODE_ARTIFACT: return "BytecodeArtifact";
    case SG_NODE_ASYNC_TASK: return "AsyncTask";
    case SG_NODE_THREAD: return "Thread";
    case SG_NODE_EXECUTOR: return "Executor";
    case SG_NODE_MH_ADAPTER: return "MethodHandleAdapter";
    case SG_NODE_LAMBDAFORM_EXEC: return "LambdaFormExecution";
    default: return "?";
  }
}

static const char* sg_edge_type_name(int t) {
  switch (t) {
    case SG_EDGE_EXECUTES: return "EXECUTES";
    case SG_EDGE_CALLS: return "CALLS";
    case SG_EDGE_GENERATED_FROM: return "GENERATED_FROM";
    case SG_EDGE_CREATED_BY: return "CREATED_BY";
    case SG_EDGE_LINKS_TO: return "LINKS_TO";
    case SG_EDGE_HAS_BYTECODE: return "HAS_BYTECODE";
    case SG_EDGE_REWRITTEN_FROM: return "REWRITTEN_FROM";
    case SG_EDGE_SCHEDULES: return "SCHEDULES";
    case SG_EDGE_SUBMITTED_TO: return "SUBMITTED_TO";
    case SG_EDGE_EXECUTES_ASYNC: return "EXECUTES_ASYNC";
    case SG_EDGE_CONTINUES_ON: return "CONTINUES_ON";
    case SG_EDGE_MH_INVOKES: return "MH_INVOKES";
    case SG_EDGE_ADAPTS_TO: return "ADAPTS_TO";
    case SG_EDGE_BINDS_TO: return "BINDS_TO";
    case SG_EDGE_INVOKE_BASIC: return "INVOKE_BASIC";
    case SG_EDGE_RESOLVES_TO: return "RESOLVES_TO";
    default: return "?";
  }
}

static uint64_t sg_hash(const char* s) {
  uint64_t h = 1469598103934665603ULL; // FNV-1a 64
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

// Copy an internal/dotted class name into dst as internal form (slashes).
static void sg_internalize(char* dst, size_t dst_len, const char* name) {
  if (dst_len == 0) return;
  size_t i = 0;
  if (name != nullptr) {
    for (; name[i] != '\0' && i < dst_len - 1; i++) {
      dst[i] = (name[i] == '.') ? '/' : name[i];
    }
  }
  dst[i] = '\0';
}

static char* sg_strdup(const char* s) {
  if (s == nullptr) return nullptr;
  size_t n = strlen(s) + 1;
  char* d = (char*)malloc(n);
  if (d != nullptr) memcpy(d, s, n);
  return d;
}

// Lock must be held.
static bool sg_map_grow_locked() {
  long new_size = g_map_size == 0 ? 4096 : g_map_size * 2;
  int32_t* nm = (int32_t*)calloc(new_size, sizeof(int32_t));
  if (nm == nullptr) return false;
  for (long i = 0; i < g_map_size; i++) {
    int32_t slot = g_map[i];
    if (slot == 0) continue;
    const char* key = g_nodes[slot - 1].key;
    long mask = new_size - 1;
    long idx = (long)(sg_hash(key) & (uint64_t)mask);
    while (nm[idx] != 0) idx = (idx + 1) & mask;
    nm[idx] = slot;
  }
  free(g_map);
  g_map = nm;
  g_map_size = new_size;
  return true;
}

// Find-or-create a node by identity key. Lock must be held. Returns node id (0
// on failure), sets *is_new. label/loader_id/crc/flags only used on creation.
static uint64_t sg_intern_locked(int type, const char* key, const char* label,
                                 uint64_t loader_id, uint32_t crc, int flags,
                                 bool* is_new) {
  *is_new = false;
  if (key == nullptr) return 0;

  if (g_map_size == 0 && !sg_map_grow_locked()) return 0;
  if ((g_map_used + 1) * 10 >= g_map_size * 7 && !sg_map_grow_locked()) return 0;

  long mask = g_map_size - 1;
  long idx = (long)(sg_hash(key) & (uint64_t)mask);
  while (g_map[idx] != 0) {
    SgNode* n = &g_nodes[g_map[idx] - 1];
    if (n->type == type && strcmp(n->key, key) == 0) {
      // Existing node. Note loader divergence for classes (observational).
      if ((type == SG_NODE_CLASS || type == SG_NODE_GENERATED_CLASS) &&
          loader_id != 0 && n->loader_id != 0 && n->loader_id != loader_id) {
        fprintf(stderr, "[JVM GRAPH NOTE] loader-divergence node=" UINT64_FORMAT
                " key=%s loader1=" UINT64_FORMAT " loader2=" UINT64_FORMAT "\n",
                (uint64_t)n->id, n->key, (uint64_t)n->loader_id, (uint64_t)loader_id);
      }
      return n->id;
    }
    idx = (idx + 1) & mask;
  }

  if (g_node_count >= SG_MAX_NODES) {
    if (!g_overflow_logged) {
      fprintf(stderr, "[JVM GRAPH NOTE] node capacity reached (%ld); dropping further nodes\n", SG_MAX_NODES);
      g_overflow_logged = true;
    }
    return 0;
  }
  if (g_node_count >= g_node_cap) {
    long new_cap = g_node_cap == 0 ? 1024 : g_node_cap * 2;
    SgNode* nn = (SgNode*)realloc(g_nodes, new_cap * sizeof(SgNode));
    if (nn == nullptr) return 0;
    g_nodes = nn;
    g_node_cap = new_cap;
  }
  char* key_copy = sg_strdup(key);
  if (key_copy == nullptr) return 0;

  SgNode* node = &g_nodes[g_node_count];
  node->id = (uint64_t)(g_node_count + 1);
  node->type = type;
  node->key = key_copy;
  node->label = sg_strdup(label);
  node->loader_id = loader_id;
  node->crc = crc;
  node->flags = flags;
  g_node_count++;

  g_map[idx] = (int32_t)g_node_count; // index + 1
  g_map_used++;
  *is_new = true;

  // Diagnostics: pick the most specific prefix.
  if (type == SG_NODE_CLASS || type == SG_NODE_GENERATED_CLASS) {
    fprintf(stderr, "[JVM GRAPH CLASS] id=" UINT64_FORMAT " type=%s name=%s loader=" UINT64_FORMAT " hidden=%d crc=%08x\n",
            (uint64_t)node->id, sg_node_type_name(type), label ? label : "?",
            (uint64_t)loader_id, (flags & 1) ? 1 : 0, crc);
  } else if (type == SG_NODE_INDY_CALLSITE) {
    fprintf(stderr, "[JVM GRAPH INDY] id=" UINT64_FORMAT " %s\n",
            (uint64_t)node->id, label ? label : key);
  } else if (type == SG_NODE_EXECUTION) {
    fprintf(stderr, "[JVM GRAPH EXEC] id=" UINT64_FORMAT " %s\n",
            (uint64_t)node->id, label ? label : key);
  } else if (type == SG_NODE_THREAD) {
    fprintf(stderr, "[JVM GRAPH THREAD] id=" UINT64_FORMAT " %s\n",
            (uint64_t)node->id, label ? label : key);
  } else if (type == SG_NODE_EXECUTOR) {
    fprintf(stderr, "[JVM GRAPH EXECUTOR] id=" UINT64_FORMAT " %s\n",
            (uint64_t)node->id, label ? label : key);
  } else if (type == SG_NODE_ASYNC_TASK) {
    fprintf(stderr, "[JVM GRAPH ASYNC] id=" UINT64_FORMAT " %s\n",
            (uint64_t)node->id, label ? label : key);
  } else if (type == SG_NODE_MH_ADAPTER || type == SG_NODE_LAMBDAFORM_EXEC) {
    fprintf(stderr, "[JVM GRAPH MH] id=" UINT64_FORMAT " type=%s %s\n",
            (uint64_t)node->id, sg_node_type_name(type), label ? label : key);
  } else {
    fprintf(stderr, "[JVM GRAPH NODE] id=" UINT64_FORMAT " type=%s key=%s\n",
            (uint64_t)node->id, sg_node_type_name(type), key);
  }
  return node->id;
}

// Lock must be held.
static void sg_add_edge_locked(uint64_t from, uint64_t to, int type) {
  if (from == 0 || to == 0) return;
  if (g_edge_count >= SG_MAX_EDGES) return;
  if (g_edge_count >= g_edge_cap) {
    long new_cap = g_edge_cap == 0 ? 2048 : g_edge_cap * 2;
    SgEdge* ne = (SgEdge*)realloc(g_edges, new_cap * sizeof(SgEdge));
    if (ne == nullptr) return;
    g_edges = ne;
    g_edge_cap = new_cap;
  }
  g_edges[g_edge_count].from = from;
  g_edges[g_edge_count].to = to;
  g_edges[g_edge_count].type = type;
  g_edge_count++;
  fprintf(stderr, "[JVM GRAPH EDGE] from=" UINT64_FORMAT " to=" UINT64_FORMAT " type=%s\n",
          (uint64_t)from, (uint64_t)to, sg_edge_type_name(type));
}

// Lock must be held. Emit a loader-divergence NOTE the first time a class name
// is seen under a second loader. Best-effort, observational; never affects
// identity. Skips when loader_id == 0 (unknown loader carries no divergence
// signal).
static void sg_note_loader_divergence_locked(const char* norm, uint64_t loader_id) {
  if (loader_id == 0) return;
  if (g_name_seen == nullptr) {
    g_name_seen = (SgNameSeen*)calloc(SG_NAME_TABLE, sizeof(SgNameSeen));
    if (g_name_seen == nullptr) return; // fail-safe: skip the diagnostic
  }
  uint64_t nh = sg_hash(norm);
  if (nh == 0) nh = 1; // reserve 0 for empty
  SgNameSeen* slot = &g_name_seen[nh & (SG_NAME_TABLE - 1)];
  if (slot->name_hash == 0) {
    slot->name_hash = nh;
    slot->loader_id = loader_id;
  } else if (slot->name_hash == nh && slot->loader_id != loader_id) {
    fprintf(stderr, "[JVM GRAPH NOTE] loader-divergence name=%s loader1=" UINT64_FORMAT
            " loader2=" UINT64_FORMAT " (distinct Class nodes)\n",
            norm, (uint64_t)slot->loader_id, (uint64_t)loader_id);
  }
  // name_hash mismatch in this slot == hash collision with another name: leave
  // the existing entry (best-effort; a missed NOTE is acceptable).
}

// Intern a Class node from an internal name. Lock must be held. Identity is
// loader-precise: the key is "C|<norm>|<loader_id>|<hidden>" so two runtime
// classes with the same internal name but different loaders are DISTINCT nodes.
static uint64_t sg_intern_class_locked(const char* internal_name, uint64_t loader_id,
                                       int hidden, bool* is_new) {
  char norm[1024];
  sg_internalize(norm, sizeof(norm), internal_name);
  char key[1160];
  snprintf(key, sizeof(key), "C|%s|" UINT64_FORMAT "|%d", norm, (uint64_t)loader_id, hidden ? 1 : 0);
  uint64_t id = sg_intern_locked(SG_NODE_CLASS, key, norm, loader_id, 0, hidden ? 1 : 0, is_new);
  if (*is_new) {
    sg_note_loader_divergence_locked(norm, loader_id);
  }
  return id;
}

uint64_t soroush_graph_execution_exact(uint64_t exec_id, uint64_t parent_exec_id,
                                       const char* dotted_class, const char* method,
                                       const char* descriptor, uint64_t loader_id,
                                       int hidden, const void* thread_id,
                                       bool* out_method_is_new) {
  if (out_method_is_new != nullptr) *out_method_is_new = false;
  if (!soroush_graph_enabled() || exec_id == 0) return 0;
  // INVARIANT: this is the SOLE execution Method-node creation path (the legacy
  // best-effort string path was removed), and it requires an exact descriptor.
  // We fail-fast (return 0 -> caller logs identity-unresolved) rather than
  // fabricate a "?" descriptor or a loader_id=0 node. NB: a hard assert would
  // violate the graph's observational/fail-safe contract, so this is a guarded
  // early return, not an abort.
  if (descriptor == nullptr || descriptor[0] == '\0') return 0;
  pthread_mutex_lock(&g_lock);

  bool cls_new = false;
  uint64_t cls = sg_intern_class_locked(dotted_class, loader_id, hidden, &cls_new);

  char norm[1024];
  sg_internalize(norm, sizeof(norm), dotted_class);
  // Exact Method identity: (class_node_id, method_name, descriptor) — no "?".
  char mkey[1300];
  snprintf(mkey, sizeof(mkey), "M|" UINT64_FORMAT "|%s|%s", (uint64_t)cls,
           method ? method : "?", descriptor);
  char mlabel[1400];
  snprintf(mlabel, sizeof(mlabel), "%s.%s%s", norm, method ? method : "?", descriptor);
  bool m_new = false;
  uint64_t mnode = sg_intern_locked(SG_NODE_METHOD, mkey, mlabel, 0, 0, 0, &m_new);
  if (out_method_is_new != nullptr) *out_method_is_new = m_new;
  if (m_new) {
    fprintf(stderr, "[JVM GRAPH CLASS] method-node=" UINT64_FORMAT " %s attached class-node="
            UINT64_FORMAT " loader=" UINT64_FORMAT " hidden=%d (exact)\n",
            (uint64_t)mnode, mlabel, (uint64_t)cls, (uint64_t)loader_id, hidden ? 1 : 0);
  }

  char ekey[64];
  snprintf(ekey, sizeof(ekey), "E|" UINT64_FORMAT, (uint64_t)exec_id);
  char elabel[1500];
  snprintf(elabel, sizeof(elabel), "exec=" UINT64_FORMAT " %s thread=%p",
           (uint64_t)exec_id, mlabel, thread_id);
  bool e_new = false;
  uint64_t enode = sg_intern_locked(SG_NODE_EXECUTION, ekey, elabel, 0, 0, 0, &e_new);

  if (e_new) {
    sg_add_edge_locked(enode, mnode, SG_EDGE_EXECUTES);
    if (parent_exec_id != 0) {
      char pkey[64];
      snprintf(pkey, sizeof(pkey), "E|" UINT64_FORMAT, (uint64_t)parent_exec_id);
      bool p_new = false;
      uint64_t pnode = sg_intern_locked(SG_NODE_EXECUTION, pkey, nullptr, 0, 0, 0, &p_new);
      sg_add_edge_locked(pnode, enode, SG_EDGE_CALLS);
    }
  }
  pthread_mutex_unlock(&g_lock);
  return mnode;
}

void soroush_graph_generated_class(const char* internal_name, int hidden,
                                   const char* generated_by, const char* source_trigger,
                                   const char* provenance_kind, int indy_trace_id,
                                   uint32_t crc, uint64_t loader_id) {
  if (!soroush_graph_enabled()) return;
  pthread_mutex_lock(&g_lock);

  char norm[1024];
  sg_internalize(norm, sizeof(norm), internal_name);
  // Loader-precise generated-class identity: name + loader + crc (generation/
  // content identity). The indy/lambda trace_id join is preserved separately via
  // the CREATED_BY edge to the IndyCallSite (keyed by trace_id).
  char gkey[1260];
  snprintf(gkey, sizeof(gkey), "G|%s|" UINT64_FORMAT "|%08x", norm, (uint64_t)loader_id, crc);
  char glabel[1400];
  snprintf(glabel, sizeof(glabel), "%s by=%s trigger=%s kind=%s",
           norm, generated_by ? generated_by : "?", source_trigger ? source_trigger : "?",
           provenance_kind ? provenance_kind : "?");
  bool g_new = false;
  uint64_t gnode = sg_intern_locked(SG_NODE_GENERATED_CLASS, gkey, glabel, loader_id, crc,
                                    hidden ? 1 : 0, &g_new);
  if (g_new) {
    bool cls_new = false;
    uint64_t cls = sg_intern_class_locked(internal_name, loader_id, hidden, &cls_new);
    sg_add_edge_locked(gnode, cls, SG_EDGE_GENERATED_FROM);
    if (indy_trace_id > 0) {
      char ikey[64];
      snprintf(ikey, sizeof(ikey), "I|%d", indy_trace_id);
      char ilabel[64];
      snprintf(ilabel, sizeof(ilabel), "trace_id=%d", indy_trace_id);
      bool i_new = false;
      uint64_t inode = sg_intern_locked(SG_NODE_INDY_CALLSITE, ikey, ilabel, 0, 0, 0, &i_new);
      sg_add_edge_locked(gnode, inode, SG_EDGE_CREATED_BY);
    }
  }
  pthread_mutex_unlock(&g_lock);
}

void soroush_graph_indy(int trace_id, const char* caller, const char* descriptor,
                        const char* bootstrap) {
  if (!soroush_graph_enabled() || trace_id <= 0) return;
  pthread_mutex_lock(&g_lock);

  char ikey[64];
  snprintf(ikey, sizeof(ikey), "I|%d", trace_id);
  char ilabel[1200];
  snprintf(ilabel, sizeof(ilabel), "trace_id=%d caller=%s indy=%s", trace_id,
           caller ? caller : "?", descriptor ? descriptor : "?");
  bool i_new = false;
  uint64_t inode = sg_intern_locked(SG_NODE_INDY_CALLSITE, ikey, ilabel, 0, 0, 0, &i_new);

  if (bootstrap != nullptr && bootstrap[0] != '\0') {
    char bkey[1100];
    snprintf(bkey, sizeof(bkey), "B|%s", bootstrap);
    bool b_new = false;
    uint64_t bnode = sg_intern_locked(SG_NODE_BOOTSTRAP_METHOD, bkey, bootstrap, 0, 0, 0, &b_new);
    if (i_new || b_new) {
      sg_add_edge_locked(inode, bnode, SG_EDGE_CREATED_BY);
    }
  }
  pthread_mutex_unlock(&g_lock);
}

void soroush_graph_linkage(int node_type, const char* source,
                           const char* internal_class, const char* method,
                           const char* descriptor, uint64_t loader_id) {
  if (!soroush_graph_enabled()) return;
  if (node_type != SG_NODE_REFLECTION_INVOKE && node_type != SG_NODE_METHODHANDLE_LINKAGE) {
    node_type = SG_NODE_METHODHANDLE_LINKAGE;
  }
  pthread_mutex_lock(&g_lock);

  char norm[1024];
  sg_internalize(norm, sizeof(norm), internal_class);
  char lkey[1300];
  snprintf(lkey, sizeof(lkey), "%c|%s.%s%s", node_type == SG_NODE_REFLECTION_INVOKE ? 'R' : 'H',
           norm, method ? method : "?", descriptor ? descriptor : "");
  char llabel[1400];
  snprintf(llabel, sizeof(llabel), "src=%s %s.%s%s", source ? source : "?", norm,
           method ? method : "?", descriptor ? descriptor : "");
  bool l_new = false;
  uint64_t lnode = sg_intern_locked(node_type, lkey, llabel, 0, 0, 0, &l_new);

  if (l_new) {
    bool cls_new = false;
    uint64_t cls = sg_intern_class_locked(internal_class, loader_id, 0, &cls_new);
    char mkey[1300];
    snprintf(mkey, sizeof(mkey), "M|" UINT64_FORMAT "|%s|%s", (uint64_t)cls,
             method ? method : "?", descriptor ? descriptor : "?");
    char mlabel[1400];
    snprintf(mlabel, sizeof(mlabel), "%s.%s%s", norm, method ? method : "?",
             descriptor ? descriptor : "");
    bool m_new = false;
    uint64_t mnode = sg_intern_locked(SG_NODE_METHOD, mkey, mlabel, 0, 0, 0, &m_new);
    sg_add_edge_locked(lnode, mnode, SG_EDGE_LINKS_TO);
  }
  pthread_mutex_unlock(&g_lock);
}

void soroush_graph_bytecode(const char* internal_class, uint32_t crc, int size,
                            int transformed, const char* load_kind, uint64_t loader_id,
                            int hidden) {
  if (!soroush_graph_enabled()) return;
  pthread_mutex_lock(&g_lock);

  char norm[1024];
  sg_internalize(norm, sizeof(norm), internal_class);
  const char* kind = transformed ? "final" : "original";
  // Loader-precise artifact identity: two loaders that load identical bytes for
  // the same name still get distinct artifacts, so each loader-specific Class
  // node links only to its own bytecode (never crosses loaders).
  char akey[1260];
  snprintf(akey, sizeof(akey), "A|%s|" UINT64_FORMAT "|%08x|%s", norm, (uint64_t)loader_id, crc, kind);
  char alabel[1360];
  snprintf(alabel, sizeof(alabel), "%s crc=%08x size=%d kind=%s load=%s loader=" UINT64_FORMAT,
           norm, crc, size, kind, load_kind ? load_kind : "?", (uint64_t)loader_id);
  bool a_new = false;
  uint64_t anode = sg_intern_locked(SG_NODE_BYTECODE_ARTIFACT, akey, alabel, loader_id, crc, hidden ? 1 : 0, &a_new);

  if (a_new) {
    bool cls_new = false;
    uint64_t cls = sg_intern_class_locked(internal_class, loader_id, hidden, &cls_new);
    sg_add_edge_locked(cls, anode, SG_EDGE_HAS_BYTECODE);
    fprintf(stderr, "[JVM GRAPH BYTECODE] artifact=" UINT64_FORMAT " kind=%s crc=%08x"
            " attached class-node=" UINT64_FORMAT " loader=" UINT64_FORMAT " name=%s\n",
            (uint64_t)anode, kind, crc, (uint64_t)cls, (uint64_t)loader_id, norm);
    if (transformed) {
      // Link the final artifact back to the recorded original artifact for the
      // SAME class+loader (scope the prefix by loader so we never cross loaders).
      char okey[1160];
      snprintf(okey, sizeof(okey), "A|%s|" UINT64_FORMAT "|", norm, (uint64_t)loader_id);
      uint64_t orig = 0;
      for (long i = 0; i < g_node_count; i++) {
        SgNode* n = &g_nodes[i];
        if (n->type != SG_NODE_BYTECODE_ARTIFACT) continue;
        size_t plen = strlen(okey);
        if (strncmp(n->key, okey, plen) == 0) {
          size_t klen = strlen(n->key);
          if (klen >= 9 && strcmp(n->key + klen - 9, "|original") == 0) { orig = n->id; break; }
        }
      }
      if (orig != 0 && orig != anode) {
        sg_add_edge_locked(anode, orig, SG_EDGE_REWRITTEN_FROM);
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// Async / cross-thread causality
//
// A direct-mapped side-table remembers, per task identity hash, the submitter's
// execution context so a worker thread that later runs the task can link back
// across the thread boundary. Direct-mapped means a hash collision overwrites
// the older entry (observational; a missed/merged edge is acceptable). All
// access is under g_lock.
// ---------------------------------------------------------------------------
struct SgAsyncCtx {
  uint32_t task_hash;
  uint64_t submitter_exec_id;
  uint64_t async_task_node;
  bool     valid;
};

static const long SG_ASYNC_TABLE = 1 << 16; // 65536 slots
static SgAsyncCtx* g_async_tab = nullptr;

// Lock must be held.
static SgAsyncCtx* sg_async_slot_locked(uint32_t task_hash) {
  if (g_async_tab == nullptr) {
    g_async_tab = (SgAsyncCtx*)calloc(SG_ASYNC_TABLE, sizeof(SgAsyncCtx));
    if (g_async_tab == nullptr) return nullptr;
  }
  return &g_async_tab[task_hash & (SG_ASYNC_TABLE - 1)];
}

// Lock must be held. Intern an Execution node by exec id (no edges).
static uint64_t sg_execution_node_locked(uint64_t exec_id) {
  if (exec_id == 0) return 0;
  char ekey[64];
  snprintf(ekey, sizeof(ekey), "E|" UINT64_FORMAT, (uint64_t)exec_id);
  bool e_new = false;
  return sg_intern_locked(SG_NODE_EXECUTION, ekey, nullptr, 0, 0, 0, &e_new);
}

// Lock must be held. Intern a Thread node by stable OS tid.
static uint64_t sg_thread_node_locked(uint64_t tid, const char* name, int is_daemon) {
  if (tid == 0) return 0;
  char tkey[64];
  snprintf(tkey, sizeof(tkey), "Th|" UINT64_FORMAT, (uint64_t)tid);
  char tlabel[256];
  snprintf(tlabel, sizeof(tlabel), "tid=" UINT64_FORMAT " name=%s%s",
           (uint64_t)tid, name ? name : "?", is_daemon ? " daemon" : "");
  bool t_new = false;
  return sg_intern_locked(SG_NODE_THREAD, tkey, tlabel, tid, 0, is_daemon ? 1 : 0, &t_new);
}

uint64_t soroush_graph_execution_node(uint64_t exec_id) {
  if (!soroush_graph_enabled() || exec_id == 0) return 0;
  pthread_mutex_lock(&g_lock);
  uint64_t id = sg_execution_node_locked(exec_id);
  pthread_mutex_unlock(&g_lock);
  return id;
}

uint64_t soroush_graph_thread_node(uint64_t tid, const char* name, int is_daemon) {
  if (!soroush_graph_enabled() || tid == 0) return 0;
  pthread_mutex_lock(&g_lock);
  uint64_t id = sg_thread_node_locked(tid, name, is_daemon);
  pthread_mutex_unlock(&g_lock);
  return id;
}

uint64_t soroush_graph_thread_start(uint64_t parent_tid, const char* parent_name,
                                    uint64_t parent_exec_id,
                                    uint64_t child_tid, const char* child_name,
                                    int child_is_daemon) {
  if (!soroush_graph_enabled() || child_tid == 0) return 0;
  pthread_mutex_lock(&g_lock);
  uint64_t parent_thr = sg_thread_node_locked(parent_tid, parent_name, 0);
  uint64_t child_thr  = sg_thread_node_locked(child_tid, child_name, child_is_daemon);
  // Structural thread->thread start edge.
  if (parent_thr != 0 && child_thr != 0) {
    sg_add_edge_locked(parent_thr, child_thr, SG_EDGE_SCHEDULES);
  }
  // Causal: the parent execution that called start() scheduled the child thread.
  if (parent_exec_id != 0) {
    uint64_t pexec = sg_execution_node_locked(parent_exec_id);
    sg_add_edge_locked(pexec, child_thr, SG_EDGE_SCHEDULES);
  }
  pthread_mutex_unlock(&g_lock);
  return child_thr;
}

void soroush_graph_async_submit(uint32_t task_hash, uint32_t executor_hash,
                                uint64_t submitter_exec_id, uint64_t submitter_tid,
                                const char* executor_label) {
  if (!soroush_graph_enabled() || task_hash == 0) return;
  pthread_mutex_lock(&g_lock);

  char tkey[64];
  snprintf(tkey, sizeof(tkey), "T|%08x", task_hash);
  char tlabel[128];
  snprintf(tlabel, sizeof(tlabel), "task=%08x submit_tid=" UINT64_FORMAT,
           task_hash, (uint64_t)submitter_tid);
  bool t_new = false;
  uint64_t tnode = sg_intern_locked(SG_NODE_ASYNC_TASK, tkey, tlabel, 0, 0, 0, &t_new);

  uint64_t xnode = 0;
  if (executor_hash != 0) {
    char xkey[64];
    snprintf(xkey, sizeof(xkey), "X|%08x", executor_hash);
    char xlabel[128];
    snprintf(xlabel, sizeof(xlabel), "%s hash=%08x",
             executor_label ? executor_label : "Executor", executor_hash);
    bool x_new = false;
    xnode = sg_intern_locked(SG_NODE_EXECUTOR, xkey, xlabel, 0, 0, 0, &x_new);
  }

  if (submitter_exec_id != 0) {
    uint64_t sexec = sg_execution_node_locked(submitter_exec_id);
    sg_add_edge_locked(sexec, tnode, SG_EDGE_SCHEDULES);
  }
  if (xnode != 0) {
    sg_add_edge_locked(tnode, xnode, SG_EDGE_SUBMITTED_TO);
  }

  SgAsyncCtx* slot = sg_async_slot_locked(task_hash);
  if (slot != nullptr) {
    slot->task_hash = task_hash;
    slot->submitter_exec_id = submitter_exec_id;
    slot->async_task_node = tnode;
    slot->valid = true;
  }
  pthread_mutex_unlock(&g_lock);
}

bool soroush_graph_async_run(uint32_t task_hash, uint64_t worker_tid,
                             uint64_t* out_submitter_exec_id,
                             uint64_t* out_async_task_node) {
  if (out_submitter_exec_id) *out_submitter_exec_id = 0;
  if (out_async_task_node) *out_async_task_node = 0;
  if (!soroush_graph_enabled() || task_hash == 0) return false;
  pthread_mutex_lock(&g_lock);

  // Touch the worker Thread node so it appears even without later ENTERs.
  (void)sg_thread_node_locked(worker_tid, nullptr, 0);

  bool found = false;
  SgAsyncCtx* slot = sg_async_slot_locked(task_hash);
  if (slot != nullptr && slot->valid && slot->task_hash == task_hash) {
    if (out_submitter_exec_id) *out_submitter_exec_id = slot->submitter_exec_id;
    if (out_async_task_node) *out_async_task_node = slot->async_task_node;
    slot->valid = false; // one-shot: a task runs once
    found = true;
  }
  pthread_mutex_unlock(&g_lock);
  return found;
}

void soroush_graph_async_continue(uint64_t cause_exec_id, uint64_t owner_node,
                                  uint64_t worker_exec_id) {
  if (!soroush_graph_enabled() || worker_exec_id == 0) return;
  if (cause_exec_id == 0 && owner_node == 0) return;
  pthread_mutex_lock(&g_lock);
  uint64_t wexec = sg_execution_node_locked(worker_exec_id);
  if (cause_exec_id != 0) {
    uint64_t cexec = sg_execution_node_locked(cause_exec_id);
    sg_add_edge_locked(cexec, wexec, SG_EDGE_CONTINUES_ON);
  }
  if (owner_node != 0) {
    sg_add_edge_locked(owner_node, wexec, SG_EDGE_EXECUTES_ASYNC);
  }
  pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// MethodHandle / LambdaForm execution tracing
// ---------------------------------------------------------------------------
void soroush_graph_mh_chain(uint64_t caller_exec_id, const char* mh_kind,
                            const char* type_desc, const char* lf_name,
                            const char* final_class, const char* final_method,
                            const char* final_desc, int is_bound,
                            uint64_t final_loader_id) {
  if (!soroush_graph_enabled()) return;
  if (mh_kind == nullptr || mh_kind[0] == '\0') return;
  pthread_mutex_lock(&g_lock);

  // MethodHandleAdapter node: kind + LambdaForm name distinguish chain members.
  char akey[768];
  snprintf(akey, sizeof(akey), "MH|%s|%s", mh_kind, lf_name ? lf_name : "?");
  char alabel[900];
  snprintf(alabel, sizeof(alabel), "%s%s type=%s lf=%s", mh_kind,
           is_bound ? " (bound)" : "", type_desc ? type_desc : "?",
           lf_name ? lf_name : "?");
  bool a_new = false;
  uint64_t anode = sg_intern_locked(SG_NODE_MH_ADAPTER, akey, alabel, 0, 0,
                                    is_bound ? 1 : 0, &a_new);

  // LambdaFormExecution node, keyed by its vmentry class.method.
  uint64_t lfnode = 0;
  if (lf_name != nullptr && lf_name[0] != '\0') {
    char lkey[600];
    snprintf(lkey, sizeof(lkey), "LF|%s", lf_name);
    bool lf_new = false;
    lfnode = sg_intern_locked(SG_NODE_LAMBDAFORM_EXEC, lkey, lf_name, 0, 0, 0, &lf_new);
    sg_add_edge_locked(anode, lfnode, SG_EDGE_INVOKE_BASIC);
  }

  // Caller execution -> this adapter (the invocation site), when known.
  if (caller_exec_id != 0) {
    uint64_t cexec = sg_execution_node_locked(caller_exec_id);
    sg_add_edge_locked(cexec, anode, SG_EDGE_MH_INVOKES);
  }

  // Final resolved target (DirectMethodHandle member) -> RESOLVES_TO Method.
  if (final_class != nullptr && final_method != nullptr) {
    char norm[1024];
    sg_internalize(norm, sizeof(norm), final_class);
    bool cls_new = false;
    uint64_t cls = sg_intern_class_locked(final_class, final_loader_id, 0, &cls_new);
    char mkey[1300];
    snprintf(mkey, sizeof(mkey), "M|" UINT64_FORMAT "|%s|%s", (uint64_t)cls,
             final_method, final_desc ? final_desc : "?");
    char mlabel[1400];
    snprintf(mlabel, sizeof(mlabel), "%s.%s%s", norm, final_method,
             final_desc ? final_desc : "");
    bool m_new = false;
    uint64_t mnode = sg_intern_locked(SG_NODE_METHOD, mkey, mlabel, 0, 0, 0, &m_new);
    sg_add_edge_locked(anode, mnode, SG_EDGE_RESOLVES_TO);
  }
  pthread_mutex_unlock(&g_lock);
}

void soroush_graph_lambdaform_exec(uint64_t exec_id, const char* lf_name) {
  if (!soroush_graph_enabled() || lf_name == nullptr || lf_name[0] == '\0') return;
  pthread_mutex_lock(&g_lock);
  char lkey[600];
  snprintf(lkey, sizeof(lkey), "LF|%s", lf_name);
  bool lf_new = false;
  uint64_t lfnode = sg_intern_locked(SG_NODE_LAMBDAFORM_EXEC, lkey, lf_name, 0, 0, 0, &lf_new);
  if (exec_id != 0) {
    uint64_t exec = sg_execution_node_locked(exec_id);
    sg_add_edge_locked(exec, lfnode, SG_EDGE_INVOKE_BASIC);
  }
  pthread_mutex_unlock(&g_lock);
}

void soroush_graph_dump_summary() {
  if (!soroush_graph_enabled()) return;
  pthread_mutex_lock(&g_lock);
  long node_by_type[16];
  long edge_by_type[16];
  memset(node_by_type, 0, sizeof(node_by_type));
  memset(edge_by_type, 0, sizeof(edge_by_type));
  for (long i = 0; i < g_node_count; i++) {
    int t = g_nodes[i].type;
    if (t >= 0 && t < 16) node_by_type[t]++;
  }
  for (long i = 0; i < g_edge_count; i++) {
    int t = g_edges[i].type;
    if (t >= 0 && t < 16) edge_by_type[t]++;
  }
  fprintf(stderr, "[JVM GRAPH SUMMARY] nodes=%ld edges=%ld\n", g_node_count, g_edge_count);
  for (int t = 1; t <= 14; t++) {
    if (node_by_type[t] > 0) {
      fprintf(stderr, "[JVM GRAPH SUMMARY] node_type=%s count=%ld\n", sg_node_type_name(t), node_by_type[t]);
    }
  }
  for (int t = 1; t <= 16; t++) {
    if (edge_by_type[t] > 0) {
      fprintf(stderr, "[JVM GRAPH SUMMARY] edge_type=%s count=%ld\n", sg_edge_type_name(t), edge_by_type[t]);
    }
  }
  pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// Semantic JSONL export  (soroush_graph_export_runtime_targets)
//
// Invoked at VM shutdown from before_exit when SOROUSH_EXPORT_RUNTIME_TARGETS
// is set.  Writes one JSON object per line to the given file path.
//
// Semantic record types (primary API surface):
//   method_identity    -- every instrumented method registered in the token
//                         registry (exact class / method / descriptor /
//                         loader_id / hidden / artifact_crc).
//   runtime_target     -- a dynamic callsite resolved to an actual target.
//                         evidence=LINKAGE_GUARANTEED comes from the runtime
//                         signal's node type (MH/reflection linkage, DMH
//                         structure walk at link time) — NOT inferred from the
//                         edge type alone.  evidence=OBSERVED_ONLY comes from
//                         instrumented ENTER events (execution_trace).
//   generated_class    -- a runtime-generated class with provenance.
//   bytecode_artifact  -- a final (or original) executable bytecode snapshot.
//   diagnostic         -- emitted when a record cannot be constructed exactly;
//                         explains what was skipped and why.
//   export_summary     -- last line; counts per record type.
//
// Reliability invariants:
//   - No fake precision: if a method identity cannot be fully resolved from the
//     graph, a diagnostic record is emitted and the target record is omitted.
//   - No loader_id=0 fabrication, no guessed descriptors, no inferred targets.
//   - EXECUTES records are deduplicated per unique Method node (one record per
//     distinct method observed to execute, regardless of call count).
//   - Fail-safe: file I/O errors are reported to stderr; the VM never aborts.
//   - Graph arrays are append-only; at VM shutdown Java threads are quiescent,
//     so counts are snapshotted under their locks and the arrays walked
//     lock-free.  Token strings are immortal (never freed / reallocated after
//     publication).
// ---------------------------------------------------------------------------

// Write a JSON-safe quoted string to f.  Escapes only the characters that
// must be escaped in JSON: " \ and control chars < 0x20.  Java identifiers,
// internal class names, and JVM descriptors never contain these, so for those
// inputs this is effectively a no-op beyond the surrounding quotes.
static void sg_json_str(FILE* f, const char* s) {
  if (s == nullptr) { fputs("null", f); return; }
  fputc('"', f);
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
    else if (*p < 0x20) { fprintf(f, "\\u%04x", (unsigned)*p); }
    else fputc(*p, f);
  }
  fputc('"', f);
}

// Extract the value of a "key=value" field from a label string.  The value
// runs from the end of field_eq to the next ASCII space (or end of string).
// Returns true on success; always NUL-terminates out.
static bool sg_export_label_field(const char* label, const char* field_eq,
                                   char* out, size_t out_len) {
  out[0] = '\0';
  if (label == nullptr || field_eq == nullptr || out_len == 0) return false;
  const char* p = strstr(label, field_eq);
  if (p == nullptr) return false;
  p += strlen(field_eq);
  const char* end = strchr(p, ' ');
  size_t len = (end != nullptr) ? (size_t)(end - p) : strlen(p);
  if (len >= out_len) len = out_len - 1;
  memcpy(out, p, len);
  out[len] = '\0';
  return len > 0;
}

// Parse a Method node's key "M|<class_node_id>|<method_name>|<descriptor>"
// and look up the referenced Class node to reconstruct loader_id and hidden.
// The class node's label is the normalized (slash-form) class name.
// Called with the graph quiescent (shutdown) or under g_lock.
// Returns false and sets all out-params to safe defaults on any parse error;
// does NOT fabricate missing information.
static bool sg_export_parse_method_node(const SgNode* mnode, long node_snap,
                                         char* out_class,  size_t class_len,
                                         char* out_method, size_t method_len,
                                         char* out_desc,   size_t desc_len,
                                         uint64_t* out_loader_id, int* out_hidden) {
  *out_loader_id = 0; *out_hidden = 0;
  if (class_len)  out_class[0]  = '\0';
  if (method_len) out_method[0] = '\0';
  if (desc_len)   out_desc[0]   = '\0';

  if (mnode == nullptr || mnode->key == nullptr) return false;
  if (mnode->type != SG_NODE_METHOD) return false;
  const char* k = mnode->key;
  if (k[0] != 'M' || k[1] != '|') return false;

  const char* p = k + 2;
  char* end_ptr;
  unsigned long long cid = strtoull(p, &end_ptr, 10);
  if (end_ptr == p || *end_ptr != '|' || cid == 0 || (long)cid > node_snap) return false;

  const SgNode* cls = &g_nodes[(long)cid - 1];
  if (cls->type != SG_NODE_CLASS && cls->type != SG_NODE_GENERATED_CLASS) return false;

  strncpy(out_class, cls->label ? cls->label : "?", class_len - 1);
  out_class[class_len - 1] = '\0';
  *out_loader_id = cls->loader_id;
  *out_hidden    = cls->flags & 1;

  p = end_ptr + 1;  // skip '|' after class_id

  // Method name runs up to the LAST '|' (Java method names and JVM descriptors
  // never contain '|', so strrchr correctly finds the name/descriptor separator).
  const char* last_pipe = strrchr(p, '|');
  if (last_pipe == nullptr || last_pipe == p) return false;

  size_t mlen = (size_t)(last_pipe - p);
  if (mlen == 0 || mlen >= method_len) return false;
  memcpy(out_method, p, mlen);
  out_method[mlen] = '\0';

  const char* desc_start = last_pipe + 1;
  if (desc_start[0] == '\0') return false;   // empty descriptor → reject
  strncpy(out_desc, desc_start, desc_len - 1);
  out_desc[desc_len - 1] = '\0';

  return true;
}

void soroush_graph_export_runtime_targets(const char* path) {
  // Fail-safe: any internal error writes a diagnostic to stderr and/or to
  // the file but never aborts the VM.
  FILE* f = fopen(path, "w");
  if (f == nullptr) {
    fprintf(stderr, "[JVM EXPORT] error: cannot open %s for writing\n", path);
    return;
  }
  fprintf(stderr, "[JVM EXPORT] writing runtime targets to %s\n", path);

  long mi_count = 0, ct_count = 0, rt_count = 0, gc_count = 0, ba_count = 0, diag_count = 0;
  bool write_ok = true; // set false if any fprintf write fails (ferror)

  // -------------------------------------------------------------------------
  // Phase 1: method_identity records from the token registry.
  //
  // The token registry is always active when instrumentation runs, independent
  // of SOROUSH_PROVENANCE_GRAPH.  Tokens are 1-based; slot 0 is unused.
  // Strings are immortal (never freed); safe to read lock-free after
  // snapshotting the count.
  // -------------------------------------------------------------------------
  pthread_mutex_lock(&g_token_lock);
  uint32_t token_snap = g_token_count;
  pthread_mutex_unlock(&g_token_lock);

  for (uint32_t t = 1; t <= token_snap; t++) {
    if (g_tokens == nullptr || t >= g_token_cap) {
      // Should not happen (token_cap >= token_count always), but be safe.
      fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"error\","
              "\"message\":\"token registry pointer null or index %u out of capacity; export aborted\"}\n",
              (unsigned)t);
      diag_count++; break;
    }
    SgMethodToken* tok = &g_tokens[t];
    if (tok->dotted_class == nullptr || tok->method == nullptr || tok->descriptor == nullptr) {
      fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"warn\","
              "\"message\":\"token %u has null identity fields; skipped\"}\n", (unsigned)t);
      diag_count++; continue;
    }
    char cls_slash[1024];
    sg_internalize(cls_slash, sizeof(cls_slash), tok->dotted_class);
    fprintf(f, "{\"record\":\"method_identity\",\"token\":%u,\"class\":", (unsigned)t);
    sg_json_str(f, cls_slash);
    fprintf(f, ",\"method\":");
    sg_json_str(f, tok->method);
    fprintf(f, ",\"descriptor\":");
    sg_json_str(f, tok->descriptor);
    fprintf(f, ",\"loader_id\":\"0x%016llx\",\"hidden\":%s,\"artifact_crc\":\"%08x\"}\n",
            (unsigned long long)tok->loader_id,
            tok->hidden ? "true" : "false",
            tok->artifact_crc);
    mi_count++;
  }

  if (ferror(f)) write_ok = false;

  // -------------------------------------------------------------------------
  // Phase 2: callsite_target records from the indy callsite side table.
  //
  // Populated by soroush_graph_indy_callsite(), which requires
  // SOROUSH_PROVENANCE_GRAPH=1.  When the graph is disabled the table is
  // empty (g_indy_site_count == 0) so the loop below produces 0 records.
  //
  // Each record represents one invokedynamic call site, with:
  //   source_class / source_loader_id / source_method / source_descriptor /
  //   source_bci / source_opcode / source_cp_index — exact when
  //   source_capture="exact" (interpreter frame was readable).
  //   source_capture="compiled_frame_bci_unavailable" when the frame was not
  //   an interpreter frame; bci and method/descriptor fields are absent.
  //   evidence="LINKAGE_GUARANTEED" — captured at JVM bootstrap linkage time.
  //   trace_id — joins with generated_class.indy_trace_id.
  //   lmf_impl_* — impl method when the BSM is LambdaMetafactory.
  // -------------------------------------------------------------------------
  {
    pthread_mutex_lock(&g_indy_site_lock);
    uint32_t site_snap = g_indy_site_count;
    pthread_mutex_unlock(&g_indy_site_lock);

    // Lock-free walk: g_indy_sites[1..site_snap] is stable at VM shutdown
    // (Java threads are quiescent; no new linkage events). Strings are immortal.
    for (uint32_t tid = 1; tid <= site_snap; tid++) {
      if (g_indy_sites == nullptr || tid >= g_indy_site_cap) break;
      SgIndySite* s = &g_indy_sites[tid];
      if (!s->valid) continue;

      bool exact_source = s->frame_captured && s->src_bci >= 0
                          && s->src_method != nullptr && s->src_desc != nullptr;

      fprintf(f, "{\"record\":\"callsite_target\","
              "\"category\":\"invokedynamic\","
              "\"trace_id\":%u,"
              "\"evidence\":\"LINKAGE_GUARANTEED\","
              "\"source_class\":", (unsigned)tid);
      sg_json_str(f, s->src_class);
      fprintf(f, ",\"source_loader_id\":\"0x%016llx\"",
              (unsigned long long)s->src_loader_id);
      fprintf(f, ",\"source_capture\":\"%s\"",
              exact_source ? "exact" : "compiled_frame_bci_unavailable");
      if (s->src_method != nullptr) {
        fprintf(f, ",\"source_method\":");
        sg_json_str(f, s->src_method);
      }
      if (s->src_desc != nullptr) {
        fprintf(f, ",\"source_descriptor\":");
        sg_json_str(f, s->src_desc);
      }
      if (s->src_bci >= 0) {
        fprintf(f, ",\"source_bci\":%d", s->src_bci);
      }
      fprintf(f, ",\"source_opcode\":\"invokedynamic\"");
      fprintf(f, ",\"source_cp_index\":%d", s->src_bss_index);
      fprintf(f, ",\"indy_name\":");
      sg_json_str(f, s->indy_name);
      fprintf(f, ",\"indy_descriptor\":");
      sg_json_str(f, s->indy_sig);
      fprintf(f, ",\"bootstrap_method\":");
      sg_json_str(f, s->bootstrap);
      if (s->lmf_impl_cls != nullptr) {
        fprintf(f, ",\"lmf_impl_class\":");
        sg_json_str(f, s->lmf_impl_cls);
        fprintf(f, ",\"lmf_impl_method\":");
        sg_json_str(f, s->lmf_impl_mth);
        fprintf(f, ",\"lmf_impl_descriptor\":");
        sg_json_str(f, s->lmf_impl_dsc);
      }
      fprintf(f, "}\n");
      ct_count++;
    }
  }

  if (ferror(f)) write_ok = false;

  // -------------------------------------------------------------------------
  // Phase 3: generic callsite_target / diagnostic records from the
  // reflection / MethodHandle side table (g_gen_buckets).
  //
  // Populated by soroush_graph_generic_callsite() from hooks in jvm.cpp
  // (JVM_InvokeMethod, JVM_NewInstanceFromConstructor) and linkResolver.cpp
  // (resolve_handle_call).  Deduped per (category, src_class, src_method,
  // src_desc, src_bci); first-in-wins.
  //
  // exact=true  → callsite_target record (source + target both exact).
  // exact=false → diagnostic record (explains what was not captured).
  //
  // Part C dedup: diagnostic records are suppressed when an exact record
  // (callsite_target, callsite_target_set, callsite_adapter_graph) already
  // exists for the same (src_class, src_method, src_bci).  This ensures that
  // sibling-BCI diagnostics emitted at CP-resolution time are superseded by
  // the exact records produced later via LF-frame attribution (Part A).
  // -------------------------------------------------------------------------
  {
    // ---- Build exact-callsite dedup set ----
    // FNV-1a hash of (src_class || "|" || src_method || "|" || bci).
    // Covers g_gen_buckets (exact=true), g_ag_buckets, and g_ts_buckets.
    // Uses a flat open-chaining table; entries are malloc'd (export-time only).
    struct SgDedupEntry { uint32_t h; const char* cls; const char* meth; int bci;
                          struct SgDedupEntry* nxt; };
#define SG_DEDUP_BKTS 512u
    SgDedupEntry* sg_ded[SG_DEDUP_BKTS];
    memset(sg_ded, 0, sizeof(sg_ded));

    auto sg_ded_hash = [](const char* cls, const char* meth, int bci) -> uint32_t {
      uint32_t h = 2166136261u;
      for (const char* p = cls;  p && *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
      h = (h ^ '|') * 16777619u;
      for (const char* p = meth; p && *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
      h = (h ^ (uint32_t)(unsigned)bci) * 16777619u;
      return h;
    };
    auto sg_ded_add = [&](const char* cls, const char* meth, int bci) {
      if (!cls || !meth || bci < 0) return;
      uint32_t h = sg_ded_hash(cls, meth, bci);
      uint32_t s = h % SG_DEDUP_BKTS;
      for (SgDedupEntry* e = sg_ded[s]; e; e = e->nxt)
        if (e->h == h && e->bci == bci &&
            strcmp(e->cls, cls) == 0 && strcmp(e->meth, meth) == 0) return;
      SgDedupEntry* ne = (SgDedupEntry*)malloc(sizeof(SgDedupEntry));
      if (!ne) return;
      ne->h = h; ne->cls = cls; ne->meth = meth; ne->bci = bci;
      ne->nxt = sg_ded[s]; sg_ded[s] = ne;
    };
    auto sg_ded_has = [&](const char* cls, const char* meth, int bci) -> bool {
      if (!cls || !meth || bci < 0) return false;
      uint32_t h = sg_ded_hash(cls, meth, bci);
      uint32_t s = h % SG_DEDUP_BKTS;
      for (SgDedupEntry* e = sg_ded[s]; e; e = e->nxt)
        if (e->h == h && e->bci == bci &&
            strcmp(e->cls, cls) == 0 && strcmp(e->meth, meth) == 0) return true;
      return false;
    };
    // Populate from exact callsite_target records in g_gen_buckets
    for (uint32_t b = 0; b < SG_GEN_BUCKETS; b++)
      for (SgGenCallsite* c = g_gen_buckets[b]; c; c = c->next)
        if (c->exact) sg_ded_add(c->src_class, c->src_method, c->src_bci);
    // Populate from callsite_adapter_graph records (g_ag_buckets)
    for (uint32_t b = 0; b < SG_AG_BUCKETS; b++)
      for (SgAgCallsite* c = g_ag_buckets[b]; c; c = c->next)
        sg_ded_add(c->src_class, c->src_method, c->src_bci);
    // Populate from callsite_target_set records (g_ts_buckets)
    for (uint32_t b = 0; b < SG_TS_BUCKETS; b++)
      for (SgTsCallsite* c = g_ts_buckets[b]; c; c = c->next)
        sg_ded_add(c->src_class, c->src_method, c->src_bci);

    // Snapshot count under lock; walk buckets lock-free at shutdown.
    pthread_mutex_lock(&g_gen_lock);
    uint32_t gen_snap = g_gen_count;
    pthread_mutex_unlock(&g_gen_lock);

    if (gen_snap > 0) {
      for (uint32_t b = 0; b < SG_GEN_BUCKETS; b++) {
        for (SgGenCallsite* c = g_gen_buckets[b]; c != nullptr; c = c->next) {
          if (c->exact) {
            // Opcode name lookup (Java bytecode values).
            const char* op_name = "invokevirtual"; // default for reflect/MH
            switch (c->opcode_byte) {
              case 0xb6: op_name = "invokevirtual";  break;
              case 0xb7: op_name = "invokespecial";  break;
              case 0xb8: op_name = "invokestatic";   break;
              case 0xb9: op_name = "invokeinterface"; break;
              case 0xba: op_name = "invokedynamic";  break;
            }
            fprintf(f, "{\"record\":\"callsite_target\",\"category\":");
            sg_json_str(f, c->category);
            fprintf(f, ",\"evidence\":\"OBSERVED_ONLY\","
                    "\"source_class\":");
            sg_json_str(f, c->src_class);
            fprintf(f, ",\"source_loader_id\":\"0x%016llx\","
                    "\"source_capture\":\"exact\","
                    "\"source_method\":",
                    (unsigned long long)c->src_loader_id);
            sg_json_str(f, c->src_method);
            fprintf(f, ",\"source_descriptor\":");
            sg_json_str(f, c->src_desc);
            fprintf(f, ",\"source_bci\":%d", c->src_bci);
            fprintf(f, ",\"source_opcode\":");
            sg_json_str(f, op_name);
            if (c->cp_index >= 0)
              fprintf(f, ",\"source_cp_index\":%d", c->cp_index);
            fprintf(f, ",\"target_class\":");
            sg_json_str(f, c->target_class);
            fprintf(f, ",\"target_loader_id\":\"0x%016llx\","
                    "\"target_method\":",
                    (unsigned long long)c->target_loader_id);
            sg_json_str(f, c->target_method);
            fprintf(f, ",\"target_descriptor\":");
            sg_json_str(f, c->target_desc);
            fprintf(f, "}\n");
            ct_count++;
          } else {
            // Part C dedup: suppress diagnostic if an exact record exists for
            // the same (src_class, src_method, src_bci).  Exact records produced
            // via LF-frame attribution (Part A) supersede sibling-BCI diagnostics.
            if (sg_ded_has(c->src_class, c->src_method, c->src_bci)) {
              continue;
            }
            // Emit diagnostic with all source fields that ARE known.
            // The opcode name lookup (same table as exact records above).
            const char* d_op_name = nullptr;
            if (c->opcode_byte != 0) {
              switch (c->opcode_byte) {
                case 0xb6: d_op_name = "invokevirtual";   break;
                case 0xb7: d_op_name = "invokespecial";   break;
                case 0xb8: d_op_name = "invokestatic";    break;
                case 0xb9: d_op_name = "invokeinterface"; break;
                case 0xba: d_op_name = "invokedynamic";   break;
                default:   d_op_name = nullptr;
              }
            }
            fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"info\","
                    "\"category\":");
            sg_json_str(f, c->category);
            fprintf(f, ",\"reason\":");
            sg_json_str(f, c->diag_reason);
            if (c->src_class) {
              fprintf(f, ",\"src_class\":");
              sg_json_str(f, c->src_class);
              fprintf(f, ",\"src_loader_id\":\"0x%016llx\"",
                      (unsigned long long)c->src_loader_id);
            }
            if (c->src_method) {
              fprintf(f, ",\"src_method\":");
              sg_json_str(f, c->src_method);
            }
            if (c->src_desc) {
              fprintf(f, ",\"src_descriptor\":");
              sg_json_str(f, c->src_desc);
            }
            if (c->src_bci >= 0)
              fprintf(f, ",\"src_bci\":%d", c->src_bci);
            if (d_op_name != nullptr) {
              fprintf(f, ",\"src_opcode\":");
              sg_json_str(f, d_op_name);
            }
            if (c->cp_index >= 0)
              fprintf(f, ",\"src_cp_index\":%d", c->cp_index);
            if (c->target_class) {
              fprintf(f, ",\"target_class\":");
              sg_json_str(f, c->target_class);
              fprintf(f, ",\"target_loader_id\":\"0x%016llx\"",
                      (unsigned long long)c->target_loader_id);
              fprintf(f, ",\"target_method\":");
              sg_json_str(f, c->target_method);
              if (c->target_desc) {
                fprintf(f, ",\"target_descriptor\":");
                sg_json_str(f, c->target_desc);
              }
            }
            fprintf(f, "}\n");
            diag_count++;
          }
        }
      }
    }

    // Free the dedup table entries
    for (uint32_t b = 0; b < SG_DEDUP_BKTS; b++) {
      for (SgDedupEntry* e = sg_ded[b]; e; ) {
        SgDedupEntry* nx = e->nxt;
        free(e);
        e = nx;
      }
    }
#undef SG_DEDUP_BKTS
  }

  if (ferror(f)) write_ok = false;

  // -------------------------------------------------------------------------
  // Phase 3.5: callsite_target_set records from the multi-target MH side table
  // (g_ts_buckets).  Each record is a guardWithTest or catchException callsite
  // with all semantic targets listed.  Active when SOROUSH_PROVENANCE_GRAPH=1.
  // -------------------------------------------------------------------------
  {
    pthread_mutex_lock(&g_ts_lock);
    uint32_t ts_snap = g_ts_count;
    pthread_mutex_unlock(&g_ts_lock);

    if (ts_snap > 0) {
      for (uint32_t b = 0; b < SG_TS_BUCKETS; b++) {
        for (SgTsCallsite* c = g_ts_buckets[b]; c != nullptr; c = c->next) {
          const char* op_name = "invokevirtual";
          switch (c->opcode_byte) {
            case 0xb6: op_name = "invokevirtual";   break;
            case 0xb7: op_name = "invokespecial";   break;
            case 0xb8: op_name = "invokestatic";    break;
            case 0xb9: op_name = "invokeinterface"; break;
            case 0xba: op_name = "invokedynamic";   break;
          }
          fprintf(f, "{\"record\":\"callsite_target_set\",\"category\":");
          sg_json_str(f, c->category);
          fprintf(f, ",\"adapter_shape\":");
          sg_json_str(f, c->adapter_shape);
          if (c->adapter_class) {
            fprintf(f, ",\"adapter_class\":");
            sg_json_str(f, c->adapter_class);
          }
          if (c->lf_kind) {
            fprintf(f, ",\"lf_kind\":");
            sg_json_str(f, c->lf_kind);
          }
          if (c->aux_info) {
            fprintf(f, ",\"exception_class\":");
            sg_json_str(f, c->aux_info);
          }
          if (c->semantic_op) {
            fprintf(f, ",\"semantic_op\":");
            sg_json_str(f, c->semantic_op);
          }
          fprintf(f, ",\"staticizable\":%s", c->staticizable ? "true" : "false");
          if (c->n_staticization_blockers > 0) {
            fprintf(f, ",\"staticization_blockers\":[");
            for (int bi = 0; bi < c->n_staticization_blockers; bi++) {
              if (bi > 0) fprintf(f, ",");
              sg_json_str(f, c->staticization_blockers[bi]);
            }
            fprintf(f, "]");
          }
          fprintf(f, ",\"evidence\":\"OBSERVED_ONLY\",\"source_class\":");
          sg_json_str(f, c->src_class);
          fprintf(f, ",\"source_loader_id\":\"0x%016llx\","
                  "\"source_capture\":\"exact\",\"source_method\":",
                  (unsigned long long)c->src_loader_id);
          sg_json_str(f, c->src_method);
          fprintf(f, ",\"source_descriptor\":");
          sg_json_str(f, c->src_desc);
          fprintf(f, ",\"source_bci\":%d,\"source_opcode\":", c->src_bci);
          sg_json_str(f, op_name);
          if (c->cp_index >= 0)
            fprintf(f, ",\"source_cp_index\":%d", c->cp_index);
          fprintf(f, ",\"targets\":[");
          for (int i = 0; i < c->n_targets; i++) {
            SgTsTarget* t = &c->targets[i];
            if (i > 0) fprintf(f, ",");
            fprintf(f, "{\"role\":");
            sg_json_str(f, t->role);
            fprintf(f, ",\"valid\":%s", t->valid ? "true" : "false");
            if (t->klass) {
              fprintf(f, ",\"class\":");
              sg_json_str(f, t->klass);
              fprintf(f, ",\"loader_id\":\"0x%016llx\"",
                      (unsigned long long)t->loader_id);
            }
            if (t->method) {
              fprintf(f, ",\"method\":");
              sg_json_str(f, t->method);
            }
            if (t->descriptor) {
              fprintf(f, ",\"descriptor\":");
              sg_json_str(f, t->descriptor);
            }
            fprintf(f, "}");
          }
          fprintf(f, "]}\n");
          ct_count++;
        }
      }
    }
  }

  if (ferror(f)) write_ok = false;

  // -------------------------------------------------------------------------
  // Phase 3.6: callsite_adapter_graph records from the GENERIC/named BMH
  // adapter graph side table (g_ag_buckets).  Each record describes the
  // full adapter structure with per-node exact targets where provable.
  // Active when SOROUSH_PROVENANCE_GRAPH=1 (no graph required).
  // -------------------------------------------------------------------------
  {
    pthread_mutex_lock(&g_ag_lock);
    uint32_t ag_snap = g_ag_count;
    pthread_mutex_unlock(&g_ag_lock);

    if (ag_snap > 0) {
      for (uint32_t b = 0; b < SG_AG_BUCKETS; b++) {
        for (SgAgCallsite* c = g_ag_buckets[b]; c != nullptr; c = c->next) {
          const char* op_name = "invokevirtual";
          switch (c->opcode_byte) {
            case 0xb6: op_name = "invokevirtual";   break;
            case 0xb7: op_name = "invokespecial";   break;
            case 0xb8: op_name = "invokestatic";    break;
            case 0xb9: op_name = "invokeinterface"; break;
            case 0xba: op_name = "invokedynamic";   break;
          }
          fprintf(f, "{\"record\":\"callsite_adapter_graph\",\"category\":");
          sg_json_str(f, c->category);
          if (c->adapter_class) {
            fprintf(f, ",\"adapter_class\":");
            sg_json_str(f, c->adapter_class);
          }
          if (c->adapter_kind) {
            fprintf(f, ",\"adapter_kind\":");
            sg_json_str(f, c->adapter_kind);
          }
          if (c->lf_kind) {
            fprintf(f, ",\"lf_kind\":");
            sg_json_str(f, c->lf_kind);
          }
          if (c->outer_desc) {
            fprintf(f, ",\"outer_descriptor\":");
            sg_json_str(f, c->outer_desc);
          }
          fprintf(f, ",\"evidence\":\"OBSERVED_ONLY\",\"all_exact\":%s",
                  c->all_exact ? "true" : "false");
          fprintf(f, ",\"source_class\":");
          sg_json_str(f, c->src_class);
          fprintf(f, ",\"source_loader_id\":\"0x%016llx\","
                  "\"source_capture\":\"exact\",\"source_method\":",
                  (unsigned long long)c->src_loader_id);
          sg_json_str(f, c->src_method);
          fprintf(f, ",\"source_descriptor\":");
          sg_json_str(f, c->src_desc);
          fprintf(f, ",\"source_bci\":%d,\"source_opcode\":", c->src_bci);
          sg_json_str(f, op_name);
          if (c->cp_index >= 0)
            fprintf(f, ",\"source_cp_index\":%d", c->cp_index);
          fprintf(f, ",\"nodes\":[");
          for (int i = 0; i < c->n_nodes; i++) {
            SgAgNode* n = &c->nodes[i];
            if (i > 0) fprintf(f, ",");
            fprintf(f, "{\"id\":%d,\"role\":", n->id);
            sg_json_str(f, n->role);
            if (n->from_desc) {
              fprintf(f, ",\"from_descriptor\":");
              sg_json_str(f, n->from_desc);
            }
            if (n->to_desc) {
              fprintf(f, ",\"to_descriptor\":");
              sg_json_str(f, n->to_desc);
            }
            fprintf(f, ",\"exact\":%s", n->exact ? "true" : "false");
            if (n->node_classification) {
              fprintf(f, ",\"classification\":");
              sg_json_str(f, n->node_classification);
            }
            if (n->node_adapter_class) {
              fprintf(f, ",\"adapter_class\":");
              sg_json_str(f, n->node_adapter_class);
            }
            if (!n->exact && n->exact_false_reason) {
              fprintf(f, ",\"exact_false_reason\":");
              sg_json_str(f, n->exact_false_reason);
            }
            if (n->semantic_op) {
              fprintf(f, ",\"semantic_op\":");
              sg_json_str(f, n->semantic_op);
            }
            if (n->from_type) {
              fprintf(f, ",\"from_type\":");
              sg_json_str(f, n->from_type);
            }
            if (n->to_type) {
              fprintf(f, ",\"to_type\":");
              sg_json_str(f, n->to_type);
            }
            if (n->exact && n->klass) {
              fprintf(f, ",\"class\":");
              sg_json_str(f, n->klass);
              fprintf(f, ",\"loader_id\":\"0x%016llx\"",
                      (unsigned long long)n->loader_id);
              if (n->method) {
                fprintf(f, ",\"method\":");
                sg_json_str(f, n->method);
              }
              if (n->descriptor) {
                fprintf(f, ",\"descriptor\":");
                sg_json_str(f, n->descriptor);
              }
            }
            fprintf(f, "}");
          }
          fprintf(f, "]");
          if (c->n_edges > 0) {
            fprintf(f, ",\"edges\":[");
            for (int i = 0; i < c->n_edges; i++) {
              if (i > 0) fprintf(f, ",");
              fprintf(f, "{\"from\":%d,\"to\":%d,\"kind\":",
                      c->edges[i].from_id, c->edges[i].to_id);
              sg_json_str(f, c->edges[i].kind);
              fprintf(f, "}");
            }
            fprintf(f, "]");
          }
          fprintf(f, ",\"staticizable\":%s", c->staticizable ? "true" : "false");
          if (c->n_staticization_blockers > 0) {
            fprintf(f, ",\"staticization_blockers\":[");
            for (int i = 0; i < c->n_staticization_blockers; i++) {
              if (i > 0) fprintf(f, ",");
              sg_json_str(f, c->staticization_blockers[i]);
            }
            fprintf(f, "]");
          }
          fprintf(f, "}\n");
          ct_count++;
        }
      }
    }
  }

  if (ferror(f)) write_ok = false;

  // -------------------------------------------------------------------------
  // Phase 4-7: graph-derived records.
  //
  // Requires SOROUSH_PROVENANCE_GRAPH=1.  If the graph is disabled, emit a
  // single diagnostic and skip to the summary (method_identity records above
  // are still written).
  // -------------------------------------------------------------------------
  if (!soroush_graph_enabled()) {
    fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"info\","
            "\"message\":\"SOROUSH_PROVENANCE_GRAPH=1 required for runtime_target,"
            " generated_class, bytecode_artifact records; graph is disabled\"}\n");
    diag_count++;
  } else {
    // Snapshot counts under their lock; then walk the arrays lock-free.
    // The graph is append-only and at VM shutdown Java threads are quiescent,
    // so elements 0..snap-1 are immutable.
    pthread_mutex_lock(&g_lock);
    long node_snap = g_node_count;
    long edge_snap = g_edge_count;
    SgNode* nodes  = g_nodes;
    SgEdge* edges  = g_edges;
    pthread_mutex_unlock(&g_lock);

    if (nodes == nullptr || edge_snap == 0 || node_snap == 0) {
      fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"info\","
              "\"message\":\"graph is empty; no graph-derived records\"}\n");
      diag_count++;
    } else {
      // Allocate three per-node helper arrays (indexed by node id, 1-based).
      // At 1M max nodes: rewritten_from ~8 MB, indy_for_gen ~4 MB,
      // observed_methods ~1 MB.  All zeroed; freed before the function returns.
      uint64_t* rewritten_from   = (uint64_t*)calloc((size_t)(node_snap + 1), sizeof(uint64_t));
      int*      indy_for_gen     = (int*)     calloc((size_t)(node_snap + 1), sizeof(int));
      bool*     observed_methods = (bool*)    calloc((size_t)(node_snap + 1), 1);

      if (!rewritten_from || !indy_for_gen || !observed_methods) {
        free(rewritten_from); free(indy_for_gen); free(observed_methods);
        fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"error\","
                "\"message\":\"OOM allocating export helpers (%ld nodes);"
                " graph-derived records skipped\"}\n", node_snap);
        diag_count++;
        goto sg_export_summary;
      }

      // -----------------------------------------------------------------------
      // Edge walk:
      //   - Build rewritten_from[final_artifact_id] = original_artifact_id.
      //   - Build indy_for_gen[gen_class_id] = indy_trace_id.
      //   - Emit runtime_target records for LINKS_TO, RESOLVES_TO, EXECUTES.
      //
      // Evidence classification is determined by the SOURCE NODE TYPE of each
      // edge, which is set at signal-emission time (not inferred from the edge
      // type alone):
      //   SG_NODE_REFLECTION_INVOKE   -> LINKAGE_GUARANTEED (MemberName.resolve)
      //   SG_NODE_METHODHANDLE_LINKAGE -> LINKAGE_GUARANTEED (MH link-time)
      //   SG_NODE_MH_ADAPTER           -> LINKAGE_GUARANTEED (DMH structure walk)
      //   SG_NODE_EXECUTION            -> OBSERVED_ONLY      (instrumented ENTER)
      // -----------------------------------------------------------------------
      for (long ei = 0; ei < edge_snap; ei++) {
        SgEdge* e = &edges[ei];
        if (e->from == 0 || e->to == 0) continue;
        if ((long)e->from > node_snap || (long)e->to > node_snap) continue;
        SgNode* src = &nodes[(long)e->from - 1];
        SgNode* tgt = &nodes[(long)e->to   - 1];

        // -- REWRITTEN_FROM: build lookup for bytecode_artifact phase ----------
        if (e->type == SG_EDGE_REWRITTEN_FROM &&
            src->type == SG_NODE_BYTECODE_ARTIFACT &&
            tgt->type == SG_NODE_BYTECODE_ARTIFACT) {
          rewritten_from[src->id] = tgt->id;
          continue;
        }

        // -- CREATED_BY: GeneratedClass -> IndyCallSite (indy_trace_id join) --
        if (e->type == SG_EDGE_CREATED_BY &&
            src->type == SG_NODE_GENERATED_CLASS &&
            tgt->type == SG_NODE_INDY_CALLSITE &&
            tgt->key != nullptr && tgt->key[0] == 'I' && tgt->key[1] == '|') {
          int tid = atoi(tgt->key + 2);
          if (tid > 0 && src->id <= (uint64_t)node_snap) {
            indy_for_gen[src->id] = tid;
          }
          continue;
        }

        // -- LINKS_TO from ReflectionInvoke/MHLinkage -> Method ---------------
        // Evidence: LINKAGE_GUARANTEED (source node type encodes the signal).
        if (e->type == SG_EDGE_LINKS_TO &&
            (src->type == SG_NODE_REFLECTION_INVOKE ||
             src->type == SG_NODE_METHODHANDLE_LINKAGE) &&
            tgt->type == SG_NODE_METHOD) {
          char t_class[512], t_method[256], t_desc[512];
          uint64_t t_loader = 0; int t_hidden = 0;
          if (!sg_export_parse_method_node(tgt, node_snap,
                                           t_class, sizeof(t_class),
                                           t_method, sizeof(t_method),
                                           t_desc,   sizeof(t_desc),
                                           &t_loader, &t_hidden)) {
            fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"warn\","
                    "\"message\":\"LINKS_TO edge: cannot resolve target method node"
                    " " UINT64_FORMAT "; record omitted\","
                    "\"edge_from\":" UINT64_FORMAT ",\"edge_to\":" UINT64_FORMAT "}\n",
                    (uint64_t)tgt->id, (uint64_t)e->from, (uint64_t)e->to);
            diag_count++; continue;
          }
          char caller_ctx[256] = "?";
          sg_export_label_field(src->label, "src=", caller_ctx, sizeof(caller_ctx));
          const char* dk = (src->type == SG_NODE_REFLECTION_INVOKE)
                            ? "reflection" : "methodhandle_linkage";
          fprintf(f, "{\"record\":\"runtime_target\","
                  "\"evidence\":\"LINKAGE_GUARANTEED\","
                  "\"dispatch_kind\":\"%s\","
                  "\"caller_context\":", dk);
          sg_json_str(f, caller_ctx);
          fprintf(f, ",\"target_class\":");
          sg_json_str(f, t_class);
          fprintf(f, ",\"target_method\":");
          sg_json_str(f, t_method);
          fprintf(f, ",\"target_descriptor\":");
          sg_json_str(f, t_desc);
          fprintf(f, ",\"target_loader_id\":\"0x%016llx\",\"target_hidden\":%s}\n",
                  (unsigned long long)t_loader, t_hidden ? "true" : "false");
          rt_count++; continue;
        }

        // -- RESOLVES_TO from MHAdapter -> Method -----------------------------
        // Evidence: LINKAGE_GUARANTEED (native DMH structure walk at link time).
        if (e->type == SG_EDGE_RESOLVES_TO &&
            src->type == SG_NODE_MH_ADAPTER &&
            tgt->type == SG_NODE_METHOD) {
          char t_class[512], t_method[256], t_desc[512];
          uint64_t t_loader = 0; int t_hidden = 0;
          if (!sg_export_parse_method_node(tgt, node_snap,
                                           t_class, sizeof(t_class),
                                           t_method, sizeof(t_method),
                                           t_desc,   sizeof(t_desc),
                                           &t_loader, &t_hidden)) {
            fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"warn\","
                    "\"message\":\"RESOLVES_TO edge: cannot resolve target method node"
                    " " UINT64_FORMAT "; record omitted\","
                    "\"edge_from\":" UINT64_FORMAT ",\"edge_to\":" UINT64_FORMAT "}\n",
                    (uint64_t)tgt->id, (uint64_t)e->from, (uint64_t)e->to);
            diag_count++; continue;
          }
          // MHAdapter key: "MH|<mh_kind>|<lf_name>" — extract mh_kind.
          char mh_kind[256] = "?";
          if (src->key != nullptr && strncmp(src->key, "MH|", 3) == 0) {
            const char* k2 = src->key + 3;
            const char* p2 = strchr(k2, '|');
            size_t klen = p2 ? (size_t)(p2 - k2) : strlen(k2);
            if (klen >= sizeof(mh_kind)) klen = sizeof(mh_kind) - 1;
            memcpy(mh_kind, k2, klen); mh_kind[klen] = '\0';
          }
          fprintf(f, "{\"record\":\"runtime_target\","
                  "\"evidence\":\"LINKAGE_GUARANTEED\","
                  "\"dispatch_kind\":\"direct_methodhandle\","
                  "\"caller_context\":");
          sg_json_str(f, mh_kind);
          fprintf(f, ",\"target_class\":");
          sg_json_str(f, t_class);
          fprintf(f, ",\"target_method\":");
          sg_json_str(f, t_method);
          fprintf(f, ",\"target_descriptor\":");
          sg_json_str(f, t_desc);
          fprintf(f, ",\"target_loader_id\":\"0x%016llx\",\"target_hidden\":%s}\n",
                  (unsigned long long)t_loader, t_hidden ? "true" : "false");
          rt_count++; continue;
        }

        // -- EXECUTES from Execution -> Method --------------------------------
        // Evidence: OBSERVED_ONLY (instrumented ENTER event at runtime).
        // Deduplicated per Method node: one record per distinct target method,
        // regardless of how many times it was called.
        if (e->type == SG_EDGE_EXECUTES &&
            src->type == SG_NODE_EXECUTION &&
            tgt->type == SG_NODE_METHOD) {
          if (tgt->id <= (uint64_t)node_snap && observed_methods[tgt->id]) {
            continue; // already emitted for this method node
          }
          char t_class[512], t_method_name[256], t_desc[512];
          uint64_t t_loader = 0; int t_hidden_f = 0;
          if (!sg_export_parse_method_node(tgt, node_snap,
                                           t_class, sizeof(t_class),
                                           t_method_name, sizeof(t_method_name),
                                           t_desc,   sizeof(t_desc),
                                           &t_loader, &t_hidden_f)) {
            if (tgt->id <= (uint64_t)node_snap) observed_methods[tgt->id] = true;
            fprintf(f, "{\"record\":\"diagnostic\",\"level\":\"warn\","
                    "\"message\":\"EXECUTES edge: cannot resolve method node"
                    " " UINT64_FORMAT "; record omitted\","
                    "\"method_node_id\":" UINT64_FORMAT "}\n",
                    (uint64_t)tgt->id, (uint64_t)tgt->id);
            diag_count++; continue;
          }
          if (tgt->id <= (uint64_t)node_snap) observed_methods[tgt->id] = true;
          fprintf(f, "{\"record\":\"runtime_target\","
                  "\"evidence\":\"OBSERVED_ONLY\","
                  "\"dispatch_kind\":\"execution_trace\","
                  "\"target_class\":");
          sg_json_str(f, t_class);
          fprintf(f, ",\"target_method\":");
          sg_json_str(f, t_method_name);
          fprintf(f, ",\"target_descriptor\":");
          sg_json_str(f, t_desc);
          fprintf(f, ",\"target_loader_id\":\"0x%016llx\",\"target_hidden\":%s}\n",
                  (unsigned long long)t_loader, t_hidden_f ? "true" : "false");
          rt_count++; continue;
        }
      } // end edge walk

      // -----------------------------------------------------------------------
      // Node walk: generated_class and bytecode_artifact records.
      // -----------------------------------------------------------------------
      for (long ni = 0; ni < node_snap; ni++) {
        SgNode* n = &nodes[ni];

        // -- generated_class --------------------------------------------------
        if (n->type == SG_NODE_GENERATED_CLASS) {
          const char* lbl = n->label ? n->label : "";
          // Label: "<norm_class> by=<generated_by> trigger=<source_trigger> kind=<provenance_kind>"
          char cls_name[1024] = "?";
          {
            const char* sp = strchr(lbl, ' ');
            size_t clen = sp ? (size_t)(sp - lbl) : strlen(lbl);
            if (clen >= sizeof(cls_name)) clen = sizeof(cls_name) - 1;
            memcpy(cls_name, lbl, clen); cls_name[clen] = '\0';
          }
          char gen_by[256] = "?", trigger[256] = "?", prov_kind[256] = "?";
          sg_export_label_field(lbl, "by=",      gen_by,    sizeof(gen_by));
          sg_export_label_field(lbl, "trigger=", trigger,   sizeof(trigger));
          sg_export_label_field(lbl, "kind=",    prov_kind, sizeof(prov_kind));
          int indy_tid = (n->id > 0 && (long)n->id <= node_snap)
                         ? indy_for_gen[n->id] : 0;

          fprintf(f, "{\"record\":\"generated_class\",\"class\":");
          sg_json_str(f, cls_name);
          fprintf(f, ",\"loader_id\":\"0x%016llx\",\"hidden\":%s,"
                  "\"crc\":\"%08x\",\"generated_by\":",
                  (unsigned long long)n->loader_id,
                  (n->flags & 1) ? "true" : "false", n->crc);
          sg_json_str(f, gen_by);
          fprintf(f, ",\"source_trigger\":");
          sg_json_str(f, trigger);
          fprintf(f, ",\"provenance_kind\":");
          sg_json_str(f, prov_kind);
          if (indy_tid > 0) {
            fprintf(f, ",\"indy_trace_id\":%d", indy_tid);
          }
          fprintf(f, "}\n");
          gc_count++; continue;
        }

        // -- bytecode_artifact ------------------------------------------------
        if (n->type == SG_NODE_BYTECODE_ARTIFACT) {
          const char* lbl = n->label ? n->label : "";
          // Label: "<norm> crc=<hex> size=<dec> kind=<kind> load=<load_kind> loader=<loader>"
          // Key:   "A|<norm>|<loader>|<crc>|<original|final>"
          const char* kk = n->key ? n->key : "";
          size_t kk_len = strlen(kk);
          bool is_final = (kk_len >= 6 &&
                           strcmp(kk + kk_len - 6, "|final") == 0);

          char cls_name[1024] = "?";
          {
            const char* sp = strchr(lbl, ' ');
            size_t clen = sp ? (size_t)(sp - lbl) : strlen(lbl);
            if (clen >= sizeof(cls_name)) clen = sizeof(cls_name) - 1;
            memcpy(cls_name, lbl, clen); cls_name[clen] = '\0';
          }
          char size_str[32] = "0", load_kind_str[128] = "?";
          sg_export_label_field(lbl, "size=", size_str,      sizeof(size_str));
          sg_export_label_field(lbl, "load=", load_kind_str, sizeof(load_kind_str));

          // Resolve the original artifact's CRC via REWRITTEN_FROM edge lookup.
          uint64_t orig_id  = (n->id > 0 && (long)n->id <= node_snap)
                              ? rewritten_from[n->id] : 0;
          uint32_t orig_crc = 0;
          if (orig_id != 0 && (long)orig_id <= node_snap) {
            orig_crc = nodes[(long)orig_id - 1].crc;
          }

          // Reconstruct the expected on-disk path using the same sanitization
          // that klassFactory.cpp's soroush_class_dump_path() applies (/ → _).
          // SOROUSH_BYTECODE_DUMP_DIR must match what the JVM used during the run.
          //
          // File layout on disk:
          //   <name>.class          → final bytes (always written by capture_final_class_bytes)
          //   <name>.original.class → pre-transformation bytes (written only when transformed)
          //
          // "original" artifact records from a transformed class point to
          // <name>.original.class; all others point to <name>.class.
          const char* bdd = getenv("SOROUSH_BYTECODE_DUMP_DIR");
          if (bdd == nullptr || bdd[0] == '\0') bdd = "/tmp/soroush_jvm_dump";
          char san[1024];
          {
            size_t ai = 0;
            for (const char* p = cls_name; *p && ai < sizeof(san) - 1; p++, ai++)
              san[ai] = (*p == '/') ? '_' : *p;
            san[ai] = '\0';
          }
          char artifact_path[2048];
          bool artifact_on_disk;
          if (!is_final) {
            // Prefer <name>.original.class (exact bytes for this record's CRC).
            snprintf(artifact_path, sizeof(artifact_path), "%s/%s.original.class", bdd, san);
            if (access(artifact_path, F_OK) == 0) {
              artifact_on_disk = true;
            } else {
              // Non-transformed class: <name>.class holds the original bytes.
              snprintf(artifact_path, sizeof(artifact_path), "%s/%s.class", bdd, san);
              artifact_on_disk = (access(artifact_path, F_OK) == 0);
            }
          } else {
            snprintf(artifact_path, sizeof(artifact_path), "%s/%s.class", bdd, san);
            artifact_on_disk = (access(artifact_path, F_OK) == 0);
          }

          fprintf(f, "{\"record\":\"bytecode_artifact\",\"class\":");
          sg_json_str(f, cls_name);
          fprintf(f, ",\"loader_id\":\"0x%016llx\","
                  "\"crc\":\"%08x\","
                  "\"size\":%s,"
                  "\"kind\":\"%s\","
                  "\"load_kind\":",
                  (unsigned long long)n->loader_id, n->crc,
                  size_str[0] ? size_str : "0",
                  is_final ? "final" : "original");
          sg_json_str(f, load_kind_str[0] ? load_kind_str : "?");
          fprintf(f, ",\"hidden\":%s", (n->flags & 1) ? "true" : "false");
          if (orig_crc != 0) {
            fprintf(f, ",\"rewritten_from_crc\":\"%08x\"", orig_crc);
          }
          if (artifact_on_disk) {
            fprintf(f, ",\"artifact_path\":");
            sg_json_str(f, artifact_path);
          } else {
            fprintf(f, ",\"artifact_bytes_dumped\":false");
          }
          fprintf(f, "}\n");
          ba_count++; continue;
        }
      } // end node walk

      free(rewritten_from);
      free(indy_for_gen);
      free(observed_methods);
    } // end node_snap > 0 branch
  } // end graph_enabled branch

sg_export_summary:
  if (ferror(f)) write_ok = false;
  fprintf(f, "{\"record\":\"export_summary\","
          "\"method_identity_count\":%ld,"
          "\"callsite_target_count\":%ld,"
          "\"runtime_target_count\":%ld,"
          "\"generated_class_count\":%ld,"
          "\"bytecode_artifact_count\":%ld,"
          "\"diagnostic_count\":%ld,"
          "\"complete\":%s}\n",
          mi_count, ct_count, rt_count, gc_count, ba_count, diag_count,
          write_ok ? "true" : "false");
  fclose(f);
  if (!write_ok) {
    fprintf(stderr,
            "[JVM EXPORT] WARNING: write error(s) occurred — output at %s is INCOMPLETE\n",
            path);
  }
  fprintf(stderr,
          "[JVM EXPORT] %s: %s"
          " (method_identity=%ld callsite_target=%ld runtime_target=%ld"
          " generated_class=%ld bytecode_artifact=%ld diagnostic=%ld)\n",
          write_ok ? "done" : "INCOMPLETE",
          path, mi_count, ct_count, rt_count, gc_count, ba_count, diag_count);
}
