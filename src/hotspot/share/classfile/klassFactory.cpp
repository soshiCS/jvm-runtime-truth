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

static u2 read_u2(const u1* p) {
  return ((u2)p[0] << 8) | p[1];
}

static u4 read_u4(const u1* p) {
  return ((u4)p[0] << 24) | ((u4)p[1] << 16) | ((u4)p[2] << 8) | p[3];
}

static void write_u2(u1* p, u2 v) {
  p[0] = (u1)(v >> 8);
  p[1] = (u1)v;
}

static void write_u4(u1* p, u4 v) {
  p[0] = (u1)(v >> 24);
  p[1] = (u1)(v >> 16);
  p[2] = (u1)(v >> 8);
  p[3] = (u1)v;
}

static void emit_u1(u1*& p, u1 v) { *p++ = v; }
static void emit_u2(u1*& p, u2 v) { write_u2(p, v); p += 2; }

static void emit_ldc_w(u1*& p, u2 string_index) {
  emit_u1(p, 0x13);
  emit_u2(p, string_index);
}

static void emit_invokestatic(u1*& p, u2 methodref) {
  emit_u1(p, 0xb8);
  emit_u2(p, methodref);
}

static void emit_soroush_trace(u1*& p, u2 string_index, u2 methodref) {
  emit_ldc_w(p, string_index);
  emit_invokestatic(p, methodref);
}

struct SoroushMethodRewrite {
  bool rewrite;
  u2 enter_string_index;
  u2 exit_string_index;
  int return_count;
  char enter_msg[512];
  char exit_msg[512];
};

static bool should_rewrite_soroush_class(const char* class_name) {
  const char* prefix = getenv("SOROUSH_REWRITE_PREFIX");
  if (prefix == nullptr || prefix[0] == '\0') {
    prefix = "com/example/springboot/";
  }
  if (strcmp(prefix, "*") == 0) return true;
  return strncmp(class_name, prefix, strlen(prefix)) == 0;
}

static bool should_dump_soroush_class(bool is_hidden, const char* class_name) {
  if (is_hidden) return true;
  if (class_name == nullptr) return false;
  return strstr(class_name, "$$Lambda") != nullptr ||
         strstr(class_name, "$Proxy") != nullptr ||
         strstr(class_name, "CGLIB") != nullptr ||
         strstr(class_name, "ByteBuddy") != nullptr;
}

