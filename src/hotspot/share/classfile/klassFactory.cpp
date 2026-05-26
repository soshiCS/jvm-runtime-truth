/*
* Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
* DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
*
* This code is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License version 2 only, as
* published by the Free Software Foundation.
*
* This code is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
* version 2 for more details (a copy is included in the LICENSE file that
* accompanied this code).
*
* You should have received a copy of the GNU General Public License version
* 2 along with this work; if not, write to the Free Software Foundation,
* Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*
* Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
* or visit www.oracle.com if you need additional information or have any
* questions.
*
*/

#include "precompiled.hpp"
#include "cds/filemap.hpp"
#include "classfile/classFileParser.hpp"
#include "classfile/classFileStream.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/classLoadInfo.hpp"
#include "classfile/klassFactory.hpp"
#include "classfile/soroushClassfileRewriter.hpp"
#include "classfile/soroushProvenanceGraph.hpp"
#include "memory/resourceArea.hpp"
#include "prims/jvmtiEnvBase.hpp"
#include "prims/jvmtiRedefineClasses.hpp"
#include "runtime/arguments.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/vframe.inline.hpp"
#include "utilities/macros.hpp"
#include "runtime/atomic.hpp"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#if INCLUDE_JFR
#include "jfr/support/jfrKlassExtension.hpp"
#endif


static volatile int g_class_count = 0;
static volatile int g_dump_count = 0;
static volatile int g_recover_hidden_count = 0;
static volatile int g_recover_lambda_count = 0;
static volatile int g_recover_proxy_count = 0;
static volatile int g_recover_transformed_count = 0;
static volatile int g_recover_file_count = 0;

extern "C" int soroush_current_indy_trace_id();

static bool soroush_trace_indy_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_TRACE_INDY");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_capture_final_bytecode_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_CAPTURE_FINAL_BYTECODE");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_runtime_recovery_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_RUNTIME_RECOVERY");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_rewriter_phase1_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_REWRITER_PHASE1");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_rewriter_phase1_failures_only() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_REWRITER_PHASE1_FAILURES_ONLY");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_rewriter_phase2_entry_nops_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_REWRITER_PHASE2_ENTRY_NOPS");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_rewriter_phase2_matches(const char* class_name) {
  const char* prefix = getenv("SOROUSH_REWRITER_PHASE2_PREFIX");
  if (class_name == nullptr || prefix == nullptr || prefix[0] == '\0') {
    return false;
  }
  if (strcmp(prefix, "*") == 0) {
    return true;
  }
  return strncmp(class_name, prefix, strlen(prefix)) == 0;
}

static bool soroush_rewriter_phase3_enter_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_REWRITER_PHASE3_ENTER");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_rewriter_phase3_matches(const char* class_name) {
  const char* prefix = getenv("SOROUSH_REWRITER_PHASE3_PREFIX");
  if (class_name == nullptr || prefix == nullptr || prefix[0] == '\0') {
    return false;
  }
  if (strcmp(prefix, "*") == 0) {
    return true;
  }
  return strncmp(class_name, prefix, strlen(prefix)) == 0;
}

static bool soroush_rewriter_phase5_normal_exit_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_REWRITER_PHASE5_NORMAL_EXIT");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

// Additive toggle: when SOROUSH_TRACE_MH_EXEC=1, also instrument the JDK's
// MethodHandle/LambdaForm internal classes so the adapter chain is visible
// *executing* (ENTER/EXIT) at runtime, on top of whatever PHASE5 prefix the
// user chose for their own code. Observational; still fully verifier-safe and
// fail-safe (any method we can't prove safe is per-method skipped as usual).
static bool soroush_mh_exec_instrument_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_TRACE_MH_EXEC");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

// Even more aggressive opt-in: allow the rewriter to instrument the JDK's
// *hidden* MethodHandle-internal classes (the runtime-customized dispatch
// LambdaForms, e.g. LambdaForm$BMH+0x...). The rewriter otherwise skips ALL
// hidden classes (conservative). This lets the actual per-invocation dispatch
// chain be traced, at the cost of touching very sensitive machinery — hence a
// distinct, explicit flag, off by default. Still fully verifier-safe + per-method
// fail-safe; if it destabilizes the MH runtime, leave it off (documented).
static bool soroush_mh_exec_hidden_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = getenv("SOROUSH_TRACE_MH_EXEC_HIDDEN");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static bool soroush_is_mh_internal_class(const char* n) {
  if (n == nullptr) return false;
  return strncmp(n, "java/lang/invoke/LambdaForm", 27) == 0 ||
         strncmp(n, "java/lang/invoke/DirectMethodHandle", 35) == 0 ||
         strncmp(n, "java/lang/invoke/BoundMethodHandle", 34) == 0 ||
         strncmp(n, "java/lang/invoke/DelegatingMethodHandle", 39) == 0 ||
         strncmp(n, "java/lang/invoke/Invokers", 25) == 0;
}

static bool soroush_rewriter_phase5_matches(const char* class_name) {
  if (class_name == nullptr) {
    return false;
  }
  const char* prefix = getenv("SOROUSH_REWRITER_PHASE5_PREFIX");
  if (prefix != nullptr && prefix[0] != '\0') {
    if (strcmp(prefix, "*") == 0) {
      return true;
    }
    if (strncmp(class_name, prefix, strlen(prefix)) == 0) {
      return true;
    }
  }
  if (soroush_mh_exec_instrument_enabled() && soroush_is_mh_internal_class(class_name)) {
    return true;
  }
  return false;
}

static bool has_branches_or_throw(const u1* code, u4 code_len) {
  for (u4 i = 0; i < code_len; i++) {
    u1 op = code[i];

    if ((op >= 0x99 && op <= 0xa8) || // if*, goto, jsr
        op == 0xc6 ||                 // ifnull
        op == 0xc7 ||                 // ifnonnull
        op == 0xc8 ||                 // goto_w
        op == 0xc9 ||                 // jsr_w
        op == 0xaa ||                 // tableswitch
        op == 0xab ||                 // lookupswitch
        op == 0xbf) {                 // athrow
      return true;
    }
  }
  return false;
}

static bool should_dump_soroush_class(bool is_hidden, const char* class_name) {
  if (is_hidden) return true;
  if (class_name == nullptr) return false;
  return strstr(class_name, "$$Lambda") != nullptr ||
         strstr(class_name, "$Proxy") != nullptr ||
         strstr(class_name, "CGLIB") != nullptr ||
         strstr(class_name, "ByteBuddy") != nullptr;
}

static const char* soroush_bytecode_dump_dir() {
  const char* v = getenv("SOROUSH_BYTECODE_DUMP_DIR");
  return (v != nullptr && v[0] != '\0') ? v : "/tmp/soroush_jvm_dump";
}

static const char* soroush_class_dump_path(const char* class_name,
                                           char* path,
                                           size_t path_len) {
  const char* dump_dir = soroush_bytecode_dump_dir();
  char generated_name[64];
  if (class_name == nullptr || class_name[0] == '\0') {
    int dump_id = Atomic::add(&g_dump_count, 1);
    snprintf(generated_name, sizeof(generated_name), "hidden_%d", dump_id);
    class_name = generated_name;
  }

  char sanitized_name[512];
  size_t i = 0;
  for (; class_name[i] != '\0' && i < sizeof(sanitized_name) - 1; i++) {
    sanitized_name[i] = class_name[i] == '/' ? '_' : class_name[i];
  }
  sanitized_name[i] = '\0';

  snprintf(path, path_len, "%s/%s.class", dump_dir, sanitized_name);
  return class_name;
}

static bool write_class_bytes_to_path(const char* path,
                                      const u1* bytes,
                                      int length) {
  if (bytes == nullptr || length <= 0 || path == nullptr) return false;

  FILE* f = fopen(path, "wb");
  if (f == nullptr) return false;
  fwrite(bytes, 1, length, f);
  fclose(f);
  return true;
}

static void dump_class_bytes(const char* class_name,
                             const u1* bytes,
                             int length) {
  if (bytes == nullptr || length <= 0) return;

  const char* dump_dir = soroush_bytecode_dump_dir();
  mkdir(dump_dir, 0755);

  char path[1024];
  class_name = soroush_class_dump_path(class_name, path, sizeof(path));
  if (!write_class_bytes_to_path(path, bytes, length)) return;

  fprintf(stderr, "[JVM DUMP] dumped class %s length=%d\n", class_name, length);
}

static uint32_t soroush_crc32(const u1* bytes, int length) {
  uint32_t crc = 0xffffffffu;
  for (int i = 0; i < length; i++) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; bit++) {
      uint32_t mask = (uint32_t)(-(int)(crc & 1u));
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return ~crc;
}