static const char* soroush_class_dump_path(const char* class_name,
                                           char* path,
                                           size_t path_len) {
  const char* dump_dir = "/tmp/soroush_jvm_dump";
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

  const char* dump_dir = "/tmp/soroush_jvm_dump";
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
                                      int original_length,
                                      bool transformed,
                                      bool hidden,
                                      const char* load_kind) {
  if (!soroush_capture_final_bytecode_enabled() || bytes == nullptr || length <= 0) {
    return;
  }

  const char* dump_dir = "/tmp/soroush_jvm_dump";
  mkdir(dump_dir, 0755);

  char path[1024];
  const char* logged_name = soroush_class_dump_path(class_name, path, sizeof(path));
  bool dumped = write_class_bytes_to_path(path, bytes, length);
  uint32_t crc = soroush_crc32(bytes, length);

  fprintf(stderr, "[JVM FINAL BYTECODE] class=%s\n", logged_name);
  fprintf(stderr, "[JVM FINAL BYTECODE] transformed=%s\n", transformed ? "yes" : "no");
  fprintf(stderr, "[JVM FINAL BYTECODE] hidden=%s\n", hidden ? "yes" : "no");
  fprintf(stderr, "[JVM FINAL BYTECODE] load_kind=%s\n", load_kind);
  fprintf(stderr, "[JVM FINAL BYTECODE] original_size=%d\n", original_length);
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

  const char* dump_dir = "/tmp/soroush_jvm_dump";
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

static u1* rewrite_soroush_main(const u1* old_bytes,
                                int old_len,
                                const char* dotted_class_name,
                                int* new_len_out) {
  if (read_u4(old_bytes) != 0xCAFEBABE) return nullptr;

  u2 old_cp_count = read_u2(old_bytes + 8);
  const char** cp_utf8 = (const char**)calloc(old_cp_count, sizeof(char*));
  u2* cp_utf8_len = (u2*)calloc(old_cp_count, sizeof(u2));
  if (cp_utf8 == nullptr || cp_utf8_len == nullptr) {
    free(cp_utf8);
    free(cp_utf8_len);
    return nullptr;
  }

  const u1* p = old_bytes + 10;
  const u1* cp_start = p;

  int code_utf8_index = -1;

  for (u2 i = 1; i < old_cp_count; i++) {
    u1 tag = *p++;

    switch (tag) {
      case 1: {
        u2 len = read_u2(p);
        const char* s = (const char*)(p + 2);

        cp_utf8[i] = s;
        cp_utf8_len[i] = len;

        if (len == 4 && memcmp(s, "Code", 4) == 0) code_utf8_index = i;

        p += 2 + len;
        break;
      }

      case 3:
      case 4:
        p += 4;
        break;

      case 5:
      case 6:
        p += 8;
        i++;
        break;

      case 7:
      case 8:
      case 16:
      case 19:
      case 20:
        p += 2;
        break;

      case 9:
      case 10:
      case 11:
      case 12:
      case 18:
      case 17:
        p += 4;
        break;

      case 15:
        p += 3;
        break;

      default:
        free(cp_utf8);
        free(cp_utf8_len);
        return nullptr;
    }
  }

  if (code_utf8_index < 0) {
    free(cp_utf8);
    free(cp_utf8_len);
    return nullptr;
  }

  const u1* cp_end = p;
  int old_cp_size = (int)(cp_end - cp_start);
  const u1* body_start = cp_end;
  const u1* scan = body_start;

  // access_flags, this_class, super_class
  scan += 6;

  u2 scan_interfaces_count = read_u2(scan);
  scan += 2 + scan_interfaces_count * 2;

  u2 scan_fields_count = read_u2(scan);
  scan += 2;

  for (u2 i = 0; i < scan_fields_count; i++) {
    scan += 6;
    u2 ac = read_u2(scan);
    scan += 2;
    for (u2 j = 0; j < ac; j++) {
      scan += 2;
      u4 alen = read_u4(scan);
      scan += 4 + alen;
    }
  }

  u2 scan_methods_count = read_u2(scan);
  scan += 2;

  SoroushMethodRewrite* method_rewrites =
      (SoroushMethodRewrite*)calloc(scan_methods_count, sizeof(SoroushMethodRewrite));
  if (method_rewrites == nullptr) {
    free(cp_utf8);
    free(cp_utf8_len);
    return nullptr;
  }

  int rewrite_count = 0;
  int code_extra = 0;
  int max_rewrite_count = (65535 - old_cp_count - 6) / 4;

  for (u2 mi = 0; mi < scan_methods_count; mi++) {
    scan += 2;
    u2 m_name = read_u2(scan); scan += 2;
    scan += 2;
    u2 attr_count = read_u2(scan); scan += 2;

    const char* method_name = cp_utf8[m_name];
    u2 method_name_len = cp_utf8_len[m_name];
    bool is_constructor =
        method_name != nullptr &&
        ((method_name_len == 6 && memcmp(method_name, "<init>", 6) == 0) ||
         (method_name_len == 8 && memcmp(method_name, "<clinit>", 8) == 0));
    bool method_has_code = false;
    bool method_has_branches_or_throw = false;
    bool method_has_exception_table = false;
    int return_count = 0;

    for (u2 ai = 0; ai < attr_count; ai++) {
      u2 attr_name = read_u2(scan); scan += 2;
      u4 attr_len = read_u4(scan); scan += 4;
      const u1* attr_body = scan;
      scan += attr_len;

      if (!is_constructor && attr_name == code_utf8_index) {
        const u1* c = attr_body;
        c += 2; // max_stack
        c += 2; // max_locals
        u4 code_len = read_u4(c); c += 4;
        const u1* code = c;
        method_has_code = true;
        method_has_branches_or_throw = has_branches_or_throw(code, code_len);
        c += code_len;
        u2 exception_table_len = read_u2(c);
        method_has_exception_table = exception_table_len > 0;
        for (u4 k = 0; k < code_len; k++) {
          if (code[k] == 0xb1) {
            return_count++;
          }
        }
      }
    }

    if (rewrite_count < max_rewrite_count &&
        method_name != nullptr &&
        !is_constructor &&
        method_has_code &&
        return_count > 0 &&
        !method_has_exception_table &&
        !method_has_branches_or_throw) {
      SoroushMethodRewrite* rewrite = &method_rewrites[mi];
      rewrite->rewrite = true;
      rewrite->return_count = return_count;
      snprintf(rewrite->enter_msg, sizeof(rewrite->enter_msg),
               "ENTER %s.%.*s",
               dotted_class_name, method_name_len, method_name);
      snprintf(rewrite->exit_msg, sizeof(rewrite->exit_msg),
               "EXIT %s.%.*s",
               dotted_class_name, method_name_len, method_name);
      code_extra += 6 + return_count * 6;
      rewrite_count++;
    }
  }

  if (rewrite_count == 0) {
    free(method_rewrites);
    free(cp_utf8);
    free(cp_utf8_len);
    return nullptr;
  }

  u2 idx_utf8_system = old_cp_count;
  u2 idx_class_system = old_cp_count + 1;
  u2 idx_utf8_soroush_trace = old_cp_count + 2;
  u2 idx_utf8_soroush_trace_desc = old_cp_count + 3;
  u2 idx_nt_soroush_trace = old_cp_count + 4;
  u2 idx_method_soroush_trace = old_cp_count + 5;

  u2 next_method_cp_index = old_cp_count + 6;
  for (u2 mi = 0; mi < scan_methods_count; mi++) {
    if (method_rewrites[mi].rewrite) {
      method_rewrites[mi].enter_string_index = next_method_cp_index + 1;
      method_rewrites[mi].exit_string_index = next_method_cp_index + 3;
      next_method_cp_index += 4;
    }
  }

  u2 new_cp_count = old_cp_count + 6 + rewrite_count * 4;

  int cp_extra = 0;
  cp_extra += 1 + 2 + (int)strlen("java/lang/System");
  cp_extra += 1 + 2;
  cp_extra += 1 + 2 + (int)strlen("soroushTrace");
  cp_extra += 1 + 2 + (int)strlen("(Ljava/lang/String;)V");
  cp_extra += 1 + 2 + 2;
  cp_extra += 1 + 2 + 2;

  for (u2 mi = 0; mi < scan_methods_count; mi++) {
    if (method_rewrites[mi].rewrite) {
      cp_extra += 1 + 2 + (int)strlen(method_rewrites[mi].enter_msg);
      cp_extra += 1 + 2;
      cp_extra += 1 + 2 + (int)strlen(method_rewrites[mi].exit_msg);
      cp_extra += 1 + 2;
    }
  }

  int max_new_size = old_len + cp_extra + code_extra + 1024;
  u1* out = (u1*)malloc(max_new_size);
  if (out == nullptr) {
    free(method_rewrites);
    free(cp_utf8);
    free(cp_utf8_len);
    return nullptr;
  }

  u1* q = out;

  memcpy(q, old_bytes, 8);
  q += 8;
  write_u2(q, new_cp_count);
  q += 2;

  memcpy(q, cp_start, old_cp_size);
  q += old_cp_size;

#define ADD_UTF8(str) do { \
    const char* s = (str); \
    int len = (int)strlen(s); \
    emit_u1(q, 1); \
    emit_u2(q, (u2)len); \
    memcpy(q, s, len); \
    q += len; \
  } while (0)

#define ADD_CLASS(name_index) do { \
    emit_u1(q, 7); \
    emit_u2(q, name_index); \
  } while (0)