static void capture_final_class_bytes(const char* class_name,
                                      const u1* bytes,
                                      int length,
                                      const u1* orig_bytes,
                                      int orig_length,
                                      bool transformed,
                                      bool hidden,
                                      const char* load_kind) {
  if (!soroush_capture_final_bytecode_enabled() || bytes == nullptr || length <= 0) {
    return;
  }

  const char* dump_dir = soroush_bytecode_dump_dir();
  mkdir(dump_dir, 0755);

  // Always write final bytes to <name>.class.
  char path[1024];
  const char* logged_name = soroush_class_dump_path(class_name, path, sizeof(path));
  bool dumped = write_class_bytes_to_path(path, bytes, length);

  // When transformed, also write the pre-transformation bytes to <name>.original.class
  // so that the original bytecode_artifact record gets a distinct, correct file.
  if (transformed && orig_bytes != nullptr && orig_length > 0) {
    char orig_path[1280];
    snprintf(orig_path, sizeof(orig_path), "%.*s", (int)(strlen(path) - 6), path); // strip ".class"
    strncat(orig_path, ".original.class", sizeof(orig_path) - strlen(orig_path) - 1);
    write_class_bytes_to_path(orig_path, orig_bytes, orig_length);
  }

  uint32_t crc = soroush_crc32(bytes, length);
  fprintf(stderr, "[JVM FINAL BYTECODE] class=%s\n", logged_name);
  fprintf(stderr, "[JVM FINAL BYTECODE] transformed=%s\n", transformed ? "yes" : "no");
  fprintf(stderr, "[JVM FINAL BYTECODE] hidden=%s\n", hidden ? "yes" : "no");
  fprintf(stderr, "[JVM FINAL BYTECODE] load_kind=%s\n", load_kind);
  fprintf(stderr, "[JVM FINAL BYTECODE] original_size=%d\n", orig_length > 0 ? orig_length : length);
  fprintf(stderr, "[JVM FINAL BYTECODE] final_size=%d\n", length);
  fprintf(stderr, "[JVM FINAL BYTECODE] crc32=%08x\n", crc);
  fprintf(stderr, "[JVM FINAL BYTECODE] dumped=%s\n", dumped ? path : "<failed>");
}

struct SoroushRecoveryProvenance {
  const char* generated_by;
  const char* source_trigger;
  const char* provenance_kind;
  const char* evidence;
  int trace_id;
};

static bool soroush_contains(const char* text, const char* needle) {
  return text != nullptr && strstr(text, needle) != nullptr;
}

static bool soroush_stack_contains(JavaThread* thread, const char* package_name) {
  if (thread == nullptr || !thread->has_last_Java_frame()) {
    return false;
  }

  int depth = 0;
  for (vframeStream vfst(thread, false, false);
       !vfst.at_end() && depth < 128;
       vfst.next(), depth++) {
    Method* method = vfst.method();
    if (method == nullptr || method->method_holder() == nullptr) {
      continue;
    }
    const char* holder_name = method->method_holder()->name()->as_C_string();
    if (soroush_contains(holder_name, package_name)) {
      return true;
    }
  }

  return false;
}

static SoroushRecoveryProvenance soroush_recovery_provenance(const char* class_name,
                                                             bool hidden,
                                                             const char* stream_source,
                                                             const char* loader_name,
                                                             JavaThread* thread) {
  SoroushRecoveryProvenance provenance;
  provenance.generated_by = "unknown";
  provenance.source_trigger = "unknown";
  provenance.provenance_kind = "unknown";
  provenance.evidence = "none";
  provenance.trace_id = soroush_current_indy_trace_id();

  if (provenance.trace_id > 0) {
    if (soroush_contains(class_name, "$$Lambda")) {
      provenance.generated_by = "LambdaMetafactory";
      provenance.source_trigger = "invokedynamic";
      provenance.provenance_kind = "exact";
      provenance.evidence = "active-invokedynamic-trace";
      return provenance;
    }

    if (hidden) {
      provenance.generated_by = "invokedynamic-bootstrap";
      provenance.source_trigger = "invokedynamic";
      provenance.provenance_kind = "exact";
      provenance.evidence = "active-invokedynamic-trace";
      return provenance;
    }
  }

  if (soroush_stack_contains(thread, "java/lang/reflect/ProxyGenerator") ||
      soroush_stack_contains(thread, "java/lang/reflect/Proxy")) {
    provenance.generated_by = "ProxyGenerator";
    provenance.source_trigger = "java.lang.reflect.Proxy";
    provenance.provenance_kind = "exact";
    provenance.evidence = "java-stack:java/lang/reflect/Proxy";
    provenance.trace_id = 0;
    return provenance;
  }

  if (soroush_stack_contains(thread, "net/bytebuddy/") ||
      soroush_contains(loader_name, "net.bytebuddy") ||
      soroush_contains(loader_name, "net/bytebuddy") ||
      soroush_contains(stream_source, "net.bytebuddy") ||
      soroush_contains(stream_source, "net/bytebuddy")) {
    provenance.generated_by = "ByteBuddy";
    provenance.source_trigger = "ByteBuddy defineClass";
    provenance.provenance_kind = "exact";
    provenance.evidence = "observed-bytebuddy-stack-loader-or-source";
    provenance.trace_id = 0;
    return provenance;
  }

  if (soroush_stack_contains(thread, "org/springframework/cglib/") ||
      soroush_stack_contains(thread, "net/sf/cglib/") ||
      soroush_contains(loader_name, "org.springframework.cglib") ||
      soroush_contains(loader_name, "org/springframework/cglib") ||
      soroush_contains(loader_name, "net.sf.cglib") ||
      soroush_contains(loader_name, "net/sf/cglib") ||
      soroush_contains(stream_source, "org.springframework.cglib") ||
      soroush_contains(stream_source, "org/springframework/cglib") ||
      soroush_contains(stream_source, "net.sf.cglib") ||
      soroush_contains(stream_source, "net/sf/cglib")) {
    provenance.generated_by = "CGLIB";
    provenance.source_trigger = "CGLIB defineClass";
    provenance.provenance_kind = "exact";
    provenance.evidence = "observed-cglib-stack-loader-or-source";
    provenance.trace_id = 0;
    return provenance;
  }

  if (hidden) {
    provenance.generated_by = "MethodHandles.Lookup.defineHiddenClass";
    provenance.source_trigger = "defineHiddenClass";
    provenance.provenance_kind = "exact";
    provenance.evidence = "hidden-class-without-active-indy-trace";
    provenance.trace_id = 0;
    return provenance;
  }

  provenance.trace_id = 0;

  if (soroush_contains(class_name, "$Proxy") || soroush_contains(class_name, "Proxy")) {
    provenance.generated_by = "ProxyGenerator";
    provenance.source_trigger = "class-name-pattern";
    provenance.provenance_kind = "heuristic";
    provenance.evidence = "class-name-pattern";
    return provenance;
  }

  if (soroush_contains(class_name, "ByteBuddy") ||
      soroush_contains(class_name, "bytebuddy")) {
    provenance.generated_by = "ByteBuddy";
    provenance.source_trigger = "class-name-pattern";
    provenance.provenance_kind = "heuristic";
    provenance.evidence = "class-name-pattern";
    return provenance;
  }

  if (soroush_contains(class_name, "CGLIB") ||
      soroush_contains(class_name, "EnhancerBySpringCGLIB")) {
    provenance.generated_by = "CGLIB";
    provenance.source_trigger = "class-name-pattern";
    provenance.provenance_kind = "heuristic";
    provenance.evidence = "class-name-pattern";
    return provenance;
  }

  return provenance;
}

static void write_recovery_metadata(const char* metadata_path,
                                    const char* class_name,
                                    bool hidden,
                                    const char* generated_by,
                                    const char* loader_name,
                                    const char* source_trigger,
                                    const char* provenance_kind,
                                    const char* evidence,
                                    int trace_id,
                                    const char* load_kind,
                                    bool transformed,
                                    int length,
                                    uint32_t crc) {
  FILE* f = fopen(metadata_path, "w");
  if (f == nullptr) return;
  fprintf(f, "class_name=%s\n", class_name == nullptr ? "<null>" : class_name);
  fprintf(f, "hidden=%s\n", hidden ? "true" : "false");
  fprintf(f, "generated_by=%s\n", generated_by);
  fprintf(f, "loader=%s\n", loader_name == nullptr ? "<unknown>" : loader_name);
  fprintf(f, "source_trigger=%s\n", source_trigger);
  fprintf(f, "provenance_kind=%s\n", provenance_kind);
  fprintf(f, "provenance_evidence=%s\n", evidence);
  fprintf(f, "trace_id=%d\n", trace_id);
  fprintf(f, "load_kind=%s\n", load_kind);
  fprintf(f, "transformed=%s\n", transformed ? "true" : "false");
  fprintf(f, "length=%d\n", length);
  fprintf(f, "crc32=%08x\n", crc);
  fprintf(f, "timestamp=%lld\n", (long long)os::javaTimeMillis());
  fclose(f);
}