#define ADD_STRING(utf8_index) do { \
    emit_u1(q, 8); \
    emit_u2(q, utf8_index); \
  } while (0)

#define ADD_NAME_AND_TYPE(name_index, desc_index) do { \
    emit_u1(q, 12); \
    emit_u2(q, name_index); \
    emit_u2(q, desc_index); \
  } while (0)

#define ADD_FIELDREF(class_index, nt_index) do { \
    emit_u1(q, 9); \
    emit_u2(q, class_index); \
    emit_u2(q, nt_index); \
  } while (0)

#define ADD_METHODREF(class_index, nt_index) do { \
    emit_u1(q, 10); \
    emit_u2(q, class_index); \
    emit_u2(q, nt_index); \
  } while (0)

  ADD_UTF8("java/lang/System");
  ADD_CLASS(idx_utf8_system);
  ADD_UTF8("soroushTrace");
  ADD_UTF8("(Ljava/lang/String;)V");
  ADD_NAME_AND_TYPE(idx_utf8_soroush_trace, idx_utf8_soroush_trace_desc);
  ADD_METHODREF(idx_class_system, idx_nt_soroush_trace);

  next_method_cp_index = old_cp_count + 6;
  for (u2 mi = 0; mi < scan_methods_count; mi++) {
    if (method_rewrites[mi].rewrite) {
      ADD_UTF8(method_rewrites[mi].enter_msg);
      ADD_STRING(next_method_cp_index);
      ADD_UTF8(method_rewrites[mi].exit_msg);
      ADD_STRING(next_method_cp_index + 2);
      next_method_cp_index += 4;
    }
  }