static void recover_runtime_generated_class(const char* class_name,
                                            ClassLoaderData* loader_data,
                                            const u1* bytes,
                                            int length,
                                            bool hidden,
                                            bool transformed,
                                            const char* load_kind,
                                            const char* stream_source) {
  if (!soroush_runtime_recovery_enabled() ||
      !should_dump_soroush_class(hidden, class_name) ||
      bytes == nullptr ||
      length <= 0) {
    return;
  }

  const char* dump_dir = soroush_bytecode_dump_dir();
  mkdir(dump_dir, 0755);

  char path[1024];
  const char* logged_name = soroush_class_dump_path(class_name, path, sizeof(path));
  const char* loader_name = loader_data == nullptr ? "<unknown>" : loader_data->loader_name_and_id();
  SoroushRecoveryProvenance provenance =
      soroush_recovery_provenance(logged_name,
                                  hidden,
                                  stream_source,
                                  loader_name,
                                  JavaThread::current());

  if (hidden || provenance.trace_id > 0) {
    char base_path[1024];
    snprintf(base_path, sizeof(base_path), "%s", path);
    size_t base_len = strlen(base_path);
    if (base_len > 6 && strcmp(base_path + base_len - 6, ".class") == 0) {
      base_path[base_len - 6] = '\0';
    }
    int recover_file_id = Atomic::add(&g_recover_file_count, 1);
    snprintf(path,
             sizeof(path),
             "%s_trace%d_recover%d.class",
             base_path,
             provenance.trace_id,
             recover_file_id);
  }

  bool dumped = write_class_bytes_to_path(path, bytes, length);
  uint32_t crc = soroush_crc32(bytes, length);

  char metadata_path[1100];
  snprintf(metadata_path, sizeof(metadata_path), "%s", path);
  size_t metadata_len = strlen(metadata_path);
  if (metadata_len > 6 && strcmp(metadata_path + metadata_len - 6, ".class") == 0) {
    metadata_path[metadata_len - 6] = '\0';
  }
  strncat(metadata_path, ".metadata.txt", sizeof(metadata_path) - strlen(metadata_path) - 1);

  write_recovery_metadata(metadata_path,
                          logged_name,
                          hidden,
                          provenance.generated_by,
                          loader_name,
                          provenance.source_trigger,
                          provenance.provenance_kind,
                          provenance.evidence,
                          provenance.trace_id,
                          load_kind,
                          transformed,
                          length,
                          crc);

  if (hidden) Atomic::inc(&g_recover_hidden_count);
  if (class_name != nullptr && strstr(class_name, "$$Lambda") != nullptr) {
    Atomic::inc(&g_recover_lambda_count);
  }
  if (class_name != nullptr && strstr(class_name, "$Proxy") != nullptr) {
    Atomic::inc(&g_recover_proxy_count);
  }
  if (transformed) Atomic::inc(&g_recover_transformed_count);

  fprintf(stderr, "[JVM RECOVER] %s class recovered\n", hidden ? "hidden" : "runtime-generated");
  fprintf(stderr, "[JVM RECOVER] class=%s\n", logged_name);
  fprintf(stderr, "[JVM RECOVER] generated_by=%s\n", provenance.generated_by);
  fprintf(stderr, "[JVM RECOVER] source_trigger=%s\n", provenance.source_trigger);
  fprintf(stderr, "[JVM RECOVER] provenance_kind=%s\n", provenance.provenance_kind);
  fprintf(stderr, "[JVM RECOVER] provenance_evidence=%s\n", provenance.evidence);
  fprintf(stderr, "[JVM RECOVER] trace_id=%d\n", provenance.trace_id);
  fprintf(stderr, "[JVM RECOVER] loader=%s\n", loader_name);
  fprintf(stderr, "[JVM RECOVER] dumped=%s\n", dumped ? path : "<failed>");
  fprintf(stderr, "[JVM RECOVER] metadata=%s\n", metadata_path);

  // Unified provenance graph: GeneratedClass node (+ GENERATED_FROM / CREATED_BY).
  if (soroush_graph_enabled()) {
    soroush_graph_generated_class(logged_name, hidden ? 1 : 0,
                                  provenance.generated_by, provenance.source_trigger,
                                  provenance.provenance_kind, provenance.trace_id,
                                  crc, (uint64_t)(uintptr_t)loader_data);
  }
}

extern "C" void soroush_runtime_recovery_print_summary() {
  if (!soroush_runtime_recovery_enabled()) {
    return;
  }

  fprintf(stderr,
          "[JVM RECOVER] summary hidden_classes=%d lambda_classes=%d proxies=%d transformed_classes=%d\n",
          Atomic::load(&g_recover_hidden_count),
          Atomic::load(&g_recover_lambda_count),
          Atomic::load(&g_recover_proxy_count),
          Atomic::load(&g_recover_transformed_count));
}

// called during initial loading of a shared class
InstanceKlass* KlassFactory::check_shared_class_file_load_hook(
                                          InstanceKlass* ik,
                                          Symbol* class_name,
                                          Handle class_loader,
                                          Handle protection_domain,
                                          const ClassFileStream *cfs,
                                          TRAPS) {
#if INCLUDE_CDS && INCLUDE_JVMTI
  assert(ik != nullptr, "sanity");
  assert(ik->is_shared(), "expecting a shared class");
  if (JvmtiExport::should_post_class_file_load_hook()) {
    ResourceMark rm(THREAD);
    // Post the CFLH
    JvmtiCachedClassFileData* cached_class_file = nullptr;
    if (cfs == nullptr) {
      cfs = FileMapInfo::open_stream_for_jvmti(ik, class_loader, CHECK_NULL);
    }
    unsigned char* ptr = (unsigned char*)cfs->buffer();
    unsigned char* end_ptr = ptr + cfs->length();
    unsigned char* old_ptr = ptr;
    JvmtiExport::post_class_file_load_hook(class_name,
                                           class_loader,
                                           protection_domain,
                                           &ptr,
                                           &end_ptr,
                                           &cached_class_file);
    if (old_ptr != ptr) {
      // JVMTI agent has modified class file data.
      // Set new class file stream using JVMTI agent modified class file data.
      ClassLoaderData* loader_data =
        ClassLoaderData::class_loader_data(class_loader());
      s2 path_index = ik->shared_classpath_index();
      ClassFileStream* stream = new ClassFileStream(ptr,
                                                    end_ptr - ptr,
                                                    cfs->source(),
                                                    ClassFileStream::verify);
      ClassLoadInfo cl_info(protection_domain);
      ClassFileParser parser(stream,
                             class_name,
                             loader_data,
                             &cl_info,
                             ClassFileParser::BROADCAST, // publicity level
                             CHECK_NULL);
      const ClassInstanceInfo* cl_inst_info = cl_info.class_hidden_info_ptr();
      InstanceKlass* new_ik = parser.create_instance_klass(true, // changed_by_loadhook
                                                           *cl_inst_info,  // dynamic_nest_host and classData
                                                           CHECK_NULL);

      if (cached_class_file != nullptr) {
        new_ik->set_cached_class_file(cached_class_file);
      }

      if (class_loader.is_null()) {
        new_ik->set_classpath_index(path_index);
      }

      return new_ik;
    }
  }
#endif

  return nullptr;
}


static ClassFileStream* check_class_file_load_hook(ClassFileStream* stream,
                                                   Symbol* name,
                                                   ClassLoaderData* loader_data,
                                                   Handle protection_domain,
                                                   JvmtiCachedClassFileData** cached_class_file,
                                                   TRAPS) {

  assert(stream != nullptr, "invariant");

  if (JvmtiExport::should_post_class_file_load_hook()) {
    const JavaThread* jt = THREAD;

    Handle class_loader(THREAD, loader_data->class_loader());

    // Get the cached class file bytes (if any) from the class that
    // is being retransformed. If class file load hook provides
    // modified class data during class loading or redefinition,
    // new cached class file buffer should be allocated.
    // We use jvmti_thread_state()
    // instead of JvmtiThreadState::state_for(jt) so we don't allocate
    // a JvmtiThreadState any earlier than necessary. This will help
    // avoid the bug described by 7126851.

    JvmtiThreadState* state = jt->jvmti_thread_state();

    if (state != nullptr) {
      Klass* k = state->get_class_being_redefined();
      if (k != nullptr && state->get_class_load_kind() == jvmti_class_load_kind_retransform) {
        InstanceKlass* class_being_redefined = InstanceKlass::cast(k);
        *cached_class_file = class_being_redefined->get_cached_class_file();
      }
    }

    unsigned char* ptr = const_cast<unsigned char*>(stream->buffer());
    unsigned char* end_ptr = ptr + stream->length();

    JvmtiExport::post_class_file_load_hook(name,
                                           class_loader,
                                           protection_domain,
                                           &ptr,
                                           &end_ptr,
                                           cached_class_file);

    if (ptr != stream->buffer()) {
      // JVMTI agent has modified class file data.
      // Set new class file stream using JVMTI agent modified class file data.
      stream = new ClassFileStream(ptr,
                                   end_ptr - ptr,
                                   stream->source(),
                                   stream->need_verify());
    }
  }

  return stream;
}