#undef ADD_UTF8
#undef ADD_CLASS
#undef ADD_STRING
#undef ADD_NAME_AND_TYPE
#undef ADD_FIELDREF
#undef ADD_METHODREF

  const u1* r = body_start;

  // access_flags, this_class, super_class
  r += 6;

  u2 interfaces_count = read_u2(r);
  r += 2 + interfaces_count * 2;

  u2 fields_count = read_u2(r);
  r += 2;

  for (u2 i = 0; i < fields_count; i++) {
    r += 6;
    u2 ac = read_u2(r);
    r += 2;
    for (u2 j = 0; j < ac; j++) {
      r += 2;
      u4 alen = read_u4(r);
      r += 4 + alen;
    }
  }

  const u1* methods_count_pos = r;
  u2 methods_count = read_u2(r);
  r += 2;

  memcpy(q, body_start, methods_count_pos + 2 - body_start);
  q += methods_count_pos + 2 - body_start;

  bool changed = false;

  for (u2 mi = 0; mi < methods_count; mi++) {
    const u1* method_start = r;

    u2 access = read_u2(r); r += 2;
    u2 m_name = read_u2(r); r += 2;
    u2 m_desc = read_u2(r); r += 2;
    u2 attr_count = read_u2(r); r += 2;
    (void)access;
    (void)m_desc;
    const char* method_name = cp_utf8[m_name];
    u2 method_name_len = cp_utf8_len[m_name];
    bool is_constructor =
        method_name != nullptr &&
        ((method_name_len == 6 && memcmp(method_name, "<init>", 6) == 0) ||
         (method_name_len == 8 && memcmp(method_name, "<clinit>", 8) == 0));

    memcpy(q, method_start, 8);
    q += 8;

    SoroushMethodRewrite* rewrite = &method_rewrites[mi];
    bool should_rewrite_method = rewrite->rewrite && !is_constructor;
    for (u2 ai = 0; ai < attr_count; ai++) {
      const u1* attr_start = r;
      u2 attr_name = read_u2(r); r += 2;
      u4 attr_len = read_u4(r); r += 4;
      const u1* attr_body = r;
      r += attr_len;

      if (should_rewrite_method && attr_name == code_utf8_index) {
        const u1* c = attr_body;

        u2 max_stack = read_u2(c); c += 2;
        u2 max_locals = read_u2(c); c += 2;
        u4 code_len = read_u4(c); c += 4;

        const u1* code = c;
        c += code_len;


        if (has_branches_or_throw(code, code_len)) {
          memcpy(q, attr_start, 6 + attr_len);
          q += 6 + attr_len;
          continue;
        }

        u2 exception_table_len = read_u2(c);
        const u1* exception_table = c;
        int exception_table_size = 2 + exception_table_len * 8;
        c += exception_table_size;

        u2 code_attr_count = read_u2(c);
        (void)code_attr_count;

        int enter_len = 6;
        int exit_len = 6;

        int return_count = rewrite->return_count;

        u4 new_code_len = code_len + enter_len + return_count * exit_len;
        u4 new_attr_len =
        2 + // max_stack
        2 + // max_locals
        4 + // code_length
        new_code_len +
        exception_table_size +
        2;  // attributes_count = 0

        write_u2(q, attr_name); q += 2;
        write_u4(q, new_attr_len); q += 4;

        write_u2(q, max_stack + 1); q += 2;
        write_u2(q, max_locals); q += 2;
        write_u4(q, new_code_len); q += 4;

        emit_soroush_trace(q, rewrite->enter_string_index, idx_method_soroush_trace);

        for (u4 k = 0; k < code_len; k++) {
          if (code[k] == 0xb1) {
            emit_soroush_trace(q, rewrite->exit_string_index, idx_method_soroush_trace);
          }
          emit_u1(q, code[k]);
        }

        // Copy exception table and code attributes unchanged.
        // This is okay for your simple Main.main test.
// Copy exception table unchanged

memcpy(q, exception_table, exception_table_size);
q += exception_table_size;

// Drop Code sub-attributes:
// LineNumberTable, LocalVariableTable, StackMapTable, etc.
write_u2(q, 0);
q += 2;

        changed = true;
      } else {
        memcpy(q, attr_start, 6 + attr_len);
        q += 6 + attr_len;
      }
    }
  }

  memcpy(q, r, old_bytes + old_len - r);
  q += old_bytes + old_len - r;

  if (!changed) {
    free(out);
    free(method_rewrites);
    free(cp_utf8);
    free(cp_utf8_len);
    return nullptr;
  }

  *new_len_out = (int)(q - out);
  free(method_rewrites);
  free(cp_utf8);
  free(cp_utf8_len);
  return out;
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

if (!cl_info.is_hidden() &&
    internal_class_name != nullptr &&
    should_rewrite_soroush_class(internal_class_name)) {
size_t class_name_len = strlen(internal_class_name);
char* dotted_class_name = (char*)malloc(class_name_len + 1);
if (dotted_class_name != nullptr) {
for (size_t i = 0; i < class_name_len; i++) {
dotted_class_name[i] = internal_class_name[i] == '/' ? '.' : internal_class_name[i];
}
dotted_class_name[class_name_len] = '\0';

rewritten_bytes = rewrite_soroush_main(stream->buffer(),
 stream->length(),
 dotted_class_name,
 &rewritten_len);
free(dotted_class_name);
}

if (rewritten_bytes != nullptr) {
fprintf(stderr,
"[JVM REWRITE] Rewrote %s, old=%d new=%d\n",
internal_class_name,
stream->length(),
rewritten_len);

actual_stream = new ClassFileStream(
rewritten_bytes,
rewritten_len,
stream->source()
);
}
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
 original_len,
 transformed,
 cl_info.is_hidden(),
 load_kind);

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