InstanceKlass* KlassFactory::create_from_stream(ClassFileStream* stream,
  Symbol* name,
  ClassLoaderData* loader_data,
  const ClassLoadInfo& cl_info,
TRAPS) {
assert(stream != nullptr, "invariant");
assert(loader_data != nullptr, "invariant");

ResourceMark rm(THREAD);

int count = Atomic::add(&g_class_count, 1);

// fprintf(stderr,
// "[JVM CAPTURE] class #%d name=%s hidden=%d length=%d\n",
// count,
// name == nullptr ? "<null>" : name->as_C_string(),
// cl_info.is_hidden(),
// stream->length());

ClassFileStream* actual_stream = stream;
u1* rewritten_bytes = nullptr;
int rewritten_len = 0;
int original_len = stream->length();
const char* load_kind = "load";

JvmtiThreadState* redefine_state = THREAD->jvmti_thread_state();
if (redefine_state != nullptr && redefine_state->get_class_being_redefined() != nullptr) {
  if (redefine_state->get_class_load_kind() == jvmti_class_load_kind_retransform) {
    load_kind = "retransform";
  } else if (redefine_state->get_class_load_kind() == jvmti_class_load_kind_redefine) {
    load_kind = "redefine";
  }
}

const char* internal_class_name = name == nullptr ? nullptr : name->as_C_string();
if (soroush_rewriter_phase1_enabled()) {
SoroushClassfileRewriter::RoundTripResult phase1 =
    SoroushClassfileRewriter::roundtrip_copy(stream->buffer(), stream->length());
if (phase1.ok) {
  bool same = phase1.length == stream->length() &&
      memcmp(phase1.bytes, stream->buffer(), stream->length()) == 0;
  if (!same || !soroush_rewriter_phase1_failures_only()) {
    fprintf(stderr,
            "[JVM REWRITER PHASE1] class=%s roundtrip=%s methods=%d instructions=%d length=%d\n",
            internal_class_name == nullptr ? "<null>" : internal_class_name,
            same ? "ok" : "mismatch",
            phase1.decoded_methods,
            phase1.decoded_instructions,
            phase1.length);
  }
} else {
  fprintf(stderr,
          "[JVM REWRITER PHASE1] class=%s roundtrip=failed error=%s\n",
          internal_class_name == nullptr ? "<null>" : internal_class_name,
          phase1.error == nullptr ? "<unknown>" : phase1.error);
}
SoroushClassfileRewriter::free_roundtrip(&phase1);
}

if ((!cl_info.is_hidden() ||
     (soroush_mh_exec_hidden_enabled() && soroush_is_mh_internal_class(internal_class_name))) &&
    internal_class_name != nullptr &&
    soroush_rewriter_phase5_normal_exit_enabled() &&
    soroush_rewriter_phase5_matches(internal_class_name)) {
SoroushClassfileRewriter::TransformResult phase5 =
    SoroushClassfileRewriter::insert_entry_exit_trace(stream->buffer(), stream->length(), internal_class_name,
        (uint64_t)(uintptr_t)loader_data, cl_info.is_hidden() ? 1 : 0,
        soroush_crc32(stream->buffer(), stream->length()));
if (phase5.ok) {
  rewritten_bytes = phase5.bytes;
  rewritten_len = phase5.length;
  phase5.bytes = nullptr;
  fprintf(stderr,
          "[JVM REWRITER PHASE5] class=%s normal_exit=ok methods=%d instructions=%d old=%d new=%d code_methods=%d ctor_methods_skipped=%d\n",
          internal_class_name,
          phase5.transformed_methods,
          phase5.decoded_instructions,
          stream->length(),
          rewritten_len,
          phase5.code_methods,
          phase5.constructor_methods);
  actual_stream = new ClassFileStream(
      rewritten_bytes,
      rewritten_len,
      stream->source()
  );
} else {
  fprintf(stderr,
          "[JVM REWRITER PHASE5] class=%s normal_exit=failed error=%s\n",
          internal_class_name,
          phase5.error == nullptr ? "<unknown>" : phase5.error);
}
SoroushClassfileRewriter::free_transform(&phase5);
}

if (rewritten_bytes == nullptr &&
    !cl_info.is_hidden() &&
    internal_class_name != nullptr &&
    soroush_rewriter_phase3_enter_enabled() &&
    soroush_rewriter_phase3_matches(internal_class_name)) {
SoroushClassfileRewriter::TransformResult phase3 =
    SoroushClassfileRewriter::insert_entry_trace(stream->buffer(), stream->length(), internal_class_name,
        (uint64_t)(uintptr_t)loader_data, cl_info.is_hidden() ? 1 : 0,
        soroush_crc32(stream->buffer(), stream->length()));
if (phase3.ok) {
  rewritten_bytes = phase3.bytes;
  rewritten_len = phase3.length;
  phase3.bytes = nullptr;
  fprintf(stderr,
          "[JVM REWRITER PHASE3] class=%s enter=ok methods=%d instructions=%d old=%d new=%d\n",
          internal_class_name,
          phase3.transformed_methods,
          phase3.decoded_instructions,
          stream->length(),
          rewritten_len);
  actual_stream = new ClassFileStream(
      rewritten_bytes,
      rewritten_len,
      stream->source()
  );
} else {
  fprintf(stderr,
          "[JVM REWRITER PHASE3] class=%s enter=failed error=%s\n",
          internal_class_name,
          phase3.error == nullptr ? "<unknown>" : phase3.error);
}
SoroushClassfileRewriter::free_transform(&phase3);
}

if (rewritten_bytes == nullptr &&
    !cl_info.is_hidden() &&
    internal_class_name != nullptr &&
    soroush_rewriter_phase2_entry_nops_enabled() &&
    soroush_rewriter_phase2_matches(internal_class_name)) {
SoroushClassfileRewriter::TransformResult phase2 =
    SoroushClassfileRewriter::insert_entry_nops(stream->buffer(), stream->length(), 4);
if (phase2.ok) {
  rewritten_bytes = phase2.bytes;
  rewritten_len = phase2.length;
  phase2.bytes = nullptr;
  fprintf(stderr,
          "[JVM REWRITER PHASE2] class=%s entry_nops=ok methods=%d instructions=%d old=%d new=%d\n",
          internal_class_name,
          phase2.transformed_methods,
          phase2.decoded_instructions,
          stream->length(),
          rewritten_len);
  actual_stream = new ClassFileStream(
      rewritten_bytes,
      rewritten_len,
      stream->source()
  );
} else {
  fprintf(stderr,
          "[JVM REWRITER PHASE2] class=%s entry_nops=failed error=%s\n",
          internal_class_name,
          phase2.error == nullptr ? "<unknown>" : phase2.error);
}
SoroushClassfileRewriter::free_transform(&phase2);
}

HandleMark hm(THREAD);

JvmtiCachedClassFileData* cached_class_file = nullptr;

ClassFileStream* old_stream = actual_stream;

THREAD->statistical_info().incr_define_class_count();

if (!cl_info.is_hidden()) {
actual_stream = check_class_file_load_hook(actual_stream,
 name,
 loader_data,
 cl_info.protection_domain(),
 &cached_class_file,
 CHECK_NULL);
}

bool transformed = old_stream != actual_stream || rewritten_bytes != nullptr;
capture_final_class_bytes(internal_class_name,
 actual_stream->buffer(),
 actual_stream->length(),
 transformed ? stream->buffer() : nullptr,
 transformed ? stream->length() : 0,
 transformed,
 cl_info.is_hidden(),
 load_kind);

// Unified provenance graph: BytecodeArtifact node(s) (+ HAS_BYTECODE / REWRITTEN_FROM).
if (soroush_graph_enabled()) {
  uint64_t graph_loader_id = (uint64_t)(uintptr_t)loader_data;
  int graph_hidden = cl_info.is_hidden() ? 1 : 0;
  if (transformed) {
    soroush_graph_bytecode(internal_class_name,
                           soroush_crc32(stream->buffer(), stream->length()),
                           stream->length(), 0, load_kind, graph_loader_id, graph_hidden);
  }
  soroush_graph_bytecode(internal_class_name,
                         soroush_crc32(actual_stream->buffer(), actual_stream->length()),
                         actual_stream->length(), transformed ? 1 : 0, load_kind,
                         graph_loader_id, graph_hidden);
}

if (should_dump_soroush_class(cl_info.is_hidden(), internal_class_name)) {
dump_class_bytes(internal_class_name,
 actual_stream->buffer(),
 actual_stream->length());
}

recover_runtime_generated_class(internal_class_name,
 loader_data,
 actual_stream->buffer(),
 actual_stream->length(),
 cl_info.is_hidden(),
 transformed,
 load_kind,
 actual_stream->source());

if (soroush_trace_indy_enabled() &&
    (cl_info.is_hidden() ||
     (internal_class_name != nullptr && strstr(internal_class_name, "$$Lambda") != nullptr))) {
int indy_trace_id = soroush_current_indy_trace_id();
if (indy_trace_id > 0) {
fprintf(stderr,
"[JVM INDY #%d] hidden_class=%s hidden=%d length=%d\n",
indy_trace_id,
internal_class_name == nullptr ? "<null>" : internal_class_name,
cl_info.is_hidden(),
actual_stream->length());
} else {
fprintf(stderr,
"[JVM INDY] hidden_class=%s hidden=%d length=%d\n",
internal_class_name == nullptr ? "<null>" : internal_class_name,
cl_info.is_hidden(),
actual_stream->length());
}
}

ClassFileParser parser(actual_stream,
name,
loader_data,
&cl_info,
ClassFileParser::BROADCAST,
CHECK_NULL);

const ClassInstanceInfo* cl_inst_info = cl_info.class_hidden_info_ptr();

InstanceKlass* result = parser.create_instance_klass(old_stream != actual_stream,
         *cl_inst_info,
         CHECK_NULL);

assert(result != nullptr, "result cannot be null with no pending exception");

if (cached_class_file != nullptr) {
result->set_cached_class_file(cached_class_file);
}

JFR_ONLY(ON_KLASS_CREATION(result, parser, THREAD);)

#if INCLUDE_CDS
if (Arguments::is_dumping_archive()) {
ClassLoader::record_result(THREAD, result, actual_stream, old_stream != actual_stream);
}
#endif

return result;
}
