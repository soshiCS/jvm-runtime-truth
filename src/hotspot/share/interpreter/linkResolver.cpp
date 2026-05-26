/*
 * Copyright (c) 1997, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/archiveUtils.hpp"
#include "classfile/defaultMethods.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/soroushProvenanceGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "compiler/compilationPolicy.hpp"
#include "compiler/compileBroker.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "interpreter/bootstrapInfo.hpp"
#include "interpreter/bytecode.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/linkResolver.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbolHandle.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/reflection.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/signature.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/macros.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "runtime/vframe.inline.hpp"
#include <stdlib.h>
#include <string.h>
#if INCLUDE_JFR
#include "jfr/jfr.hpp"
#endif

static bool soroush_trace_reflection_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = ::getenv("SOROUSH_TRACE_REFLECTION");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static void soroush_print_reflect_member_name(oop mname) {
  if (mname == nullptr || !java_lang_invoke_MemberName::is_instance(mname)) {
    fprintf(stderr, "<null-or-non-MemberName>");
    return;
  }

  Method* target = java_lang_invoke_MemberName::vmtarget(mname);
  if (target != nullptr) {
    fprintf(stderr, "%s.%s%s",
            target->method_holder()->name()->as_C_string(),
            target->name()->as_C_string(),
            target->signature()->as_C_string());
    return;
  }

  oop clazz = java_lang_invoke_MemberName::clazz(mname);
  oop name = java_lang_invoke_MemberName::name(mname);
  fprintf(stderr, "MemberName[");
  if (clazz != nullptr && java_lang_Class::is_instance(clazz)) {
    Klass* k = java_lang_Class::as_Klass(clazz);
    fprintf(stderr, "class=%s ", k == nullptr ? "<null>" : k->name()->as_C_string());
  }
  if (name != nullptr && java_lang_String::is_instance(name)) {
    fprintf(stderr, "name=%s", java_lang_String::as_utf8_string(name));
  }
  fprintf(stderr, "]");
}

static void soroush_print_reflect_method_handle(oop mh) {
  if (mh == nullptr || !java_lang_invoke_MethodHandle::is_instance(mh)) {
    fprintf(stderr, "<null-or-non-MethodHandle>");
    return;
  }

  fprintf(stderr, "%s", mh->klass()->external_name());

  if (java_lang_invoke_DirectMethodHandle::is_instance(mh)) {
    fprintf(stderr, " direct_target=");
    soroush_print_reflect_member_name(java_lang_invoke_DirectMethodHandle::member(mh));
  }

  oop form = java_lang_invoke_MethodHandle::form(mh);
  if (form != nullptr && java_lang_invoke_LambdaForm::is_instance(form)) {
    fprintf(stderr, " lambda_form_vmentry=");
    soroush_print_reflect_member_name(java_lang_invoke_LambdaForm::vmentry(form));
  }
}

static bool soroush_trace_mh_exec_enabled() {
  static int enabled = -1;
  if (enabled == -1) {
    const char* value = ::getenv("SOROUSH_TRACE_MH_EXEC");
    enabled = (value != nullptr && strcmp(value, "1") == 0) ? 1 : 0;
  }
  return enabled == 1;
}

// Compact MethodType descriptor "(<nparams> args)-><rtype>" for a MethodHandle.
// Read-only; bounded; fail-safe (leaves "?" on any unexpected shape).
static void soroush_mh_type_desc(oop mh, char* buf, size_t len) {
  if (len == 0) return;
  buf[0] = '\0';
  oop mt = java_lang_invoke_MethodHandle::type(mh);
  if (mt == nullptr || !java_lang_invoke_MethodType::is_instance(mt)) {
    snprintf(buf, len, "?");
    return;
  }
  int nargs = java_lang_invoke_MethodType::ptype_count(mt);
  const char* rname = "?";
  oop rt = java_lang_invoke_MethodType::rtype(mt);
  if (rt != nullptr && java_lang_Class::is_instance(rt)) {
    if (java_lang_Class::is_primitive(rt)) {
      rname = type2name(java_lang_Class::primitive_type(rt));
    } else {
      Klass* rk = java_lang_Class::as_Klass(rt);
      if (rk != nullptr) rname = rk->external_name();
    }
  }
  snprintf(buf, len, "(%d args)->%s", nargs, rname != nullptr ? rname : "?");
}

// LambdaForm name for a MethodHandle: its form's vmentry MemberName as
// "<class>.<method>" (e.g. "java.lang.invoke.LambdaForm$DMH.invokeStatic").
static void soroush_mh_lf_name(oop mh, char* buf, size_t len) {
  if (len == 0) return;
  buf[0] = '\0';
  oop form = java_lang_invoke_MethodHandle::form(mh);
  if (form == nullptr || !java_lang_invoke_LambdaForm::is_instance(form)) return;
  oop vmentry = java_lang_invoke_LambdaForm::vmentry(form);
  if (vmentry == nullptr || !java_lang_invoke_MemberName::is_instance(vmentry)) return;

  Method* target = java_lang_invoke_MemberName::vmtarget(vmentry);
  if (target != nullptr) {
    snprintf(buf, len, "%s.%s", target->method_holder()->name()->as_C_string(),
             target->name()->as_C_string());
    return;
  }
  oop clazz = java_lang_invoke_MemberName::clazz(vmentry);
  oop name = java_lang_invoke_MemberName::name(vmentry);
  const char* cn = "?";
  if (clazz != nullptr && java_lang_Class::is_instance(clazz)) {
    Klass* k = java_lang_Class::as_Klass(clazz);
    if (k != nullptr) cn = k->name()->as_C_string();
  }
  if (name != nullptr && java_lang_String::is_instance(name)) {
    snprintf(buf, len, "%s.%s", cn, java_lang_String::as_utf8_string(name));
  } else {
    snprintf(buf, len, "%s.?", cn);
  }
}

// Observational MethodHandle / LambdaForm execution-structure tracer. Runs at an
// invocation linkage site (resolve_handle_call) with the appendix MethodHandle.
// Read-only walk of the MH object graph (no behavior change); emits [JVM MH EXEC]
// diagnostics and provenance-graph MHAdapter/LambdaFormExec nodes + edges.
// NOTE: per-invocation invokeBasic/linkTo* execution runs in generated assembly
// stubs (no safe C++ hook); the deep adapter-chain *execution* is instead made
// visible by instrumenting the LambdaForm bytecode (SOROUSH_TRACE_MH_EXEC + the
// PHASE5 rewriter). This hook captures the structural shape at link time.
static void soroush_trace_mh_execution(oop mh, const char* site, Klass* caller, TRAPS) {
  bool log = soroush_trace_mh_exec_enabled();
  bool graph = soroush_graph_enabled();
  if (!log && !graph) return;
  if (mh == nullptr) return;
  ResourceMark rm(THREAD);

  // Common case at invokeExact/invoke linkage: the appendix is the *invoker*
  // MemberName, not the user's MethodHandle (that is a runtime receiver value).
  // Record the invoke site -> invoker LambdaForm so it connects (by name) to the
  // LambdaForm executions captured via bytecode instrumentation.
  if (!java_lang_invoke_MethodHandle::is_instance(mh)) {
    if (!java_lang_invoke_MemberName::is_instance(mh)) return;
    Method* inv = java_lang_invoke_MemberName::vmtarget(mh);
    if (inv == nullptr) return;
    char inv_name[640];
    snprintf(inv_name, sizeof(inv_name), "%s.%s",
             inv->method_holder()->name()->as_C_string(), inv->name()->as_C_string());
    if (log) {
      fprintf(stderr, "[JVM MH EXEC] site=%s caller=%s invoker=%s\n", site,
              caller == nullptr ? "<unknown>" : caller->name()->as_C_string(), inv_name);
    }
    if (graph) {
      soroush_graph_mh_chain(soroush_runtime_current_exec_id(), "InvokerSite",
                             nullptr, inv_name, nullptr, nullptr, nullptr, 0, 0);
    }
    return;
  }

  const char* kind = mh->klass()->external_name();
  int is_bound = (kind != nullptr && strstr(kind, "BoundMethodHandle") != nullptr) ? 1 : 0;

  char type_desc[256];
  soroush_mh_type_desc(mh, type_desc, sizeof(type_desc));
  char lf_name[512];
  soroush_mh_lf_name(mh, lf_name, sizeof(lf_name));

  const char* final_class = nullptr;
  const char* final_method = nullptr;
  const char* final_desc = nullptr;
  uint64_t final_loader_id = 0;
  if (java_lang_invoke_DirectMethodHandle::is_instance(mh)) {
    oop member = java_lang_invoke_DirectMethodHandle::member(mh);
    if (member != nullptr && java_lang_invoke_MemberName::is_instance(member)) {
      Method* t = java_lang_invoke_MemberName::vmtarget(member);
      if (t != nullptr) {
        final_class = t->method_holder()->name()->as_C_string();
        final_method = t->name()->as_C_string();
        final_desc = t->signature()->as_C_string();
        final_loader_id = (uint64_t)(uintptr_t)t->method_holder()->class_loader_data();
      }
    }
  }

  if (log) {
    fprintf(stderr, "[JVM MH EXEC] site=%s caller=%s kind=%s type=%s bound=%d\n",
            site, caller == nullptr ? "<unknown>" : caller->name()->as_C_string(),
            kind, type_desc, is_bound);
    if (lf_name[0] != '\0') {
      fprintf(stderr, "[JVM LAMBDAFORM EXEC] lf=%s\n", lf_name);
    }
    if (final_class != nullptr) {
      fprintf(stderr, "[JVM MH ADAPTER] resolves_to=%s.%s%s\n",
              final_class, final_method, final_desc ? final_desc : "");
    }
  }
  if (graph) {
    uint64_t caller_exec = soroush_runtime_current_exec_id();
    soroush_graph_mh_chain(caller_exec, kind, type_desc,
                           lf_name[0] != '\0' ? lf_name : nullptr,
                           final_class, final_method, final_desc, is_bound,
                           final_loader_id);
  }
}

static void soroush_trace_runtime_dispatch(const char* kind,
                                           Klass* receiver_klass,
                                           const methodHandle& resolved_method,
                                           const methodHandle& selected_method,
                                           TRAPS) {
  if (!soroush_trace_reflection_enabled() || selected_method.is_null()) {
    return;
  }

  ResourceMark rm(THREAD);
  fprintf(stderr, "[JVM REFLECT] dispatch_kind=%s\n", kind);
  fprintf(stderr, "[JVM REFLECT] receiver_class=%s\n",
          receiver_klass == nullptr ? "<null>" : receiver_klass->name()->as_C_string());
  fprintf(stderr, "[JVM REFLECT] resolved_method=%s.%s%s\n",
          resolved_method->method_holder()->name()->as_C_string(),
          resolved_method->name()->as_C_string(),
          resolved_method->signature()->as_C_string());
  fprintf(stderr, "[JVM REFLECT] final_resolved_method=%s.%s%s\n",
          selected_method->method_holder()->name()->as_C_string(),
          selected_method->name()->as_C_string(),
          selected_method->signature()->as_C_string());
}

//------------------------------------------------------------------------------------------------------------------------
// Implementation of CallInfo


void CallInfo::set_static(Klass* resolved_klass, const methodHandle& resolved_method, TRAPS) {
  int vtable_index = Method::nonvirtual_vtable_index;
  set_common(resolved_klass, resolved_method, resolved_method, CallInfo::direct_call, vtable_index, CHECK);
}


void CallInfo::set_interface(Klass* resolved_klass,
                             const methodHandle& resolved_method,
                             const methodHandle& selected_method,
                             int itable_index, TRAPS) {
  // This is only called for interface methods. If the resolved_method
  // comes from java/lang/Object, it can be the subject of a virtual call, so
  // we should pick the vtable index from the resolved method.
  // In that case, the caller must call set_virtual instead of set_interface.
  assert(resolved_method->method_holder()->is_interface(), "");
  assert(itable_index == resolved_method()->itable_index(), "");
  set_common(resolved_klass, resolved_method, selected_method, CallInfo::itable_call, itable_index, CHECK);
}

void CallInfo::set_virtual(Klass* resolved_klass,
                           const methodHandle& resolved_method,
                           const methodHandle& selected_method,
                           int vtable_index, TRAPS) {
  assert(vtable_index >= 0 || vtable_index == Method::nonvirtual_vtable_index, "valid index");
  assert(vtable_index < 0 || !resolved_method->has_vtable_index() || vtable_index == resolved_method->vtable_index(), "");
  CallKind kind = (vtable_index >= 0 && !resolved_method->can_be_statically_bound() ? CallInfo::vtable_call : CallInfo::direct_call);
  set_common(resolved_klass, resolved_method, selected_method, kind, vtable_index, CHECK);
  assert(!resolved_method->is_compiled_lambda_form(), "these must be handled via an invokehandle call");
}

void CallInfo::set_handle(Klass* resolved_klass,
                          const methodHandle& resolved_method,
                          Handle resolved_appendix, TRAPS) {
  guarantee(resolved_method.not_null(), "resolved method is null");
  assert(resolved_method->intrinsic_id() == vmIntrinsics::_invokeBasic ||
         resolved_method->is_compiled_lambda_form(),
         "linkMethod must return one of these");
  int vtable_index = Method::nonvirtual_vtable_index;
  assert(!resolved_method->has_vtable_index(), "");
  set_common(resolved_klass, resolved_method, resolved_method, CallInfo::direct_call, vtable_index, CHECK);
  _resolved_appendix = resolved_appendix;
}

void CallInfo::set_common(Klass* resolved_klass,
                          const methodHandle& resolved_method,
                          const methodHandle& selected_method,
                          CallKind kind,
                          int index,
                          TRAPS) {
  assert(resolved_method->signature() == selected_method->signature(), "signatures must correspond");
  _resolved_klass  = resolved_klass;
  _resolved_method = resolved_method;
  _selected_method = selected_method;
  _call_kind       = kind;
  _call_index      = index;
  _resolved_appendix = Handle();
  DEBUG_ONLY(verify());  // verify before making side effects

  CompilationPolicy::compile_if_required(selected_method, THREAD);
}

// utility query for unreflecting a method
CallInfo::CallInfo(Method* resolved_method, Klass* resolved_klass, TRAPS) {
  Klass* resolved_method_holder = resolved_method->method_holder();
  if (resolved_klass == nullptr) { // 2nd argument defaults to holder of 1st
    resolved_klass = resolved_method_holder;
  }
  _resolved_klass  = resolved_klass;
  _resolved_method = methodHandle(THREAD, resolved_method);
  _selected_method = methodHandle(THREAD, resolved_method);
  // classify:
  CallKind kind = CallInfo::unknown_kind;
  int index = resolved_method->vtable_index();
  if (resolved_method->can_be_statically_bound()) {
    kind = CallInfo::direct_call;
  } else if (!resolved_method_holder->is_interface()) {
    // Could be an Object method inherited into an interface, but still a vtable call.
    kind = CallInfo::vtable_call;
  } else if (!resolved_klass->is_interface()) {
    // A default or miranda method.  Compute the vtable index.
    index = LinkResolver::vtable_index_of_interface_method(resolved_klass, _resolved_method);
    assert(index >= 0 , "we should have valid vtable index at this point");

    kind = CallInfo::vtable_call;
  } else if (resolved_method->has_vtable_index()) {
    // Can occur if an interface redeclares a method of Object.

#ifdef ASSERT
    // Ensure that this is really the case.
    Klass* object_klass = vmClasses::Object_klass();
    Method * object_resolved_method = object_klass->vtable().method_at(index);
    assert(object_resolved_method->name() == resolved_method->name(),
      "Object and interface method names should match at vtable index %d, %s != %s",
      index, object_resolved_method->name()->as_C_string(), resolved_method->name()->as_C_string());
    assert(object_resolved_method->signature() == resolved_method->signature(),
      "Object and interface method signatures should match at vtable index %d, %s != %s",
      index, object_resolved_method->signature()->as_C_string(), resolved_method->signature()->as_C_string());
#endif // ASSERT

    kind = CallInfo::vtable_call;
  } else {
    // A regular interface call.
    kind = CallInfo::itable_call;
    index = resolved_method->itable_index();
  }
  assert(index == Method::nonvirtual_vtable_index || index >= 0, "bad index %d", index);
  _call_kind  = kind;
  _call_index = index;
  _resolved_appendix = Handle();
  // Find or create a ResolvedMethod instance for this Method*
  set_resolved_method_name(CHECK);

  DEBUG_ONLY(verify());
}

void CallInfo::set_resolved_method_name(TRAPS) {
  assert(_resolved_method() != nullptr, "Should already have a Method*");
  oop rmethod_name = java_lang_invoke_ResolvedMethodName::find_resolved_method(_resolved_method, CHECK);
  _resolved_method_name = Handle(THREAD, rmethod_name);
}

#ifdef ASSERT
void CallInfo::verify() {
  switch (call_kind()) {  // the meaning and allowed value of index depends on kind
  case CallInfo::direct_call:
    if (_call_index == Method::nonvirtual_vtable_index)  break;
    // else fall through to check vtable index:
  case CallInfo::vtable_call:
    assert(resolved_klass()->verify_vtable_index(_call_index), "");
    break;
  case CallInfo::itable_call:
    assert(resolved_method()->method_holder()->verify_itable_index(_call_index), "");
    break;
  case CallInfo::unknown_kind:
    assert(call_kind() != CallInfo::unknown_kind, "CallInfo must be set");
    break;
  default:
    fatal("Unexpected call kind %d", call_kind());
  }
}
#endif // ASSERT

#ifndef PRODUCT
void CallInfo::print() {
  ResourceMark rm;
  const char* kindstr;
  switch (_call_kind) {
  case direct_call: kindstr = "direct";  break;
  case vtable_call: kindstr = "vtable";  break;
  case itable_call: kindstr = "itable";  break;
  default         : kindstr = "unknown"; break;
  }
  tty->print_cr("Call %s@%d %s", kindstr, _call_index,
                _resolved_method.is_null() ? "(none)" : _resolved_method->name_and_sig_as_C_string());
}
#endif

//------------------------------------------------------------------------------------------------------------------------
// Implementation of LinkInfo

LinkInfo::LinkInfo(const constantPoolHandle& pool, int index, const methodHandle& current_method, Bytecodes::Code code, TRAPS) {
   // resolve klass
  _resolved_klass = pool->klass_ref_at(index, code, CHECK);

  // Get name, signature, and static klass
  _name          = pool->name_ref_at(index, code);
  _signature     = pool->signature_ref_at(index, code);
  _tag           = pool->tag_ref_at(index, code);
  _current_klass = pool->pool_holder();
  _current_method = current_method;

  // Coming from the constant pool always checks access
  _check_access  = true;
  _check_loader_constraints = true;
}

LinkInfo::LinkInfo(const constantPoolHandle& pool, int index, Bytecodes::Code code, TRAPS) {
   // resolve klass
  _resolved_klass = pool->klass_ref_at(index, code, CHECK);

  // Get name, signature, and static klass
  _name          = pool->name_ref_at(index, code);
  _signature     = pool->signature_ref_at(index, code);
  _tag           = pool->tag_ref_at(index, code);
  _current_klass = pool->pool_holder();
  _current_method = methodHandle();

  // Coming from the constant pool always checks access
  _check_access  = true;
  _check_loader_constraints = true;
}

#ifndef PRODUCT
void LinkInfo::print() {
  ResourceMark rm;
  tty->print_cr("Link resolved_klass=%s name=%s signature=%s current_klass=%s check_access=%s check_loader_constraints=%s",
                _resolved_klass->name()->as_C_string(),
                _name->as_C_string(),
                _signature->as_C_string(),
                _current_klass == nullptr ? "(none)" : _current_klass->name()->as_C_string(),
                _check_access ? "true" : "false",
                _check_loader_constraints ? "true" : "false");

}
#endif // PRODUCT
//------------------------------------------------------------------------------------------------------------------------
// Klass resolution

void LinkResolver::check_klass_accessibility(Klass* ref_klass, Klass* sel_klass, TRAPS) {
  Klass* base_klass = sel_klass;
  if (sel_klass->is_objArray_klass()) {
    base_klass = ObjArrayKlass::cast(sel_klass)->bottom_klass();
  }
  // The element type could be a typeArray - we only need the access
  // check if it is a reference to another class.
  if (!base_klass->is_instance_klass()) {
    return;  // no relevant check to do
  }

  Reflection::VerifyClassAccessResults vca_result =
    Reflection::verify_class_access(ref_klass, InstanceKlass::cast(base_klass), true);
  if (vca_result != Reflection::ACCESS_OK) {
    ResourceMark rm(THREAD);
    char* msg = Reflection::verify_class_access_msg(ref_klass,
                                                    InstanceKlass::cast(base_klass),
                                                    vca_result);
    bool same_module = (base_klass->module() == ref_klass->module());
    if (msg == nullptr) {
      Exceptions::fthrow(
        THREAD_AND_LOCATION,
        vmSymbols::java_lang_IllegalAccessError(),
        "failed to access class %s from class %s (%s%s%s)",
        base_klass->external_name(),
        ref_klass->external_name(),
        (same_module) ? base_klass->joint_in_module_of_loader(ref_klass) : base_klass->class_in_module_of_loader(),
        (same_module) ? "" : "; ",
        (same_module) ? "" : ref_klass->class_in_module_of_loader());
    } else {
      // Use module specific message returned by verify_class_access_msg().
      Exceptions::fthrow(
        THREAD_AND_LOCATION,
        vmSymbols::java_lang_IllegalAccessError(),
        "%s", msg);
    }
  }
}

//------------------------------------------------------------------------------------------------------------------------
// Method resolution
//
// According to JVM spec. $5.4.3c & $5.4.3d

// Look up method in klasses, including static methods
// Then look up local default methods
Method* LinkResolver::lookup_method_in_klasses(const LinkInfo& link_info,
                                               bool checkpolymorphism,
                                               bool in_imethod_resolve) {
  NoSafepointVerifier nsv;  // Method* returned may not be reclaimed

  Klass* klass = link_info.resolved_klass();
  Symbol* name = link_info.name();
  Symbol* signature = link_info.signature();

  // Ignore overpasses so statics can be found during resolution
  Method* result = klass->uncached_lookup_method(name, signature, Klass::OverpassLookupMode::skip);

  if (klass->is_array_klass()) {
    // Only consider klass and super klass for arrays
    return result;
  }

  InstanceKlass* ik = InstanceKlass::cast(klass);

  // JDK 8, JVMS 5.4.3.4: Interface method resolution should
  // ignore static and non-public methods of java.lang.Object,
  // like clone and finalize.
  if (in_imethod_resolve &&
      result != nullptr &&
      ik->is_interface() &&
      (result->is_static() || !result->is_public()) &&
      result->method_holder() == vmClasses::Object_klass()) {
    result = nullptr;
  }

  // Before considering default methods, check for an overpass in the
  // current class if a method has not been found.
  if (result == nullptr) {
    result = ik->find_method(name, signature);
  }

  if (result == nullptr) {
    Array<Method*>* default_methods = ik->default_methods();
    if (default_methods != nullptr) {
      result = InstanceKlass::find_method(default_methods, name, signature);
    }
  }

  if (checkpolymorphism && result != nullptr) {
    vmIntrinsics::ID iid = result->intrinsic_id();
    if (MethodHandles::is_signature_polymorphic(iid)) {
      // Do not link directly to these.  The VM must produce a synthetic one using lookup_polymorphic_method.
      return nullptr;
    }
  }
  return result;
}

// returns first instance method
// Looks up method in classes, then looks up local default methods
Method* LinkResolver::lookup_instance_method_in_klasses(Klass* klass,
                                                        Symbol* name,
                                                        Symbol* signature,
                                                        Klass::PrivateLookupMode private_mode) {
  Method* result = klass->uncached_lookup_method(name, signature, Klass::OverpassLookupMode::find, private_mode);

  while (result != nullptr && result->is_static() && result->method_holder()->super() != nullptr) {
    Klass* super_klass = result->method_holder()->super();
    result = super_klass->uncached_lookup_method(name, signature, Klass::OverpassLookupMode::find, private_mode);
  }

  if (klass->is_array_klass()) {
    // Only consider klass and super klass for arrays
    return result;
  }

  if (result == nullptr) {
    Array<Method*>* default_methods = InstanceKlass::cast(klass)->default_methods();
    if (default_methods != nullptr) {
      result = InstanceKlass::find_method(default_methods, name, signature);
      assert(result == nullptr || !result->is_static(), "static defaults not allowed");
    }
  }
  return result;
}

int LinkResolver::vtable_index_of_interface_method(Klass* klass, const methodHandle& resolved_method) {
  InstanceKlass* ik = InstanceKlass::cast(klass);
  return ik->vtable_index_of_interface_method(resolved_method());
}

Method* LinkResolver::lookup_method_in_interfaces(const LinkInfo& cp_info) {
  InstanceKlass *ik = InstanceKlass::cast(cp_info.resolved_klass());

  // Specify 'true' in order to skip default methods when searching the
  // interfaces.  Function lookup_method_in_klasses() already looked for
  // the method in the default methods table.
  return ik->lookup_method_in_all_interfaces(cp_info.name(), cp_info.signature(), Klass::DefaultsLookupMode::skip);
}

Method* LinkResolver::lookup_polymorphic_method(const LinkInfo& link_info,
                                                Handle *appendix_result_or_null,
                                                TRAPS) {
  ResourceMark rm(THREAD);
  Klass* klass = link_info.resolved_klass();
  Symbol* name = link_info.name();
  Symbol* full_signature = link_info.signature();
  LogTarget(Info, methodhandles) lt_mh;

  vmIntrinsics::ID iid = MethodHandles::signature_polymorphic_name_id(name);
  log_info(methodhandles)("lookup_polymorphic_method iid=%s %s.%s%s",
                          vmIntrinsics::name_at(iid), klass->external_name(),
                          name->as_C_string(), full_signature->as_C_string());
  if ((klass == vmClasses::MethodHandle_klass() ||
       klass == vmClasses::VarHandle_klass()) &&
      iid != vmIntrinsics::_none) {
    if (MethodHandles::is_signature_polymorphic_intrinsic(iid)) {
      // Most of these do not need an up-call to Java to resolve, so can be done anywhere.
      // Do not erase last argument type (MemberName) if it is a static linkTo method.
      bool keep_last_arg = MethodHandles::is_signature_polymorphic_static(iid);
      TempNewSymbol basic_signature =
        MethodHandles::lookup_basic_type_signature(full_signature, keep_last_arg);
      log_info(methodhandles)("lookup_polymorphic_method %s %s => basic %s",
                              name->as_C_string(),
                              full_signature->as_C_string(),
                              basic_signature->as_C_string());
      Method* result = SystemDictionary::find_method_handle_intrinsic(iid,
                                                              basic_signature,
                                                              CHECK_NULL);
      if (result != nullptr) {
        assert(result->is_method_handle_intrinsic(), "MH.invokeBasic or MH.linkTo* intrinsic");
        assert(result->intrinsic_id() != vmIntrinsics::_invokeGeneric, "wrong place to find this");
        assert(basic_signature == result->signature(), "predict the result signature");
        if (lt_mh.is_enabled()) {
          LogStream ls(lt_mh);
          ls.print("lookup_polymorphic_method => intrinsic ");
          result->print_on(&ls);
        }
      }
      return result;
    } else if (iid == vmIntrinsics::_invokeGeneric
               && THREAD->can_call_java()
               && appendix_result_or_null != nullptr) {
      // This is a method with type-checking semantics.
      // We will ask Java code to spin an adapter method for it.
      if (!MethodHandles::enabled()) {
        // Make sure the Java part of the runtime has been booted up.
        Klass* natives = vmClasses::MethodHandleNatives_klass();
        if (natives == nullptr || InstanceKlass::cast(natives)->is_not_initialized()) {
          SystemDictionary::resolve_or_fail(vmSymbols::java_lang_invoke_MethodHandleNatives(),
                                            Handle(),
                                            Handle(),
                                            true,
                                            CHECK_NULL);
        }
      }

      Handle appendix;
      Method* result = SystemDictionary::find_method_handle_invoker(klass,
                                                                    name,
                                                                    full_signature,
                                                                    link_info.current_klass(),
                                                                    &appendix,
                                                                    CHECK_NULL);
      if (lt_mh.is_enabled()) {
        LogStream ls(lt_mh);
        ls.print("lookup_polymorphic_method => (via Java) ");
        result->print_on(&ls);
        ls.print("  lookup_polymorphic_method => appendix = ");
        appendix.is_null() ? ls.print_cr("(none)") : appendix->print_on(&ls);
      }
      if (result != nullptr) {
#ifdef ASSERT
        ResourceMark rm(THREAD);

        TempNewSymbol basic_signature =
          MethodHandles::lookup_basic_type_signature(full_signature);
        int actual_size_of_params = result->size_of_parameters();
        int expected_size_of_params = ArgumentSizeComputer(basic_signature).size();
        // +1 for MethodHandle.this, +1 for trailing MethodType
        if (!MethodHandles::is_signature_polymorphic_static(iid))  expected_size_of_params += 1;
        if (appendix.not_null())                                   expected_size_of_params += 1;
        if (actual_size_of_params != expected_size_of_params) {
          tty->print_cr("*** basic_signature=%s", basic_signature->as_C_string());
          tty->print_cr("*** result for %s: ", vmIntrinsics::name_at(iid));
          result->print();
        }
        assert(actual_size_of_params == expected_size_of_params,
               "%d != %d", actual_size_of_params, expected_size_of_params);
#endif //ASSERT

        assert(appendix_result_or_null != nullptr, "");
        (*appendix_result_or_null) = appendix;
      }
      return result;
    }
  }
  return nullptr;
}

static void print_nest_host_error_on(stringStream* ss, Klass* ref_klass, Klass* sel_klass) {
  assert(ref_klass->is_instance_klass(), "must be");
  assert(sel_klass->is_instance_klass(), "must be");
  InstanceKlass* ref_ik = InstanceKlass::cast(ref_klass);
  InstanceKlass* sel_ik = InstanceKlass::cast(sel_klass);
  const char* nest_host_error_1 = ref_ik->nest_host_error();
  const char* nest_host_error_2 = sel_ik->nest_host_error();
  if (nest_host_error_1 != nullptr || nest_host_error_2 != nullptr) {
    ss->print(", (%s%s%s)",
              (nest_host_error_1 != nullptr) ? nest_host_error_1 : "",
              (nest_host_error_1 != nullptr && nest_host_error_2 != nullptr) ? ", " : "",
              (nest_host_error_2 != nullptr) ? nest_host_error_2 : "");
  }
}

void LinkResolver::check_method_accessability(Klass* ref_klass,
                                              Klass* resolved_klass,
                                              Klass* sel_klass,
                                              const methodHandle& sel_method,
                                              TRAPS) {

  AccessFlags flags = sel_method->access_flags();

  // Special case:  arrays always override "clone". JVMS 2.15.
  // If the resolved klass is an array class, and the declaring class
  // is java.lang.Object and the method is "clone", set the flags
  // to public.
  //
  // We'll check for the method name first, as that's most likely
  // to be false (so we'll short-circuit out of these tests).
  if (sel_method->name() == vmSymbols::clone_name() &&
      sel_klass == vmClasses::Object_klass() &&
      resolved_klass->is_array_klass()) {
    // We need to change "protected" to "public".
    assert(flags.is_protected(), "clone not protected?");
    jint new_flags = flags.as_int();
    new_flags = new_flags & (~JVM_ACC_PROTECTED);
    new_flags = new_flags | JVM_ACC_PUBLIC;
    flags.set_flags(new_flags);
  }
//  assert(extra_arg_result_or_null != nullptr, "must be able to return extra argument");

  bool can_access = Reflection::verify_member_access(ref_klass,
                                                     resolved_klass,
                                                     sel_klass,
                                                     flags,
                                                     true, false, CHECK);
  // Any existing exceptions that may have been thrown
  // have been allowed to propagate.
  if (!can_access) {
    ResourceMark rm(THREAD);
    stringStream ss;
    bool same_module = (sel_klass->module() == ref_klass->module());
    ss.print("class %s tried to access %s%s%smethod '%s' (%s%s%s)",
             ref_klass->external_name(),
             sel_method->is_abstract()  ? "abstract "  : "",
             sel_method->is_protected() ? "protected " : "",
             sel_method->is_private()   ? "private "   : "",
             sel_method->external_name(),
             (same_module) ? ref_klass->joint_in_module_of_loader(sel_klass) : ref_klass->class_in_module_of_loader(),
             (same_module) ? "" : "; ",
             (same_module) ? "" : sel_klass->class_in_module_of_loader()
             );

    // For private access see if there was a problem with nest host
    // resolution, and if so report that as part of the message.
    if (sel_method->is_private()) {
      print_nest_host_error_on(&ss, ref_klass, sel_klass);
    }

    Exceptions::fthrow(THREAD_AND_LOCATION,
                       vmSymbols::java_lang_IllegalAccessError(),
                       "%s",
                       ss.as_string()
                       );
    return;
  }
}

void LinkResolver::resolve_continuation_enter(CallInfo& callinfo, TRAPS) {
  Klass* resolved_klass = vmClasses::Continuation_klass();
  Symbol* method_name = vmSymbols::enter_name();
  Symbol* method_signature = vmSymbols::continuationEnter_signature();
  Klass*  current_klass = resolved_klass;
  LinkInfo link_info(resolved_klass, method_name, method_signature, current_klass);
  Method* resolved_method = resolve_method(link_info, Bytecodes::_invokestatic, CHECK);
  callinfo.set_static(resolved_klass, methodHandle(THREAD, resolved_method), CHECK);
}

Method* LinkResolver::resolve_method_statically(Bytecodes::Code code,
                                                const constantPoolHandle& pool, int index, TRAPS) {
  // This method is used only
  // (1) in C2 from InlineTree::ok_to_inline (via ciMethod::check_call),
  // and
  // (2) in Bytecode_invoke::static_target
  // It appears to fail when applied to an invokeinterface call site.
  // FIXME: Remove this method and ciMethod::check_call; refactor to use the other LinkResolver entry points.
  // resolve klass
  if (code == Bytecodes::_invokedynamic) {
    Klass* resolved_klass = vmClasses::MethodHandle_klass();
    Symbol* method_name = vmSymbols::invoke_name();
    Symbol* method_signature = pool->signature_ref_at(index, code);
    Klass*  current_klass = pool->pool_holder();
    LinkInfo link_info(resolved_klass, method_name, method_signature, current_klass);
    return resolve_method(link_info, code, THREAD);
  }

  LinkInfo link_info(pool, index, methodHandle(), code, CHECK_NULL);
  Klass* resolved_klass = link_info.resolved_klass();

  if (pool->has_preresolution()
      || ((resolved_klass == vmClasses::MethodHandle_klass() || resolved_klass == vmClasses::VarHandle_klass()) &&
          MethodHandles::is_signature_polymorphic_name(resolved_klass, link_info.name()))) {
    Method* result = ConstantPool::method_at_if_loaded(pool, index);
    if (result != nullptr) {
      return result;
    }
  }

  if (code == Bytecodes::_invokeinterface) {
    return resolve_interface_method(link_info, code, THREAD);
  } else if (code == Bytecodes::_invokevirtual) {
    return resolve_method(link_info, code, THREAD);
  } else if (!resolved_klass->is_interface()) {
    return resolve_method(link_info, code, THREAD);
  } else {
    return resolve_interface_method(link_info, code, THREAD);
  }
}

// Check and print a loader constraint violation message for method or interface method
void LinkResolver::check_method_loader_constraints(const LinkInfo& link_info,
                                                   const methodHandle& resolved_method,
                                                   const char* method_type, TRAPS) {
  Handle current_loader(THREAD, link_info.current_klass()->class_loader());
  Handle resolved_loader(THREAD, resolved_method->method_holder()->class_loader());

  ResourceMark rm(THREAD);
  Symbol* failed_type_symbol =
    SystemDictionary::check_signature_loaders(link_info.signature(),
                                              /*klass_being_linked*/ nullptr, // We are not linking class
                                              current_loader,
                                              resolved_loader, true);
  if (failed_type_symbol != nullptr) {
    Klass* current_class = link_info.current_klass();
    ClassLoaderData* current_loader_data = current_class->class_loader_data();
    assert(current_loader_data != nullptr, "current class has no class loader data");
    Klass* resolved_method_class = resolved_method->method_holder();
    ClassLoaderData* target_loader_data = resolved_method_class->class_loader_data();
    assert(target_loader_data != nullptr, "resolved method's class has no class loader data");

    stringStream ss;
    ss.print("loader constraint violation: when resolving %s '", method_type);
    Method::print_external_name(&ss, link_info.resolved_klass(), link_info.name(), link_info.signature());
    ss.print("' the class loader %s of the current class, %s,"
             " and the class loader %s for the method's defining class, %s, have"
             " different Class objects for the type %s used in the signature (%s; %s)",
             current_loader_data->loader_name_and_id(),
             current_class->name()->as_C_string(),
             target_loader_data->loader_name_and_id(),
             resolved_method_class->name()->as_C_string(),
             failed_type_symbol->as_C_string(),
             current_class->class_in_module_of_loader(false, true),
             resolved_method_class->class_in_module_of_loader(false, true));
    THROW_MSG(vmSymbols::java_lang_LinkageError(), ss.as_string());
  }
}

void LinkResolver::check_field_loader_constraints(Symbol* field, Symbol* sig,
                                                  Klass* current_klass,
                                                  Klass* sel_klass, TRAPS) {
  Handle ref_loader(THREAD, current_klass->class_loader());
  Handle sel_loader(THREAD, sel_klass->class_loader());

  ResourceMark rm(THREAD);  // needed for check_signature_loaders
  Symbol* failed_type_symbol =
    SystemDictionary::check_signature_loaders(sig,
                                              /*klass_being_linked*/ nullptr, // We are not linking class
                                              ref_loader, sel_loader,
                                              false);
  if (failed_type_symbol != nullptr) {
    stringStream ss;
    const char* failed_type_name = failed_type_symbol->as_klass_external_name();

    ss.print("loader constraint violation: when resolving field \"%s\" of type %s, "
             "the class loader %s of the current class, %s, "
             "and the class loader %s for the field's defining %s, %s, "
             "have different Class objects for type %s (%s; %s)",
             field->as_C_string(),
             failed_type_name,
             current_klass->class_loader_data()->loader_name_and_id(),
             current_klass->external_name(),
             sel_klass->class_loader_data()->loader_name_and_id(),
             sel_klass->external_kind(),
             sel_klass->external_name(),
             failed_type_name,
             current_klass->class_in_module_of_loader(false, true),
             sel_klass->class_in_module_of_loader(false, true));
    THROW_MSG(vmSymbols::java_lang_LinkageError(), ss.as_string());
  }
}

Method* LinkResolver::resolve_method(const LinkInfo& link_info,
                                     Bytecodes::Code code, TRAPS) {

  Handle nested_exception;
  Klass* resolved_klass = link_info.resolved_klass();

  // 1. For invokevirtual, cannot call an interface method
  if (code == Bytecodes::_invokevirtual && resolved_klass->is_interface()) {
    ResourceMark rm(THREAD);
    char buf[200];
    jio_snprintf(buf, sizeof(buf), "Found interface %s, but class was expected",
        resolved_klass->external_name());
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), buf);
  }

  // 2. check constant pool tag for called method - must be JVM_CONSTANT_Methodref
  if (!link_info.tag().is_invalid() && !link_info.tag().is_method()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Method '");
    Method::print_external_name(&ss, link_info.resolved_klass(), link_info.name(), link_info.signature());
    ss.print("' must be Methodref constant");
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }

  // 3. lookup method in resolved klass and its super klasses
  methodHandle resolved_method(THREAD, lookup_method_in_klasses(link_info, true, false));

  // 4. lookup method in all the interfaces implemented by the resolved klass
  if (resolved_method.is_null() && !resolved_klass->is_array_klass()) { // not found in the class hierarchy
    resolved_method = methodHandle(THREAD, lookup_method_in_interfaces(link_info));

    if (resolved_method.is_null()) {
      // JSR 292:  see if this is an implicitly generated method MethodHandle.linkToVirtual(*...), etc
      Method* method = lookup_polymorphic_method(link_info, (Handle*)nullptr, THREAD);
      resolved_method = methodHandle(THREAD, method);
      if (HAS_PENDING_EXCEPTION) {
        nested_exception = Handle(THREAD, PENDING_EXCEPTION);
        CLEAR_PENDING_EXCEPTION;
      }
    }
  }

  // 5. method lookup failed
  if (resolved_method.is_null()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("'");
    Method::print_external_name(&ss, resolved_klass, link_info.name(), link_info.signature());
    ss.print("'");
    THROW_MSG_CAUSE_(vmSymbols::java_lang_NoSuchMethodError(),
                     ss.as_string(), nested_exception, nullptr);
  }

  // 6. access checks, access checking may be turned off when calling from within the VM.
  Klass* current_klass = link_info.current_klass();
  if (link_info.check_access()) {
    assert(current_klass != nullptr , "current_klass should not be null");

    // check if method can be accessed by the referring class
    check_method_accessability(current_klass,
                               resolved_klass,
                               resolved_method->method_holder(),
                               resolved_method,
                               CHECK_NULL);
  }
  if (link_info.check_loader_constraints()) {
    // check loader constraints
    check_method_loader_constraints(link_info, resolved_method, "method", CHECK_NULL);
  }

  return resolved_method();
}

static void trace_method_resolution(const char* prefix,
                                    Klass* klass,
                                    Klass* resolved_klass,
                                    Method* method,
                                    bool logitables,
                                    int index = -1) {
#ifndef PRODUCT
  ResourceMark rm;
  Log(itables) logi;
  LogStream lsi(logi.trace());
  Log(vtables) logv;
  LogStream lsv(logv.trace());
  outputStream* st;
  if (logitables) {
    st = &lsi;
  } else {
    st = &lsv;
  }
  st->print("%s%s, compile-time-class:%s, method:%s, method_holder:%s, access_flags: ",
            prefix,
            (klass == nullptr ? "<null>" : klass->internal_name()),
            resolved_klass->internal_name(),
            Method::name_and_sig_as_C_string(resolved_klass,
                                             method->name(),
                                             method->signature()),
            method->method_holder()->internal_name());
  method->print_linkage_flags(st);
  if (index != -1) {
    st->print("vtable_index:%d", index);
  }
  st->cr();
#endif // PRODUCT
}

// Do linktime resolution of a method in the interface within the context of the specified bytecode.
Method* LinkResolver::resolve_interface_method(const LinkInfo& link_info, Bytecodes::Code code, TRAPS) {

  Klass* resolved_klass = link_info.resolved_klass();

  // check if klass is interface
  if (!resolved_klass->is_interface()) {
    ResourceMark rm(THREAD);
    char buf[200];
    jio_snprintf(buf, sizeof(buf), "Found class %s, but interface was expected", resolved_klass->external_name());
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), buf);
  }

  // check constant pool tag for called method - must be JVM_CONSTANT_InterfaceMethodref
  if (!link_info.tag().is_invalid() && !link_info.tag().is_interface_method()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Method '");
    Method::print_external_name(&ss, link_info.resolved_klass(), link_info.name(), link_info.signature());
    ss.print("' must be InterfaceMethodref constant");
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }

  // lookup method in this interface or its super, java.lang.Object
  // JDK8: also look for static methods
  methodHandle resolved_method(THREAD, lookup_method_in_klasses(link_info, false, true));

  if (resolved_method.is_null() && !resolved_klass->is_array_klass()) {
    // lookup method in all the super-interfaces
    resolved_method = methodHandle(THREAD, lookup_method_in_interfaces(link_info));
  }

  if (resolved_method.is_null()) {
    // no method found
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("'");
    Method::print_external_name(&ss, resolved_klass, link_info.name(), link_info.signature());
    ss.print("'");
    THROW_MSG_NULL(vmSymbols::java_lang_NoSuchMethodError(), ss.as_string());
  }

  if (link_info.check_access()) {
    // JDK8 adds non-public interface methods, and accessability check requirement
    Klass* current_klass = link_info.current_klass();

    assert(current_klass != nullptr , "current_klass should not be null");

    // check if method can be accessed by the referring class
    check_method_accessability(current_klass,
                               resolved_klass,
                               resolved_method->method_holder(),
                               resolved_method,
                               CHECK_NULL);
  }
  if (link_info.check_loader_constraints()) {
    check_method_loader_constraints(link_info, resolved_method, "interface method", CHECK_NULL);
  }

  if (code != Bytecodes::_invokestatic && resolved_method->is_static()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Expected instance not static method '");
    Method::print_external_name(&ss, resolved_klass,
                                resolved_method->name(), resolved_method->signature());
    ss.print("'");
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }

  if (log_develop_is_enabled(Trace, itables)) {
    char buf[200];
    jio_snprintf(buf, sizeof(buf), "%s resolved interface method: caller-class:",
                 Bytecodes::name(code));
    trace_method_resolution(buf, link_info.current_klass(), resolved_klass, resolved_method(), true);
  }

  return resolved_method();
}

//------------------------------------------------------------------------------------------------------------------------
// Field resolution

void LinkResolver::check_field_accessability(Klass* ref_klass,
                                             Klass* resolved_klass,
                                             Klass* sel_klass,
                                             const fieldDescriptor& fd,
                                             TRAPS) {
  bool can_access = Reflection::verify_member_access(ref_klass,
                                                     resolved_klass,
                                                     sel_klass,
                                                     fd.access_flags(),
                                                     true, false, CHECK);
  // Any existing exceptions that may have been thrown, for example LinkageErrors
  // from nest-host resolution, have been allowed to propagate.
  if (!can_access) {
    bool same_module = (sel_klass->module() == ref_klass->module());
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("class %s tried to access %s%sfield %s.%s (%s%s%s)",
             ref_klass->external_name(),
             fd.is_protected() ? "protected " : "",
             fd.is_private()   ? "private "   : "",
             sel_klass->external_name(),
             fd.name()->as_C_string(),
             (same_module) ? ref_klass->joint_in_module_of_loader(sel_klass) : ref_klass->class_in_module_of_loader(),
             (same_module) ? "" : "; ",
             (same_module) ? "" : sel_klass->class_in_module_of_loader()
             );
    // For private access see if there was a problem with nest host
    // resolution, and if so report that as part of the message.
    if (fd.is_private()) {
      print_nest_host_error_on(&ss, ref_klass, sel_klass);
    }
    Exceptions::fthrow(THREAD_AND_LOCATION,
                       vmSymbols::java_lang_IllegalAccessError(),
                       "%s",
                       ss.as_string()
                       );
    return;
  }
}

void LinkResolver::resolve_field_access(fieldDescriptor& fd, const constantPoolHandle& pool, int index, const methodHandle& method, Bytecodes::Code byte, TRAPS) {
  LinkInfo link_info(pool, index, method, byte, CHECK);
  resolve_field(fd, link_info, byte, true, CHECK);
}

void LinkResolver::resolve_field(fieldDescriptor& fd,
                                 const LinkInfo& link_info,
                                 Bytecodes::Code byte, bool initialize_class,
                                 TRAPS) {
  assert(byte == Bytecodes::_getstatic || byte == Bytecodes::_putstatic ||
         byte == Bytecodes::_getfield  || byte == Bytecodes::_putfield  ||
         byte == Bytecodes::_nofast_getfield  || byte == Bytecodes::_nofast_putfield  ||
         (byte == Bytecodes::_nop && !link_info.check_access()), "bad field access bytecode");

  bool is_static = (byte == Bytecodes::_getstatic || byte == Bytecodes::_putstatic);
  bool is_put    = (byte == Bytecodes::_putfield  || byte == Bytecodes::_putstatic || byte == Bytecodes::_nofast_putfield);
  // Check if there's a resolved klass containing the field
  Klass* resolved_klass = link_info.resolved_klass();
  Symbol* field = link_info.name();
  Symbol* sig = link_info.signature();

  // Resolve instance field
  Klass* sel_klass = resolved_klass->find_field(field, sig, &fd);
  // check if field exists; i.e., if a klass containing the field def has been selected
  if (sel_klass == nullptr) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Class %s does not have member field '", resolved_klass->external_name());
    sig->print_as_field_external_type(&ss);
    ss.print(" %s'", field->as_C_string());
    THROW_MSG(vmSymbols::java_lang_NoSuchFieldError(), ss.as_string());
  }

  // Access checking may be turned off when calling from within the VM.
  Klass* current_klass = link_info.current_klass();
  if (link_info.check_access()) {

    // check access
    check_field_accessability(current_klass, resolved_klass, sel_klass, fd, CHECK);

    // check for errors
    if (is_static != fd.is_static()) {
      ResourceMark rm(THREAD);
      char msg[200];
      jio_snprintf(msg, sizeof(msg), "Expected %s field %s.%s", is_static ? "static" : "non-static", resolved_klass->external_name(), fd.name()->as_C_string());
      THROW_MSG(vmSymbols::java_lang_IncompatibleClassChangeError(), msg);
    }

    // A final field can be modified only
    // (1) by methods declared in the class declaring the field and
    // (2) by the <clinit> method (in case of a static field)
    //     or by the <init> method (in case of an instance field).
    if (is_put && fd.access_flags().is_final()) {

      if (sel_klass != current_klass) {
        ResourceMark rm(THREAD);
        stringStream ss;
        ss.print("Update to %s final field %s.%s attempted from a different class (%s) than the field's declaring class",
                 is_static ? "static" : "non-static", resolved_klass->external_name(), fd.name()->as_C_string(),
                current_klass->external_name());
        THROW_MSG(vmSymbols::java_lang_IllegalAccessError(), ss.as_string());
      }

      if (fd.constants()->pool_holder()->major_version() >= 53) {
        Method* m = link_info.current_method();
        assert(m != nullptr, "information about the current method must be available for 'put' bytecodes");
        bool is_initialized_static_final_update = (byte == Bytecodes::_putstatic &&
                                                   fd.is_static() &&
                                                   !m->is_static_initializer());
        bool is_initialized_instance_final_update = ((byte == Bytecodes::_putfield || byte == Bytecodes::_nofast_putfield) &&
                                                     !fd.is_static() &&
                                                     !m->is_object_initializer());

        if (is_initialized_static_final_update || is_initialized_instance_final_update) {
          ResourceMark rm(THREAD);
          stringStream ss;
          ss.print("Update to %s final field %s.%s attempted from a different method (%s) than the initializer method %s ",
                   is_static ? "static" : "non-static", resolved_klass->external_name(), fd.name()->as_C_string(),
                   m->name()->as_C_string(),
                   is_static ? "<clinit>" : "<init>");
          THROW_MSG(vmSymbols::java_lang_IllegalAccessError(), ss.as_string());
        }
      }
    }

    // initialize resolved_klass if necessary
    // note 1: the klass which declared the field must be initialized (i.e, sel_klass)
    //         according to the newest JVM spec (5.5, p.170) - was bug (gri 7/28/99)
    //
    // note 2: we don't want to force initialization if we are just checking
    //         if the field access is legal; e.g., during compilation
    if (is_static && initialize_class) {
      sel_klass->initialize(CHECK);
    }
  }

  if (link_info.check_loader_constraints() && (sel_klass != current_klass) && (current_klass != nullptr)) {
    check_field_loader_constraints(field, sig, current_klass, sel_klass, CHECK);
  }

  // return information. note that the klass is set to the actual klass containing the
  // field, otherwise access of static fields in superclasses will not work.
}


//------------------------------------------------------------------------------------------------------------------------
// Invoke resolution
//
// Naming conventions:
//
// resolved_method    the specified method (i.e., static receiver specified via constant pool index)
// sel_method         the selected method  (selected via run-time lookup; e.g., based on dynamic receiver class)
// resolved_klass     the specified klass  (i.e., specified via constant pool index)
// recv_klass         the receiver klass


void LinkResolver::resolve_static_call(CallInfo& result,
                                       const LinkInfo& link_info,
                                       bool initialize_class, TRAPS) {
  Method* resolved_method = linktime_resolve_static_method(link_info, CHECK);

  // The resolved class can change as a result of this resolution.
  Klass* resolved_klass = resolved_method->method_holder();

  // Initialize klass (this should only happen if everything is ok)
  if (initialize_class && resolved_klass->should_be_initialized()) {
    resolved_klass->initialize(CHECK);
    // Use updated LinkInfo to reresolve with resolved method holder
    LinkInfo new_info(resolved_klass, link_info.name(), link_info.signature(),
                      link_info.current_klass(),
                      link_info.check_access() ? LinkInfo::AccessCheck::required : LinkInfo::AccessCheck::skip,
                      link_info.check_loader_constraints() ? LinkInfo::LoaderConstraintCheck::required : LinkInfo::LoaderConstraintCheck::skip);
    resolved_method = linktime_resolve_static_method(new_info, CHECK);
  }

  if (resolved_method->is_continuation_native_intrinsic()
      && resolved_method->from_interpreted_entry() == nullptr) { // does a load_acquire
    methodHandle mh(THREAD, resolved_method);
    // Generate a compiled form of the enterSpecial intrinsic.
    AdapterHandlerLibrary::create_native_wrapper(mh);
  }

  // setup result
  result.set_static(resolved_klass, methodHandle(THREAD, resolved_method), CHECK);
  JFR_ONLY(Jfr::on_resolution(result, CHECK);)
}

// throws linktime exceptions
Method* LinkResolver::linktime_resolve_static_method(const LinkInfo& link_info, TRAPS) {

  Klass* resolved_klass = link_info.resolved_klass();
  Method* resolved_method;
  if (!resolved_klass->is_interface()) {
    resolved_method = resolve_method(link_info, Bytecodes::_invokestatic, CHECK_NULL);
  } else {
    resolved_method = resolve_interface_method(link_info, Bytecodes::_invokestatic, CHECK_NULL);
  }
  assert(resolved_method->name() != vmSymbols::class_initializer_name(), "should have been checked in verifier");

  // check if static
  if (!resolved_method->is_static()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Expected static method '");
    resolved_method->print_external_name(&ss);
    ss.print("'");
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }
  return resolved_method;
}


void LinkResolver::resolve_special_call(CallInfo& result,
                                        Handle recv,
                                        const LinkInfo& link_info,
                                        TRAPS) {
  Method* resolved_method = linktime_resolve_special_method(link_info, CHECK);
  runtime_resolve_special_method(result, link_info, methodHandle(THREAD, resolved_method), recv, CHECK);
}

// throws linktime exceptions
Method* LinkResolver::linktime_resolve_special_method(const LinkInfo& link_info, TRAPS) {

  // Invokespecial is called for multiple special reasons:
  // <init>
  // local private method invocation, for classes and interfaces
  // superclass.method, which can also resolve to a default method
  // and the selected method is recalculated relative to the direct superclass
  // superinterface.method, which explicitly does not check shadowing
  Klass* resolved_klass = link_info.resolved_klass();
  Method* resolved_method = nullptr;

  if (!resolved_klass->is_interface()) {
    resolved_method = resolve_method(link_info, Bytecodes::_invokespecial, CHECK_NULL);
  } else {
    resolved_method = resolve_interface_method(link_info, Bytecodes::_invokespecial, CHECK_NULL);
  }

  // check if method name is <init>, that it is found in same klass as static type
  if (resolved_method->name() == vmSymbols::object_initializer_name() &&
      resolved_method->method_holder() != resolved_klass) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("%s: method '", resolved_klass->external_name());
    resolved_method->signature()->print_as_signature_external_return_type(&ss);
    ss.print(" %s(", resolved_method->name()->as_C_string());
    resolved_method->signature()->print_as_signature_external_parameters(&ss);
    ss.print(")' not found");
    Exceptions::fthrow(
      THREAD_AND_LOCATION,
      vmSymbols::java_lang_NoSuchMethodError(),
      "%s", ss.as_string());
    return nullptr;
  }

  // ensure that invokespecial's interface method reference is in
  // a direct superinterface, not an indirect superinterface
  Klass* current_klass = link_info.current_klass();
  if (current_klass != nullptr && resolved_klass->is_interface()) {
    InstanceKlass* klass_to_check = InstanceKlass::cast(current_klass);
    // Disable verification for the dynamically-generated reflection bytecodes.
    bool is_reflect = klass_to_check->is_subclass_of(
                        vmClasses::reflect_MagicAccessorImpl_klass());

    if (!is_reflect &&
        !klass_to_check->is_same_or_direct_interface(resolved_klass)) {
      ResourceMark rm(THREAD);
      stringStream ss;
      ss.print("Interface method reference: '");
      resolved_method->print_external_name(&ss);
      ss.print("', is in an indirect superinterface of %s",
               current_klass->external_name());
      THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
    }
  }

  // check if not static
  if (resolved_method->is_static()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Expecting non-static method '");
    resolved_method->print_external_name(&ss);
    ss.print("'");
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }

  if (log_develop_is_enabled(Trace, itables)) {
    trace_method_resolution("invokespecial resolved method: caller-class:",
                            current_klass, resolved_klass, resolved_method, true);
  }

  return resolved_method;
}

// throws runtime exceptions
void LinkResolver::runtime_resolve_special_method(CallInfo& result,
                                                  const LinkInfo& link_info,
                                                  const methodHandle& resolved_method,
                                                  Handle recv, TRAPS) {

  Klass* resolved_klass = link_info.resolved_klass();

  // resolved method is selected method unless we have an old-style lookup
  // for a superclass method
  // Invokespecial for a superinterface, resolved method is selected method,
  // no checks for shadowing
  methodHandle sel_method(THREAD, resolved_method());

  if (link_info.check_access() &&
      // check if the method is not <init>
      resolved_method->name() != vmSymbols::object_initializer_name()) {

    Klass* current_klass = link_info.current_klass();

    // Check if the class of the resolved_klass is a superclass
    // (not supertype in order to exclude interface classes) of the current class.
    // This check is not performed for super.invoke for interface methods
    // in super interfaces.
    if (current_klass->is_subclass_of(resolved_klass) &&
        current_klass != resolved_klass) {
      // Lookup super method
      Klass* super_klass = current_klass->super();
      Method* instance_method = lookup_instance_method_in_klasses(super_klass,
                                                     resolved_method->name(),
                                                     resolved_method->signature(),
                                                     Klass::PrivateLookupMode::find);
      sel_method = methodHandle(THREAD, instance_method);

      // check if found
      if (sel_method.is_null()) {
        ResourceMark rm(THREAD);
        stringStream ss;
        ss.print("'");
        resolved_method->print_external_name(&ss);
        ss.print("'");
        THROW_MSG(vmSymbols::java_lang_AbstractMethodError(), ss.as_string());
      // check loader constraints if found a different method
      } else if (link_info.check_loader_constraints() && sel_method() != resolved_method()) {
        check_method_loader_constraints(link_info, sel_method, "method", CHECK);
      }
    }

    // Check that the class of objectref (the receiver) is the current class or interface,
    // or a subtype of the current class or interface (the sender), otherwise invokespecial
    // throws IllegalAccessError.
    // The verifier checks that the sender is a subtype of the class in the I/MR operand.
    // The verifier also checks that the receiver is a subtype of the sender, if the sender is
    // a class.  If the sender is an interface, the check has to be performed at runtime.
    InstanceKlass* sender = InstanceKlass::cast(current_klass);
    if (sender->is_interface() && recv.not_null()) {
      Klass* receiver_klass = recv->klass();
      if (!receiver_klass->is_subtype_of(sender)) {
        ResourceMark rm(THREAD);
        char buf[500];
        jio_snprintf(buf, sizeof(buf),
                     "Receiver class %s must be the current class or a subtype of interface %s",
                     receiver_klass->external_name(),
                     sender->external_name());
        THROW_MSG(vmSymbols::java_lang_IllegalAccessError(), buf);
      }
    }
  }

  // check if not static
  if (sel_method->is_static()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Expecting non-static method '");
    resolved_method->print_external_name(&ss);
    ss.print("'");
    THROW_MSG(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }

  // check if abstract
  if (sel_method->is_abstract()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("'");
    Method::print_external_name(&ss, resolved_klass, sel_method->name(), sel_method->signature());
    ss.print("'");
    THROW_MSG(vmSymbols::java_lang_AbstractMethodError(), ss.as_string());
  }

  if (log_develop_is_enabled(Trace, itables)) {
    trace_method_resolution("invokespecial selected method: resolved-class:",
                            resolved_klass, resolved_klass, sel_method(), true);
  }

  // setup result
  result.set_static(resolved_klass, sel_method, CHECK);
  JFR_ONLY(Jfr::on_resolution(result, CHECK);)
}

void LinkResolver::resolve_virtual_call(CallInfo& result, Handle recv, Klass* receiver_klass,
                                        const LinkInfo& link_info,
                                        bool check_null_and_abstract, TRAPS) {
  Method* resolved_method = linktime_resolve_virtual_method(link_info, CHECK);
  runtime_resolve_virtual_method(result, methodHandle(THREAD, resolved_method),
                                 link_info.resolved_klass(),
                                 recv, receiver_klass,
                                 check_null_and_abstract, CHECK);
}

// throws linktime exceptions
Method* LinkResolver::linktime_resolve_virtual_method(const LinkInfo& link_info,
                                                           TRAPS) {
  // normal method resolution
  Method* resolved_method = resolve_method(link_info, Bytecodes::_invokevirtual, CHECK_NULL);

  assert(resolved_method->name() != vmSymbols::object_initializer_name(), "should have been checked in verifier");
  assert(resolved_method->name() != vmSymbols::class_initializer_name (), "should have been checked in verifier");

  // check if private interface method
  Klass* resolved_klass = link_info.resolved_klass();
  Klass* current_klass = link_info.current_klass();

  // This is impossible, if resolve_klass is an interface, we've thrown icce in resolve_method
  if (resolved_klass->is_interface() && resolved_method->is_private()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("private interface method requires invokespecial, not invokevirtual: method '");
    resolved_method->print_external_name(&ss);
    ss.print("', caller-class: %s",
             (current_klass == nullptr ? "<null>" : current_klass->internal_name()));
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }

  // check if not static
  if (resolved_method->is_static()) {
    ResourceMark rm(THREAD);
    stringStream ss;
    ss.print("Expecting non-static method '");
    resolved_method->print_external_name(&ss);
    ss.print("'");
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }

  if (log_develop_is_enabled(Trace, vtables)) {
    trace_method_resolution("invokevirtual resolved method: caller-class:",
                            current_klass, resolved_klass, resolved_method, false);
  }

  return resolved_method;
}

// throws runtime exceptions
void LinkResolver::runtime_resolve_virtual_method(CallInfo& result,
                                                  const methodHandle& resolved_method,
                                                  Klass* resolved_klass,
                                                  Handle recv,
                                                  Klass* recv_klass,
                                                  bool check_null_and_abstract,
                                                  TRAPS) {

  // setup default return values
  int vtable_index = Method::invalid_vtable_index;
  methodHandle selected_method;

  // runtime method resolution
  if (check_null_and_abstract && recv.is_null()) { // check if receiver exists
    THROW(vmSymbols::java_lang_NullPointerException());
  }

  // Virtual methods cannot be resolved before its klass has been linked, for otherwise the Method*'s
  // has not been rewritten, and the vtable initialized. Make sure to do this after the nullcheck, since
  // a missing receiver might result in a bogus lookup.
  assert(resolved_method->method_holder()->is_linked(), "must be linked");

  // do lookup based on receiver klass using the vtable index
  if (resolved_method->method_holder()->is_interface()) { // default or miranda method
    vtable_index = vtable_index_of_interface_method(resolved_klass, resolved_method);
    assert(vtable_index >= 0 , "we should have valid vtable index at this point");

    selected_method = methodHandle(THREAD, recv_klass->method_at_vtable(vtable_index));
  } else {
    // at this point we are sure that resolved_method is virtual and not
    // a default or miranda method; therefore, it must have a valid vtable index.
    assert(!resolved_method->has_itable_index(), "");
    vtable_index = resolved_method->vtable_index();
    // We could get a negative vtable_index of nonvirtual_vtable_index for private
    // methods, or for final methods. Private methods never appear in the vtable
    // and never override other methods. As an optimization, final methods are
    // never put in the vtable, unless they override an existing method.
    // So if we do get nonvirtual_vtable_index, it means the selected method is the
    // resolved method, and it can never be changed by an override.
    if (vtable_index == Method::nonvirtual_vtable_index) {
      assert(resolved_method->can_be_statically_bound(), "cannot override this method");
      selected_method = resolved_method;
    } else {
      selected_method = methodHandle(THREAD, recv_klass->method_at_vtable(vtable_index));
    }
  }

  // check if method exists
  if (selected_method.is_null()) {
    throw_abstract_method_error(resolved_method, recv_klass, CHECK);
  }

  // check if abstract
  if (check_null_and_abstract && selected_method->is_abstract()) {
    // Pass arguments for generating a verbose error message.
    throw_abstract_method_error(resolved_method, selected_method, recv_klass, CHECK);
  }

  if (log_develop_is_enabled(Trace, vtables)) {
    trace_method_resolution("invokevirtual selected method: receiver-class:",
                            recv_klass, resolved_klass, selected_method(),
                            false, vtable_index);
  }
  soroush_trace_runtime_dispatch("invokevirtual",
                                 recv_klass,
                                 resolved_method,
                                 selected_method,
                                 THREAD);
  // setup result
  result.set_virtual(resolved_klass, resolved_method, selected_method, vtable_index, CHECK);
  JFR_ONLY(Jfr::on_resolution(result, CHECK);)
}

void LinkResolver::resolve_interface_call(CallInfo& result, Handle recv, Klass* recv_klass,
                                          const LinkInfo& link_info,
                                          bool check_null_and_abstract, TRAPS) {
  // throws linktime exceptions
  Method* resolved_method = linktime_resolve_interface_method(link_info, CHECK);
  methodHandle mh(THREAD, resolved_method);
  runtime_resolve_interface_method(result, mh, link_info.resolved_klass(),
                                   recv, recv_klass, check_null_and_abstract, CHECK);
}

Method* LinkResolver::linktime_resolve_interface_method(const LinkInfo& link_info,
                                                             TRAPS) {
  // normal interface method resolution
  Method* resolved_method = resolve_interface_method(link_info, Bytecodes::_invokeinterface, CHECK_NULL);
  assert(resolved_method->name() != vmSymbols::object_initializer_name(), "should have been checked in verifier");
  assert(resolved_method->name() != vmSymbols::class_initializer_name (), "should have been checked in verifier");

  return resolved_method;
}

// throws runtime exceptions
void LinkResolver::runtime_resolve_interface_method(CallInfo& result,
                                                    const methodHandle& resolved_method,
                                                    Klass* resolved_klass,
                                                    Handle recv,
                                                    Klass* recv_klass,
                                                    bool check_null_and_abstract, TRAPS) {

  // check if receiver exists
  if (check_null_and_abstract && recv.is_null()) {
    THROW(vmSymbols::java_lang_NullPointerException());
  }

  // check if receiver klass implements the resolved interface
  if (!recv_klass->is_subtype_of(resolved_klass)) {
    ResourceMark rm(THREAD);
    char buf[200];
    jio_snprintf(buf, sizeof(buf), "Class %s does not implement the requested interface %s",
                 recv_klass->external_name(),
                 resolved_klass->external_name());
    THROW_MSG(vmSymbols::java_lang_IncompatibleClassChangeError(), buf);
  }

  methodHandle selected_method = resolved_method;

  // resolve the method in the receiver class, unless it is private
  if (!resolved_method()->is_private()) {
    // do lookup based on receiver klass
    // This search must match the linktime preparation search for itable initialization
    // to correctly enforce loader constraints for interface method inheritance.
    // Private methods are skipped as the resolved method was not private.
    Method* method = lookup_instance_method_in_klasses(recv_klass,
                                                       resolved_method->name(),
                                                       resolved_method->signature(),
                                                       Klass::PrivateLookupMode::skip);
    selected_method = methodHandle(THREAD, method);

    if (selected_method.is_null() && !check_null_and_abstract) {
      // In theory this is a harmless placeholder value, but
      // in practice leaving in null affects the nsk default method tests.
      // This needs further study.
      selected_method = resolved_method;
    }
    // check if method exists
    if (selected_method.is_null()) {
      // Pass arguments for generating a verbose error message.
      throw_abstract_method_error(resolved_method, recv_klass, CHECK);
    }
    // check access
    // Throw Illegal Access Error if selected_method is not public.
    if (!selected_method->is_public()) {
      ResourceMark rm(THREAD);
      stringStream ss;
      ss.print("'");
      Method::print_external_name(&ss, recv_klass, selected_method->name(), selected_method->signature());
      ss.print("'");
      THROW_MSG(vmSymbols::java_lang_IllegalAccessError(), ss.as_string());
    }
    // check if abstract
    if (check_null_and_abstract && selected_method->is_abstract()) {
      throw_abstract_method_error(resolved_method, selected_method, recv_klass, CHECK);
    }
  }

  if (log_develop_is_enabled(Trace, itables)) {
    trace_method_resolution("invokeinterface selected method: receiver-class:",
                            recv_klass, resolved_klass, selected_method(), true);
  }
  soroush_trace_runtime_dispatch("invokeinterface",
                                 recv_klass,
                                 resolved_method,
                                 selected_method,
                                 THREAD);
  // setup result
  if (resolved_method->has_vtable_index()) {
    int vtable_index = resolved_method->vtable_index();
    log_develop_trace(itables)("  -- vtable index: %d", vtable_index);
    assert(vtable_index == selected_method->vtable_index(), "sanity check");
    result.set_virtual(resolved_klass, resolved_method, selected_method, vtable_index, CHECK);
  } else if (resolved_method->has_itable_index()) {
    int itable_index = resolved_method()->itable_index();
    log_develop_trace(itables)("  -- itable index: %d", itable_index);
    result.set_interface(resolved_klass, resolved_method, selected_method, itable_index, CHECK);
  } else {
    int index = resolved_method->vtable_index();
    log_develop_trace(itables)("  -- non itable/vtable index: %d", index);
    assert(index == Method::nonvirtual_vtable_index, "Oops hit another case!");
    assert(resolved_method()->is_private() ||
           (resolved_method()->is_final() && resolved_method->method_holder() == vmClasses::Object_klass()),
           "Should only have non-virtual invokeinterface for private or final-Object methods!");
    assert(resolved_method()->can_be_statically_bound(), "Should only have non-virtual invokeinterface for statically bound methods!");
    // This sets up the nonvirtual form of "virtual" call (as needed for final and private methods)
    result.set_virtual(resolved_klass, resolved_method, resolved_method, index, CHECK);
  }
  JFR_ONLY(Jfr::on_resolution(result, CHECK);)
}


Method* LinkResolver::linktime_resolve_interface_method_or_null(
                                                 const LinkInfo& link_info) {
  EXCEPTION_MARK;
  Method* method_result = linktime_resolve_interface_method(link_info, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    CLEAR_PENDING_EXCEPTION;
    return nullptr;
  } else {
    return method_result;
  }
}

Method* LinkResolver::linktime_resolve_virtual_method_or_null(
                                                 const LinkInfo& link_info) {
  EXCEPTION_MARK;
  Method* method_result = linktime_resolve_virtual_method(link_info, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    CLEAR_PENDING_EXCEPTION;
    return nullptr;
  } else {
    return method_result;
  }
}

Method* LinkResolver::resolve_virtual_call_or_null(
                                                 Klass* receiver_klass,
                                                 const LinkInfo& link_info) {
  EXCEPTION_MARK;
  CallInfo info;
  resolve_virtual_call(info, Handle(), receiver_klass, link_info, false, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    CLEAR_PENDING_EXCEPTION;
    return nullptr;
  }
  return info.selected_method();
}

Method* LinkResolver::resolve_interface_call_or_null(
                                                 Klass* receiver_klass,
                                                 const LinkInfo& link_info) {
  EXCEPTION_MARK;
  CallInfo info;
  resolve_interface_call(info, Handle(), receiver_klass, link_info, false, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    CLEAR_PENDING_EXCEPTION;
    return nullptr;
  }
  return info.selected_method();
}

int LinkResolver::resolve_virtual_vtable_index(Klass* receiver_klass,
                                               const LinkInfo& link_info) {
  EXCEPTION_MARK;
  CallInfo info;
  resolve_virtual_call(info, Handle(), receiver_klass, link_info,
                       /*check_null_or_abstract*/false, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    CLEAR_PENDING_EXCEPTION;
    return Method::invalid_vtable_index;
  }
  return info.vtable_index();
}

Method* LinkResolver::resolve_static_call_or_null(const LinkInfo& link_info) {
  EXCEPTION_MARK;
  CallInfo info;
  resolve_static_call(info, link_info, /*initialize_class*/false, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    CLEAR_PENDING_EXCEPTION;
    return nullptr;
  }
  return info.selected_method();
}

Method* LinkResolver::resolve_special_call_or_null(const LinkInfo& link_info) {
  EXCEPTION_MARK;
  CallInfo info;
  resolve_special_call(info, Handle(), link_info, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    CLEAR_PENDING_EXCEPTION;
    return nullptr;
  }
  return info.selected_method();
}



//------------------------------------------------------------------------------------------------------------------------
// ConstantPool entries

void LinkResolver::resolve_invoke(CallInfo& result, Handle recv, const constantPoolHandle& pool, int index, Bytecodes::Code byte, TRAPS) {
  switch (byte) {
    case Bytecodes::_invokestatic   : resolve_invokestatic   (result,       pool, index, CHECK); break;
    case Bytecodes::_invokespecial  : resolve_invokespecial  (result, recv, pool, index, CHECK); break;
    case Bytecodes::_invokevirtual  : resolve_invokevirtual  (result, recv, pool, index, CHECK); break;
    case Bytecodes::_invokehandle   : resolve_invokehandle   (result,       pool, index, CHECK); break;
    case Bytecodes::_invokedynamic  : resolve_invokedynamic  (result,       pool, index, CHECK); break;
    case Bytecodes::_invokeinterface: resolve_invokeinterface(result, recv, pool, index, CHECK); break;
    default                         :                                                            break;
  }
  return;
}

void LinkResolver::resolve_invoke(CallInfo& result, Handle& recv,
                             const methodHandle& attached_method,
                             Bytecodes::Code byte, TRAPS) {
  Klass* defc = attached_method->method_holder();
  Symbol* name = attached_method->name();
  Symbol* type = attached_method->signature();
  LinkInfo link_info(defc, name, type);
  switch(byte) {
    case Bytecodes::_invokevirtual:
      resolve_virtual_call(result, recv, recv->klass(), link_info,
                           /*check_null_and_abstract=*/true, CHECK);
      break;
    case Bytecodes::_invokeinterface:
      resolve_interface_call(result, recv, recv->klass(), link_info,
                             /*check_null_and_abstract=*/true, CHECK);
      break;
    case Bytecodes::_invokestatic:
      resolve_static_call(result, link_info, /*initialize_class=*/false, CHECK);
      break;
    case Bytecodes::_invokespecial:
      resolve_special_call(result, recv, link_info, CHECK);
      break;
    default:
      fatal("bad call: %s", Bytecodes::name(byte));
      break;
  }
}

void LinkResolver::resolve_invokestatic(CallInfo& result, const constantPoolHandle& pool, int index, TRAPS) {
  LinkInfo link_info(pool, index, Bytecodes::_invokestatic, CHECK);
  resolve_static_call(result, link_info, /*initialize_class*/true, CHECK);
}


void LinkResolver::resolve_invokespecial(CallInfo& result, Handle recv,
                                         const constantPoolHandle& pool, int index, TRAPS) {
  LinkInfo link_info(pool, index, Bytecodes::_invokespecial, CHECK);
  resolve_special_call(result, recv, link_info, CHECK);
}


void LinkResolver::resolve_invokevirtual(CallInfo& result, Handle recv,
                                          const constantPoolHandle& pool, int index,
                                          TRAPS) {

  LinkInfo link_info(pool, index, Bytecodes::_invokevirtual, CHECK);
  Klass* recvrKlass = recv.is_null() ? (Klass*)nullptr : recv->klass();
  resolve_virtual_call(result, recv, recvrKlass, link_info, /*check_null_or_abstract*/true, CHECK);
}


void LinkResolver::resolve_invokeinterface(CallInfo& result, Handle recv, const constantPoolHandle& pool, int index, TRAPS) {
  LinkInfo link_info(pool, index, Bytecodes::_invokeinterface, CHECK);
  Klass* recvrKlass = recv.is_null() ? (Klass*)nullptr : recv->klass();
  resolve_interface_call(result, recv, recvrKlass, link_info, true, CHECK);
}

bool LinkResolver::resolve_previously_linked_invokehandle(CallInfo& result, const LinkInfo& link_info, const constantPoolHandle& pool, int index, TRAPS) {
  int cache_index = ConstantPool::decode_cpcache_index(index, true);
  ConstantPoolCacheEntry* cpce = pool->cache()->entry_at(cache_index);
  if (!cpce->is_f1_null()) {
    Klass* resolved_klass = link_info.resolved_klass();
    methodHandle method(THREAD, cpce->f1_as_method());
    Handle     appendix(THREAD, cpce->appendix_if_resolved(pool));
    result.set_handle(resolved_klass, method, appendix, CHECK_false);
    JFR_ONLY(Jfr::on_resolution(result, CHECK_false);)
    return true;
  } else {
    return false;
  }
}

void LinkResolver::resolve_invokehandle(CallInfo& result, const constantPoolHandle& pool, int index, TRAPS) {
  LinkInfo link_info(pool, index, Bytecodes::_invokehandle, CHECK);
  if (log_is_enabled(Info, methodhandles)) {
    ResourceMark rm(THREAD);
    log_info(methodhandles)("resolve_invokehandle %s %s", link_info.name()->as_C_string(),
                            link_info.signature()->as_C_string());
  }
  { // Check if the call site has been bound already, and short circuit:
    bool is_done = resolve_previously_linked_invokehandle(result, link_info, pool, index, CHECK);
    if (is_done) return;
  }
  resolve_handle_call(result, link_info, CHECK);
}

// ---------------------------------------------------------------------------
// Soroush: file-local helpers for MethodHandle runtime callsite capture.
// ---------------------------------------------------------------------------

// Count parameter slots from a JVM descriptor "(T1T2...)R".
// Excludes the receiver.  Returns -1 on malformed input.
// J/D = 2 slots; all other types = 1 slot.
static int sg_count_param_slots(Symbol* sig) {
  if (sig == nullptr) return -1;
  const char* s = sig->as_C_string();
  if (s == nullptr || s[0] != '(') return -1;
  s++; // skip '('
  int slots = 0;
  while (*s != ')' && *s != '\0') {
    switch (*s) {
    case 'J': case 'D':
      slots += 2; s++; break;
    case 'L':
      slots++;
      s++; // skip 'L'
      while (*s != ';' && *s != '\0') s++;
      if (*s == ';') s++;
      break;
    case '[':
      slots++; // any array = 1 slot regardless of dimensions
      while (*s == '[') s++;
      if (*s == 'L') {
        s++;
        while (*s != ';' && *s != '\0') s++;
        if (*s == ';') s++;
      } else if (*s != '\0' && *s != ')') {
        s++; // skip primitive component char
      }
      break;
    default:
      slots++; s++; break;
    }
  }
  return slots;
}

// Maps simple single-instruction push bytecodes to the number of stack slots
// they produce.  Returns 0 for anything that isn't a plain local-load or
// constant-push — signals a complex argument expression (invokestatic, getfield,
// etc.) that the backward-walk heuristic cannot handle.
static int sg_slots_pushed(Bytecodes::Code bc) {
  switch (bc) {
  // 1-slot pushes
  case Bytecodes::_aconst_null:
  case Bytecodes::_aload:   case Bytecodes::_aload_0: case Bytecodes::_aload_1:
  case Bytecodes::_aload_2: case Bytecodes::_aload_3:
  case Bytecodes::_iload:   case Bytecodes::_iload_0: case Bytecodes::_iload_1:
  case Bytecodes::_iload_2: case Bytecodes::_iload_3:
  case Bytecodes::_fload:   case Bytecodes::_fload_0: case Bytecodes::_fload_1:
  case Bytecodes::_fload_2: case Bytecodes::_fload_3:
  case Bytecodes::_iconst_m1: case Bytecodes::_iconst_0: case Bytecodes::_iconst_1:
  case Bytecodes::_iconst_2:  case Bytecodes::_iconst_3: case Bytecodes::_iconst_4:
  case Bytecodes::_iconst_5:
  case Bytecodes::_fconst_0: case Bytecodes::_fconst_1: case Bytecodes::_fconst_2:
  case Bytecodes::_bipush: case Bytecodes::_sipush:
  case Bytecodes::_ldc:    case Bytecodes::_ldc_w:
    return 1;
  // 2-slot pushes
  case Bytecodes::_lload:   case Bytecodes::_lload_0: case Bytecodes::_lload_1:
  case Bytecodes::_lload_2: case Bytecodes::_lload_3:
  case Bytecodes::_dload:   case Bytecodes::_dload_0: case Bytecodes::_dload_1:
  case Bytecodes::_dload_2: case Bytecodes::_dload_3:
  case Bytecodes::_lconst_0: case Bytecodes::_lconst_1:
  case Bytecodes::_dconst_0: case Bytecodes::_dconst_1:
  case Bytecodes::_ldc2_w:
    return 2;
  default:
    return 0; // complex: invokestatic, getfield, checkcast, etc.
  }
}

// Forward-scan method bytecodes to find which local variable holds the MH
// receiver for the invokehandle at invoke_bci.
// Strategy: collect all BCIs up to invoke_bci into a linear array, then walk
// backwards consuming arg_slots worth of simple single-instruction pushes.
// The instruction at pos after that walk is the MH-receiver push; we return
// its local-variable index, or -1 if the pattern is not a simple aload.
// Conservative by design: returns -1 rather than guessing.
static int sg_find_receiver_local_slot(Method* m, int invoke_bci, int arg_slots) {
  if (m == nullptr || invoke_bci <= 0 || arg_slots < 0 || invoke_bci > 1024)
    return -1;
  address code      = m->code_base();
  int     code_size = m->code_size();
  if (invoke_bci >= code_size) return -1;

  int bci_arr[1025];
  int count = 0;
  for (int bci = 0; bci < invoke_bci; ) {
    if (count >= 1025) return -1;
    bci_arr[count++] = bci;
    int bc_len = Bytecodes::length_at(m, code + bci);
    if (bc_len <= 0) return -1;
    bci += bc_len;
  }
  if (count == 0) return -1;

  int remaining = arg_slots;
  int pos       = count - 1;
  while (remaining > 0 && pos >= 0) {
    Bytecodes::Code bc = Bytecodes::java_code_at(m, code + bci_arr[pos]);
    int pushed = sg_slots_pushed(bc);
    if (pushed == 0) return -1; // non-simple arg expression
    remaining -= pushed;
    pos--;
  }
  if (remaining != 0 || pos < 0) return -1;

  // pos now indexes the instruction that pushed the MH receiver
  address recv_bcp = code + bci_arr[pos];
  switch (Bytecodes::java_code_at(m, recv_bcp)) {
  case Bytecodes::_aload_0: return 0;
  case Bytecodes::_aload_1: return 1;
  case Bytecodes::_aload_2: return 2;
  case Bytecodes::_aload_3: return 3;
  case Bytecodes::_aload:   return (int)(uint8_t)recv_bcp[1];
  default:                  return -1; // getfield, ldc of MH constant, etc.
  }
}

// ---------------------------------------------------------------------------
// sg_analyze_mh_receiver — compiler-grade symbolic bytecode stack simulator
//
// Replaces sg_find_receiver_local_slot.  Forward-scans method bytecodes from
// BCI 0 to invoke_bci maintaining a typed symbolic operand stack, then returns
// which local variable holds the MH receiver — or a precise diagnostic reason
// when the origin cannot be proven.
//
// Rationale: the old backward scanner returned -1 whenever any argument before
// the invoke was produced by a complex bytecode (invokestatic Integer.valueOf,
// getfield, etc.) because sg_slots_pushed returned 0.  The forward simulator
// correctly accounts for those instructions by reading their CP-cache descriptor
// and adjusting the depth, so the receiver slot identity is preserved.
//
// Reliability rules obeyed:
//   • Never guesses — returns DIAGNOSTIC instead of a wrong local index.
//   • Bails on backward branches, switches, multi-op fast forms, and unknown
//     bytecodes; each emits a distinct reason string.
//   • CP-cache lookup for descriptors: after class loading every invoke/field
//     bytecode operand is a CP-cache index (not a raw CP index).
// ---------------------------------------------------------------------------

// Count parameter slots from a C-string JVM descriptor.
// is_static=true: don't add 1 for an implicit receiver.
// Returns -1 on malformed input.
static int sg_cstr_param_slots(const char* desc, bool is_static) {
  if (!desc || desc[0] != '(') return -1;
  const char* p = desc + 1;
  int slots = is_static ? 0 : 1; // +1 for implicit receiver if not static
  while (*p && *p != ')') {
    switch (*p++) {
    case 'J': case 'D': slots += 2; break;
    case 'L':
      slots++;
      while (*p && *p != ';') p++;
      if (*p == ';') p++;
      break;
    case '[':
      slots++;
      while (*p == '[') p++;
      if (*p == 'L') { p++; while (*p && *p != ';') p++; if (*p == ';') p++; }
      else if (*p && *p != ')') p++;
      break;
    default:
      slots++;
      break;
    }
  }
  return slots;
}

// Return slot count for the return type of a C-string descriptor.
static int sg_cstr_return_slots(const char* desc) {
  if (!desc) return 1;
  const char* p = strchr(desc, ')');
  if (!p) return 1;
  p++;
  switch (*p) {
  case 'V': return 0;
  case 'J': case 'D': return 2;
  default: return 1;
  }
}

// Read the 2-byte CP-cache index operand from bcp+1.
// The Rewriter stores it in NATIVE byte order (Bytes::put_native_u2), so we
// must read it the same way.  Big-endian manual reads were giving garbage on
// aarch64 (little-endian), causing bounds-check failures in sg_invoke_desc_at.
static inline int sg_u2at(address bcp) {
  return (int)Bytes::get_native_u2(bcp + 1);
}

// Get method descriptor for an invoke bytecode via the CP-cache.
// After class loading the 2-byte operand of every invoke instruction is a
// CP-cache index; returns nullptr on any lookup failure.
static const char* sg_invoke_desc_at(ConstantPool* cp, address bcp) {
  if (!cp) return nullptr;
  ConstantPoolCache* cache = cp->cache();
  if (!cache) return nullptr;
  int cc_idx = sg_u2at(bcp);
  if (cc_idx < 0 || cc_idx >= cache->length()) return nullptr;
  int raw = cache->entry_at(cc_idx)->constant_pool_index();
  if (raw < 1 || raw >= cp->length()) return nullptr;
  Symbol* sig = cp->uncached_signature_ref_at(raw);
  return sig ? sig->as_C_string() : nullptr;
}

// Get field category (1 or 2 slots) for a getfield/getstatic/putfield/putstatic.
// For fast forms the type is encoded in the raw opcode; otherwise falls back to
// the CP-cache descriptor.
static int sg_field_cat_at(ConstantPool* cp, address bcp) {
  Bytecodes::Code raw_bc = (Bytecodes::Code)(uint8_t)bcp[0];
  if (raw_bc == Bytecodes::_fast_lgetfield || raw_bc == Bytecodes::_fast_dgetfield ||
      raw_bc == Bytecodes::_fast_lputfield || raw_bc == Bytecodes::_fast_dputfield)
    return 2;
  // Not a known cat-2 fast form — try CP-cache descriptor
  if (cp) {
    ConstantPoolCache* cache = cp->cache();
    if (cache) {
      int cc_idx = sg_u2at(bcp);
      if (cc_idx >= 0 && cc_idx < cache->length()) {
        int raw = cache->entry_at(cc_idx)->constant_pool_index();
        if (raw >= 1 && raw < cp->length()) {
          Symbol* sig = cp->uncached_signature_ref_at(raw);
          if (sig) {
            const char* s = sig->as_C_string();
            if (s && (s[0] == 'J' || s[0] == 'D')) return 2;
          }
        }
      }
    }
  }
  return 1;
}

#define SG_SIM_MAX_DEPTH 256

enum SgSvKind : uint8_t {
  SG_SV_UNKNOWN = 0,  // untracked / cannot be classified
  SG_SV_LOCAL,        // value from aload_N — local variable index known
  SG_SV_RESULT,       // result of invoke*/getfield/getstatic/new/constant
};

struct SgSlot { SgSvKind kind; int32_t local_n; };
static inline SgSlot sg_sl(SgSvKind k, int n = -1) {
  SgSlot s; s.kind = k; s.local_n = n; return s;
}

enum  SgRecvMode { SG_RECV_LOCAL, SG_RECV_DIAGNOSTIC };
struct SgRecvResult {
  SgRecvMode mode;
  int32_t    local_n;    // valid when mode == SG_RECV_LOCAL
  char       diag[192];  // valid when mode == SG_RECV_DIAGNOSTIC
};

static SgRecvResult sg_analyze_mh_receiver(Method* m, int invoke_bci, int arg_slots) {
  SgRecvResult bad; bad.mode = SG_RECV_DIAGNOSTIC; bad.local_n = -1; bad.diag[0] = '\0';

  if (!m || invoke_bci <= 0 || arg_slots < 0 || invoke_bci > 65535) {
    snprintf(bad.diag, sizeof(bad.diag), "invalid_args_to_analyzer"); return bad;
  }
  address code      = m->code_base();
  int     code_size = m->code_size();
  if (invoke_bci >= code_size) {
    snprintf(bad.diag, sizeof(bad.diag), "invoke_bci_out_of_range"); return bad;
  }
  ConstantPool* cp = m->constants();
  SgSlot stk[SG_SIM_MAX_DEPTH];
  int    sp = 0;

  for (int bci = 0; bci < invoke_bci; ) {
    address bcp = code + bci;

    // Detect multi-op fast forms whose java_code() is misleading
    // (_fast_iaccess_0 maps to _aload_0 but actually does aload_0+getfield).
    Bytecodes::Code raw_bc = (Bytecodes::Code)(uint8_t)bcp[0];
    if (raw_bc == Bytecodes::_fast_iaccess_0 ||
        raw_bc == Bytecodes::_fast_aaccess_0 ||
        raw_bc == Bytecodes::_fast_faccess_0 ||
        raw_bc == Bytecodes::_fast_iload2    ||
        raw_bc == Bytecodes::_fast_icaload) {
      snprintf(bad.diag, sizeof(bad.diag),
               "unsupported_fast_multiop_0x%02x_at_%d", (int)raw_bc, bci);
      return bad;
    }

    Bytecodes::Code bc     = Bytecodes::java_code_at(m, bcp);
    int             bc_len = Bytecodes::length_at(m, bcp);
    if (bc_len <= 0) {
      snprintf(bad.diag, sizeof(bad.diag), "zero_length_bc_at_%d", bci); return bad;
    }
    if (sp > SG_SIM_MAX_DEPTH - 4) {
      snprintf(bad.diag, sizeof(bad.diag), "stack_sim_overflow_at_%d", bci); return bad;
    }

    switch (bc) {
    // ---- constants: push 1 slot ----
    case Bytecodes::_aconst_null:
      stk[sp++] = sg_sl(SG_SV_RESULT); break; // null ref, not a useful MH
    case Bytecodes::_iconst_m1: case Bytecodes::_iconst_0: case Bytecodes::_iconst_1:
    case Bytecodes::_iconst_2:  case Bytecodes::_iconst_3: case Bytecodes::_iconst_4:
    case Bytecodes::_iconst_5:
    case Bytecodes::_fconst_0:  case Bytecodes::_fconst_1: case Bytecodes::_fconst_2:
    case Bytecodes::_bipush: case Bytecodes::_sipush:
    case Bytecodes::_ldc:    case Bytecodes::_ldc_w:
      stk[sp++] = sg_sl(SG_SV_RESULT); break; // treat ldc MH constant as opaque
    // ---- constants: push 2 slots ----
    case Bytecodes::_lconst_0: case Bytecodes::_lconst_1:
    case Bytecodes::_dconst_0: case Bytecodes::_dconst_1:
    case Bytecodes::_ldc2_w:
      stk[sp++] = sg_sl(SG_SV_RESULT); stk[sp++] = sg_sl(SG_SV_RESULT); break;
    // ---- aload: track local index ----
    case Bytecodes::_aload_0: stk[sp++] = sg_sl(SG_SV_LOCAL, 0); break;
    case Bytecodes::_aload_1: stk[sp++] = sg_sl(SG_SV_LOCAL, 1); break;
    case Bytecodes::_aload_2: stk[sp++] = sg_sl(SG_SV_LOCAL, 2); break;
    case Bytecodes::_aload_3: stk[sp++] = sg_sl(SG_SV_LOCAL, 3); break;
    case Bytecodes::_aload: {
      int n = (bc_len == 2) ? (int)(uint8_t)bcp[1] : (int)(uint8_t)bcp[2]; // wide
      stk[sp++] = sg_sl(SG_SV_LOCAL, n); break;
    }
    // ---- iload/fload: 1-slot non-ref ----
    case Bytecodes::_iload_0: case Bytecodes::_iload_1:
    case Bytecodes::_iload_2: case Bytecodes::_iload_3:
    case Bytecodes::_fload_0: case Bytecodes::_fload_1:
    case Bytecodes::_fload_2: case Bytecodes::_fload_3:
    case Bytecodes::_iload:   case Bytecodes::_fload:
      stk[sp++] = sg_sl(SG_SV_RESULT); break;
    // ---- lload/dload: 2-slot ----
    case Bytecodes::_lload_0: case Bytecodes::_lload_1:
    case Bytecodes::_lload_2: case Bytecodes::_lload_3:
    case Bytecodes::_dload_0: case Bytecodes::_dload_1:
    case Bytecodes::_dload_2: case Bytecodes::_dload_3:
    case Bytecodes::_lload:   case Bytecodes::_dload:
      stk[sp++] = sg_sl(SG_SV_RESULT); stk[sp++] = sg_sl(SG_SV_RESULT); break;
    // ---- stores: pop ----
    case Bytecodes::_astore_0: case Bytecodes::_astore_1:
    case Bytecodes::_astore_2: case Bytecodes::_astore_3:
    case Bytecodes::_astore:
    case Bytecodes::_istore_0: case Bytecodes::_istore_1:
    case Bytecodes::_istore_2: case Bytecodes::_istore_3:
    case Bytecodes::_istore:
    case Bytecodes::_fstore_0: case Bytecodes::_fstore_1:
    case Bytecodes::_fstore_2: case Bytecodes::_fstore_3:
    case Bytecodes::_fstore:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_store_%d",bci); return bad; }
      sp--; break;
    case Bytecodes::_lstore_0: case Bytecodes::_lstore_1:
    case Bytecodes::_lstore_2: case Bytecodes::_lstore_3:
    case Bytecodes::_lstore:
    case Bytecodes::_dstore_0: case Bytecodes::_dstore_1:
    case Bytecodes::_dstore_2: case Bytecodes::_dstore_3:
    case Bytecodes::_dstore:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_lstore_%d",bci); return bad; }
      sp -= 2; break;
    // ---- stack manipulation ----
    case Bytecodes::_pop:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_pop_%d",bci); return bad; }
      sp--; break;
    case Bytecodes::_pop2:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_pop2_%d",bci); return bad; }
      sp -= 2; break;
    case Bytecodes::_dup:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_dup_%d",bci); return bad; }
      stk[sp] = stk[sp-1]; sp++; break;
    case Bytecodes::_dup_x1:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_dup_x1_%d",bci); return bad; }
      { SgSlot t = stk[sp-1];
        stk[sp] = stk[sp-1]; stk[sp-1] = stk[sp-2]; stk[sp-2] = t; sp++; } break;
    case Bytecodes::_dup_x2:
      if (sp < 3) { snprintf(bad.diag,sizeof(bad.diag),"underflow_dup_x2_%d",bci); return bad; }
      { SgSlot t = stk[sp-1];
        stk[sp] = t; stk[sp-1] = stk[sp-2]; stk[sp-2] = stk[sp-3]; stk[sp-3] = t; sp++; } break;
    case Bytecodes::_dup2:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_dup2_%d",bci); return bad; }
      stk[sp] = stk[sp-2]; stk[sp+1] = stk[sp-1]; sp += 2; break;
    case Bytecodes::_dup2_x1: case Bytecodes::_dup2_x2:
      snprintf(bad.diag,sizeof(bad.diag),"unsupported_dup2x_at_%d",bci); return bad;
    case Bytecodes::_swap:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_swap_%d",bci); return bad; }
      { SgSlot t = stk[sp-1]; stk[sp-1] = stk[sp-2]; stk[sp-2] = t; } break;
    // ---- checkcast: type change only, preserve symbolic value ----
    case Bytecodes::_checkcast: break;
    // ---- instanceof: pop ref, push int ----
    case Bytecodes::_instanceof:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_instanceof_%d",bci); return bad; }
      stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- allocation ----
    case Bytecodes::_new:
      stk[sp++] = sg_sl(SG_SV_RESULT); break;
    case Bytecodes::_newarray: case Bytecodes::_anewarray:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_newarray_%d",bci); return bad; }
      stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    case Bytecodes::_multianewarray: {
      int dims = (int)(uint8_t)bcp[3];
      if (sp < dims) { snprintf(bad.diag,sizeof(bad.diag),"underflow_multianewarray_%d",bci); return bad; }
      sp -= dims; stk[sp++] = sg_sl(SG_SV_RESULT); break;
    }
    case Bytecodes::_arraylength:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_arraylength_%d",bci); return bad; }
      stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- array loads ----
    case Bytecodes::_aaload: case Bytecodes::_iaload: case Bytecodes::_faload:
    case Bytecodes::_baload: case Bytecodes::_caload: case Bytecodes::_saload:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_xaload_%d",bci); return bad; }
      sp--; stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    case Bytecodes::_laload: case Bytecodes::_daload:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_laload_%d",bci); return bad; }
      stk[sp-2] = sg_sl(SG_SV_RESULT); stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- array stores ----
    case Bytecodes::_aastore: case Bytecodes::_iastore: case Bytecodes::_fastore:
    case Bytecodes::_bastore: case Bytecodes::_castore: case Bytecodes::_sastore:
      if (sp < 3) { snprintf(bad.diag,sizeof(bad.diag),"underflow_xastore_%d",bci); return bad; }
      sp -= 3; break;
    case Bytecodes::_lastore: case Bytecodes::_dastore:
      if (sp < 4) { snprintf(bad.diag,sizeof(bad.diag),"underflow_lastore_%d",bci); return bad; }
      sp -= 4; break;
    // ---- int/float binary arithmetic ----
    case Bytecodes::_iadd: case Bytecodes::_isub: case Bytecodes::_imul: case Bytecodes::_idiv:
    case Bytecodes::_irem: case Bytecodes::_iand: case Bytecodes::_ior:  case Bytecodes::_ixor:
    case Bytecodes::_ishl: case Bytecodes::_ishr: case Bytecodes::_iushr:
    case Bytecodes::_fadd: case Bytecodes::_fsub: case Bytecodes::_fmul:
    case Bytecodes::_fdiv: case Bytecodes::_frem:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_ibinop_%d",bci); return bad; }
      sp--; stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- long/double binary arithmetic ----
    case Bytecodes::_ladd: case Bytecodes::_lsub: case Bytecodes::_lmul: case Bytecodes::_ldiv:
    case Bytecodes::_lrem: case Bytecodes::_land: case Bytecodes::_lor:  case Bytecodes::_lxor:
    case Bytecodes::_dadd: case Bytecodes::_dsub: case Bytecodes::_dmul:
    case Bytecodes::_ddiv: case Bytecodes::_drem:
      if (sp < 4) { snprintf(bad.diag,sizeof(bad.diag),"underflow_lbinop_%d",bci); return bad; }
      sp -= 2; stk[sp-2] = sg_sl(SG_SV_RESULT); stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- lshl/lshr/lushr: long(2) + int(1) → long(2), net -1 ----
    case Bytecodes::_lshl: case Bytecodes::_lshr: case Bytecodes::_lushr:
      if (sp < 3) { snprintf(bad.diag,sizeof(bad.diag),"underflow_lshift_%d",bci); return bad; }
      sp--; stk[sp-2] = sg_sl(SG_SV_RESULT); stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- unary arithmetic ----
    case Bytecodes::_ineg: case Bytecodes::_fneg:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_ineg_%d",bci); return bad; }
      stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    case Bytecodes::_lneg: case Bytecodes::_dneg:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_lneg_%d",bci); return bad; }
      stk[sp-2] = sg_sl(SG_SV_RESULT); stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    case Bytecodes::_iinc: break; // local-local, no stack change
    // ---- conversions (1→1) ----
    case Bytecodes::_i2b: case Bytecodes::_i2c: case Bytecodes::_i2s:
    case Bytecodes::_i2f: case Bytecodes::_f2i:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_conv1_%d",bci); return bad; }
      stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- conversions (1→2): widen ----
    case Bytecodes::_i2l: case Bytecodes::_i2d: case Bytecodes::_f2l: case Bytecodes::_f2d:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_widen_%d",bci); return bad; }
      stk[sp-1] = sg_sl(SG_SV_RESULT); stk[sp++] = sg_sl(SG_SV_RESULT); break;
    // ---- conversions (2→1): narrow ----
    case Bytecodes::_l2i: case Bytecodes::_l2f: case Bytecodes::_d2i: case Bytecodes::_d2f:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_narrow_%d",bci); return bad; }
      sp--; stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- conversions (2→2) ----
    case Bytecodes::_l2d: case Bytecodes::_d2l:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_l2d_%d",bci); return bad; }
      stk[sp-2] = sg_sl(SG_SV_RESULT); stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- comparisons ----
    case Bytecodes::_lcmp:
      if (sp < 4) { snprintf(bad.diag,sizeof(bad.diag),"underflow_lcmp_%d",bci); return bad; }
      sp -= 3; stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    case Bytecodes::_fcmpl: case Bytecodes::_fcmpg:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_fcmp_%d",bci); return bad; }
      sp--; stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    case Bytecodes::_dcmpl: case Bytecodes::_dcmpg:
      if (sp < 4) { snprintf(bad.diag,sizeof(bad.diag),"underflow_dcmp_%d",bci); return bad; }
      sp -= 3; stk[sp-1] = sg_sl(SG_SV_RESULT); break;
    // ---- conditional branches: pop condition, continue linear scan ----
    case Bytecodes::_ifeq: case Bytecodes::_ifne: case Bytecodes::_iflt:
    case Bytecodes::_ifge: case Bytecodes::_ifgt: case Bytecodes::_ifle:
    case Bytecodes::_ifnull: case Bytecodes::_ifnonnull:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_if_%d",bci); return bad; }
      sp--; break;
    case Bytecodes::_if_icmpeq: case Bytecodes::_if_icmpne: case Bytecodes::_if_icmplt:
    case Bytecodes::_if_icmpge: case Bytecodes::_if_icmpgt: case Bytecodes::_if_icmple:
    case Bytecodes::_if_acmpeq: case Bytecodes::_if_acmpne:
      if (sp < 2) { snprintf(bad.diag,sizeof(bad.diag),"underflow_ifcmp_%d",bci); return bad; }
      sp -= 2; break;
    // ---- goto: follow forward jumps; bail on backward (loop) ----
    case Bytecodes::_goto: {
      int off = (int)(int16_t)(((uint16_t)(uint8_t)bcp[1] << 8) | (uint8_t)bcp[2]);
      int tgt = bci + off;
      if (tgt <= bci) {
        snprintf(bad.diag,sizeof(bad.diag),"backward_goto_at_%d_target_%d",bci,tgt);
        return bad;
      }
      bci = tgt; continue;
    }
    case Bytecodes::_goto_w: {
      int off = (int)((uint32_t)(uint8_t)bcp[1]<<24 | (uint32_t)(uint8_t)bcp[2]<<16
                    | (uint32_t)(uint8_t)bcp[3]<<8  | (uint8_t)bcp[4]);
      int tgt = bci + off;
      if (tgt <= bci) {
        snprintf(bad.diag,sizeof(bad.diag),"backward_goto_w_at_%d_target_%d",bci,tgt);
        return bad;
      }
      bci = tgt; continue;
    }
    // ---- switch: complex control flow ----
    case Bytecodes::_tableswitch:  case Bytecodes::_lookupswitch:
    case Bytecodes::_fast_linearswitch: case Bytecodes::_fast_binaryswitch:
      snprintf(bad.diag,sizeof(bad.diag),"switch_at_%d",bci); return bad;
    // ---- nop ----
    case Bytecodes::_nop: break;
    // ---- athrow ----
    case Bytecodes::_athrow:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_athrow_%d",bci); return bad; }
      sp--; break;
    // ---- monitor ----
    case Bytecodes::_monitorenter: case Bytecodes::_monitorexit:
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_monitor_%d",bci); return bad; }
      sp--; break;
    // ---- getstatic: push field slots ----
    case Bytecodes::_getstatic: {
      int cat = sg_field_cat_at(cp, bcp);
      stk[sp++] = sg_sl(SG_SV_RESULT);
      if (cat == 2) stk[sp++] = sg_sl(SG_SV_RESULT);
      break;
    }
    // ---- getfield: pop ref, push field slots ----
    case Bytecodes::_getfield: {
      if (sp < 1) { snprintf(bad.diag,sizeof(bad.diag),"underflow_getfield_%d",bci); return bad; }
      int cat = sg_field_cat_at(cp, bcp);
      sp--;
      stk[sp++] = sg_sl(SG_SV_RESULT);
      if (cat == 2) stk[sp++] = sg_sl(SG_SV_RESULT);
      break;
    }
    // ---- putstatic: pop field slots ----
    case Bytecodes::_putstatic: {
      int cat = sg_field_cat_at(cp, bcp);
      if (sp < cat) { snprintf(bad.diag,sizeof(bad.diag),"underflow_putstatic_%d",bci); return bad; }
      sp -= cat; break;
    }
    // ---- putfield: pop ref + field slots ----
    case Bytecodes::_putfield: {
      int cat = sg_field_cat_at(cp, bcp);
      int n = 1 + cat;
      if (sp < n) { snprintf(bad.diag,sizeof(bad.diag),"underflow_putfield_%d",bci); return bad; }
      sp -= n; break;
    }
    // ---- invokestatic: descriptor via CP-cache; no implicit receiver ----
    case Bytecodes::_invokestatic: {
      const char* desc = sg_invoke_desc_at(cp, bcp);
      if (!desc) { snprintf(bad.diag,sizeof(bad.diag),"no_desc_invokestatic_%d",bci); return bad; }
      int ps = sg_cstr_param_slots(desc, true);
      int rs = sg_cstr_return_slots(desc);
      if (ps < 0) { snprintf(bad.diag,sizeof(bad.diag),"bad_desc_invokestatic_%d",bci); return bad; }
      if (sp < ps) { snprintf(bad.diag,sizeof(bad.diag),"underflow_invokestatic_%d",bci); return bad; }
      sp -= ps;
      for (int i=0; i<rs; i++) stk[sp++] = sg_sl(SG_SV_RESULT);
      break;
    }
    // ---- instance invokes (virtual/interface/special/handle): include receiver ----
    case Bytecodes::_invokevirtual:
    case Bytecodes::_invokeinterface:
    case Bytecodes::_invokespecial:
    case Bytecodes::_invokehandle: {
      const char* desc = sg_invoke_desc_at(cp, bcp);
      if (!desc) { snprintf(bad.diag,sizeof(bad.diag),"no_desc_invoke_%d",bci); return bad; }
      int ps = sg_cstr_param_slots(desc, false); // false = include receiver slot
      int rs = sg_cstr_return_slots(desc);
      if (ps < 0) { snprintf(bad.diag,sizeof(bad.diag),"bad_desc_invoke_%d",bci); return bad; }
      if (sp < ps) { snprintf(bad.diag,sizeof(bad.diag),"underflow_invoke_%d",bci); return bad; }
      sp -= ps;
      for (int i=0; i<rs; i++) stk[sp++] = sg_sl(SG_SV_RESULT);
      break;
    }
    // ---- invokedynamic: 4-byte native-endian operand encodes indy index ----
    // The Rewriter stores ~indy_index (negative) via put_native_u4.
    // Use resolved_indy_entry_at to retrieve the CP index and descriptor.
    case Bytecodes::_invokedynamic: {
      if (!cp || !cp->cache()) {
        snprintf(bad.diag,sizeof(bad.diag),"no_cp_for_indy_%d",bci); return bad;
      }
      int raw_u4 = (int)Bytes::get_native_u4(bcp + 1);
      if (!ConstantPool::is_invokedynamic_index(raw_u4)) {
        snprintf(bad.diag,sizeof(bad.diag),"invalid_indy_index_%d",bci); return bad;
      }
      int indy_idx = ConstantPool::decode_invokedynamic_index(raw_u4);
      if (indy_idx < 0 || indy_idx >= cp->cache()->resolved_indy_entries_length()) {
        snprintf(bad.diag,sizeof(bad.diag),"indy_idx_oob_%d",bci); return bad;
      }
      ResolvedIndyEntry* indy_entry = cp->cache()->resolved_indy_entry_at(indy_idx);
      int raw_cp = (int)indy_entry->constant_pool_index();
      if (raw_cp < 1 || raw_cp >= cp->length()) {
        snprintf(bad.diag,sizeof(bad.diag),"indy_cp_oob_%d",bci); return bad;
      }
      Symbol* sig = cp->uncached_signature_ref_at(raw_cp);
      if (!sig) { snprintf(bad.diag,sizeof(bad.diag),"no_sig_indy_%d",bci); return bad; }
      const char* desc = sig->as_C_string();
      int ps = sg_cstr_param_slots(desc, true); // no receiver for invokedynamic
      int rs = sg_cstr_return_slots(desc);
      if (ps < 0) { snprintf(bad.diag,sizeof(bad.diag),"bad_desc_indy_%d",bci); return bad; }
      if (sp < ps) { snprintf(bad.diag,sizeof(bad.diag),"underflow_indy_%d",bci); return bad; }
      sp -= ps;
      for (int i=0; i<rs; i++) stk[sp++] = sg_sl(SG_SV_RESULT);
      break;
    }
    // ---- return (should not appear before invoke_bci in valid code) ----
    case Bytecodes::_return:  case Bytecodes::_ireturn: case Bytecodes::_lreturn:
    case Bytecodes::_freturn: case Bytecodes::_dreturn: case Bytecodes::_areturn:
      snprintf(bad.diag,sizeof(bad.diag),"return_before_invoke_%d",bci); return bad;
    // ---- jsr/ret: obsolete ----
    case Bytecodes::_jsr: case Bytecodes::_jsr_w: case Bytecodes::_ret:
      snprintf(bad.diag,sizeof(bad.diag),"jsr_unsupported_%d",bci); return bad;
    default:
      snprintf(bad.diag,sizeof(bad.diag),
               "unknown_bc_0x%02x_at_%d", (int)(uint8_t)bcp[0], bci);
      return bad;
    }
    bci += bc_len;
  }

  // At invoke_bci the operand stack layout is:
  //   [receiver] [arg_0] [arg_1] ... [arg_{n-1}]   (arg_slots total for args)
  // Receiver is at slot index (sp - 1 - arg_slots).
  int recv_idx = sp - 1 - arg_slots;
  if (recv_idx < 0 || recv_idx >= sp) {
    snprintf(bad.diag,sizeof(bad.diag),
             "recv_slot_oob sp=%d arg_slots=%d", sp, arg_slots);
    return bad;
  }
  SgSlot recv = stk[recv_idx];
  if (recv.kind == SG_SV_LOCAL && recv.local_n >= 0) {
    SgRecvResult ok;
    ok.mode    = SG_RECV_LOCAL;
    ok.local_n = recv.local_n;
    ok.diag[0] = '\0';
    return ok;
  }
  if (recv.kind == SG_SV_RESULT) {
    snprintf(bad.diag,sizeof(bad.diag),"recv_from_method_result_or_field");
    return bad;
  }
  snprintf(bad.diag,sizeof(bad.diag),"recv_unknown_kind_%d", (int)recv.kind);
  return bad;
}

// ---------------------------------------------------------------------------
// sg_emit_sibling_bcis — sibling-BCI silent-omission eliminator.
//
// When resolve_handle_call fires for BCI X (the first invocation of a CP cache
// entry), multiple other BCIs in the same method may share that CP cache entry
// and thus NEVER trigger resolve_handle_call themselves.  This function finds
// all such sibling BCIs and emits a diagnostic for each, so that no executed
// MethodHandle callsite is silently omitted.
//
// For each sibling BCI, we run the stack simulator to determine whether the
// MH receiver is:
//   (a) the same local slot as at the primary BCI → diagnostic "sibling_bci_same_local"
//   (b) a different local slot                    → diagnostic "sibling_bci_distinct_receiver"
//   (c) analysis failed                           → diagnostic "sibling_bci_unanalyzable"
//
// Hard rule: the diagnostic is always emitted at resolve time.  If a later
// LF-frame attribution fire produces an exact record for the same (class, method,
// bci), the export dedup phase keeps only the exact record (Part C).
// ---------------------------------------------------------------------------
static void sg_emit_sibling_bcis(
    const char*  cat,
    Method*      user_method,   // the attributed user method
    int          primary_bci,   // the BCI that triggered resolve_handle_call
    int          primary_recv_local,  // local slot at primary_bci (-1 = unknown)
    int          src_cp,        // CP constant pool index of the shared CP entry
    const char*  src_class,     uint64_t src_loader,
    const char*  src_method_name, const char* src_desc,
    int          arg_slots)     // parameter slots in the MH type (for simulator)
{
  if (user_method == nullptr || src_cp < 0) return;
  ConstantPool* cpool = user_method->constants();
  if (cpool == nullptr || cpool->cache() == nullptr) return;

  int  code_size = user_method->code_size();
  bool any_sibling = false;

  for (int scan_bci = 0; scan_bci < code_size; ) {
    address scan_bcp = user_method->bcp_from(scan_bci);
    Bytecodes::Code bc = Bytecodes::code_at(user_method, scan_bcp);

    // Only care about invoke* bytecodes (after rewriting may be _invokehandle)
    bool is_inv = (bc == Bytecodes::_invokevirtual  ||
                   bc == Bytecodes::_invokehandle   ||
                   bc == Bytecodes::_invokespecial  ||
                   bc == Bytecodes::_invokestatic   ||
                   bc == Bytecodes::_invokeinterface);
    if (is_inv && scan_bci != primary_bci) {
      int scan_cp = -1;
      if (scan_bci + 2 < code_size) {
        int cc_idx = sg_u2at(scan_bcp + 1);
        if (cc_idx >= 0 && cc_idx < cpool->cache()->length()) {
          scan_cp = cpool->cache()->entry_at(cc_idx)->constant_pool_index();
        }
      }
      if (scan_cp == src_cp) {
        // This BCI shares the same CP constant pool index as primary_bci.
        // The CP cache entry is already resolved (we're IN resolve_handle_call
        // for it), so this sibling will never trigger resolve_handle_call.
        char reason[256];
        if (arg_slots >= 0) {
          SgRecvResult sr = sg_analyze_mh_receiver(user_method, scan_bci, arg_slots);
          if (sr.mode == SG_RECV_LOCAL) {
            if (primary_recv_local >= 0 && sr.local_n == primary_recv_local) {
              snprintf(reason, sizeof(reason),
                       "sibling_bci_same_local local=%d cp_index=%d",
                       sr.local_n, src_cp);
            } else {
              snprintf(reason, sizeof(reason),
                       "sibling_bci_distinct_receiver local=%d primary_local=%d cp_index=%d",
                       sr.local_n, primary_recv_local, src_cp);
            }
          } else {
            snprintf(reason, sizeof(reason),
                     "sibling_bci_unanalyzable diag=%s cp_index=%d",
                     sr.diag[0] ? sr.diag : "?", src_cp);
          }
        } else {
          snprintf(reason, sizeof(reason),
                   "sibling_bci_arg_slots_unknown cp_index=%d", src_cp);
        }
        int scan_opcode = (int)(uint8_t)bc;
        soroush_graph_generic_callsite(cat,
            src_class, src_loader, src_method_name, src_desc,
            scan_bci, scan_opcode, scan_cp,
            nullptr, 0, nullptr, nullptr,
            /*src_ok=*/true, /*tgt_ok=*/false, reason);
        any_sibling = true;
      }
    }

    int bc_len = Bytecodes::length_at(user_method, scan_bcp);
    if (bc_len <= 0) break;
    scan_bci += bc_len;
  }
  (void)any_sibling;
}

// ---------------------------------------------------------------------------
// MethodHandle adapter-structure walking (runtime receiver inspection v2).
//
// Taxonomy (JDK 21 field layout, audited from source):
//
//   DirectMethodHandle (+ Special/Interface/Constructor/Accessor subclasses)
//     → member (MemberName) → single exact target
//
//   DelegatingMethodHandle subclasses with `target: MethodHandle` field:
//     CountingWrapper, IntrinsicMethodHandle, WrappedMember, AsVarargsCollector
//     → transparent wrappers, recurse through `target`
//
//   BoundMethodHandle$Species_LLL  + LambdaForm.kind == "GUARD"
//     → guardWithTest: argL0=test, argL1=target(maybe wrapped), argL2=fallback
//     → callsite_target_set with roles test / true_target / false_target
//
//   BoundMethodHandle$Species_LLLLL + LambdaForm.kind == "GUARD_WITH_CATCH"
//     → catchException: argL0=try_target, argL1=exception Class, argL2=handler
//     → callsite_target_set with roles try_target / handler (aux_info=exception_class)
//
//   All other BoundMethodHandle shapes (asType/filter/fold/permute/etc.)
//     → LambdaForm.kind == "GENERIC" (cannot distinguish roles)
//     → diagnostic unsupported_adapter_shape
//
// NOTE: Kind.CONVERT is defined in the Java source but never assigned in
// JDK 21 — asType and all pairwise-convert adapters use Kind.GENERIC.
// ---------------------------------------------------------------------------

struct SgMhTarget {
  const char* klass;
  uint64_t    loader_id;
  const char* method;
  const char* descriptor;
  bool        valid;
};

// SgAdapterNode: one node in an extracted GENERIC-BMH adapter graph.
struct SgAdapterNode {
  int         id;
  const char* role;               // "adapted_target", "primary_target", "nested_argL0", …
  const char* from_desc;          // outer adapter type descriptor (type_conversion only)
  const char* to_desc;            // inner component's own type descriptor
  const char* node_adapter_class; // non-null for non-exact BMH nodes: species class name
  const char* node_classification;// "user_target","helper_adapter","helper_invoker","helper_boxing",
                                  // "helper_reflection","internal_jdk","bound_data","unknown"
  const char* exact_false_reason; // null when exact=true; reason string otherwise
  SgMhTarget  target;             // valid when has_target == true
  bool        has_target;
};

// SgAdapterEdge: directed edge "parent contains child" in the nested BMH graph.
struct SgAdapterEdge {
  int         from_id;
  int         to_id;
  const char* kind;  // "contains"
};

struct SgMhWalkResult {
  enum Shape {
    DIRECT,         // DirectMethodHandle  → single exact target
    GWT,            // guardWithTest       → test / true_target / false_target
    GWC,            // catchException      → try_target / handler
    ADAPTER_GRAPH,  // GENERIC/named BMH   → adapter graph extracted
    BMH_UNKNOWN,    // BMH with unextractable form
    MH_UNKNOWN      // not a known MH class
  };

  Shape       shape;
  const char* adapter_class; // outermost MH class name
  const char* lf_kind;       // LambdaForm.Kind.name() (ResourceMark-allocated)
  const char* aux_info;      // GWC: exception class name

  // DIRECT / GWT / GWC fields:
  int         n_targets;
  const char* roles[4];
  SgMhTarget  targets[4];
  bool        all_exact;

  // ADAPTER_GRAPH fields:
  int           n_graph_nodes;
  SgAdapterNode graph_nodes[16];
  int           n_graph_edges;
  SgAdapterEdge graph_edges[16];
  const char*   graph_kind;      // "type_conversion", "dual_target", "multi_target", …
  const char*   outer_desc;      // MH type descriptor as presented to the caller
  bool          graph_all_exact;
  char          graph_diag[192]; // non-empty when extraction partially or fully failed
};

static bool sg_oop_valid(oop o) {
  return o != nullptr && oopDesc::is_oop_or_null(o);
}

// Scan the class hierarchy of `obj` for an instance field named `fname` with
// descriptor `fsig` and return its value.  Returns nullptr if not found.
static oop sg_find_field_oop(oop obj, const char* fname, const char* fsig) {
  if (!sg_oop_valid(obj)) return nullptr;
  Klass* k = obj->klass();
  while (k != nullptr && k->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(k);
    for (JavaFieldStream jfs(ik); !jfs.done(); jfs.next()) {
      Symbol* fn = jfs.name();
      Symbol* fs = jfs.signature();
      if (fn == nullptr || fs == nullptr) continue;
      if (strcmp(fn->as_C_string(), fname) == 0 &&
          strcmp(fs->as_C_string(), fsig)  == 0) {
        return obj->obj_field(jfs.field_descriptor().offset());
      }
    }
    k = k->super();
  }
  return nullptr;
}

// Read LambdaForm.kind.name() for a MethodHandle.
// Returns a ResourceMark-allocated C string, or nullptr on failure.
static const char* sg_lf_kind_name(oop mh_oop) {
  if (!sg_oop_valid(mh_oop) || !java_lang_invoke_MethodHandle::is_instance(mh_oop))
    return nullptr;
  oop lf = java_lang_invoke_MethodHandle::form(mh_oop);
  if (!sg_oop_valid(lf)) return nullptr;

  oop kind_oop = sg_find_field_oop(lf, "kind", "Ljava/lang/invoke/LambdaForm$Kind;");
  if (!sg_oop_valid(kind_oop)) return nullptr;

  // Read java.lang.Enum.name (String field in the enum supertype)
  Klass* k = kind_oop->klass();
  while (k != nullptr && k->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(k);
    for (JavaFieldStream jfs(ik); !jfs.done(); jfs.next()) {
      Symbol* fn = jfs.name();
      Symbol* fs = jfs.signature();
      if (fn == nullptr || fs == nullptr) continue;
      if (strcmp(fn->as_C_string(), "name") == 0 &&
          strcmp(fs->as_C_string(), "Ljava/lang/String;") == 0) {
        oop str = kind_oop->obj_field(jfs.field_descriptor().offset());
        if (str != nullptr && java_lang_String::is_instance(str))
          return java_lang_String::as_utf8_string(str);
        return nullptr;
      }
    }
    k = k->super();
  }
  return nullptr;
}

// Read argL{index} from a BoundMethodHandle species oop.
static oop sg_bmh_arg_at(oop mh_oop, int index) {
  if (!sg_oop_valid(mh_oop) || index < 0 || index > 7) return nullptr;
  char fname[12];
  snprintf(fname, sizeof(fname), "argL%d", index);
  Klass* k = mh_oop->klass();
  while (k != nullptr && k->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(k);
    for (JavaFieldStream jfs(ik); !jfs.done(); jfs.next()) {
      Symbol* fn = jfs.name();
      if (fn != nullptr && strcmp(fn->as_C_string(), fname) == 0)
        return mh_oop->obj_field(jfs.field_descriptor().offset());
    }
    k = k->super();
  }
  return nullptr;
}

// Fill an SgMhTarget from a DirectMethodHandle.  Returns true on success.
static bool sg_extract_dmh_target(oop mh_oop, SgMhTarget* out) {
  if (!sg_oop_valid(mh_oop) || out == nullptr) return false;
  if (!java_lang_invoke_DirectMethodHandle::is_instance(mh_oop)) return false;
  oop mn = java_lang_invoke_DirectMethodHandle::member(mh_oop);
  if (!sg_oop_valid(mn) || !java_lang_invoke_MemberName::is_instance(mn)) return false;
  Method* tm = (Method*)java_lang_invoke_MemberName::vmtarget(mn);
  if (tm == nullptr) return false;
  out->klass      = tm->method_holder()->name()->as_C_string();
  out->loader_id  = (uint64_t)(uintptr_t)tm->method_holder()->class_loader_data();
  out->method     = tm->name()->as_C_string();
  out->descriptor = tm->signature()->as_C_string();
  out->valid      = true;
  return true;
}

// Unwrap transparent DelegatingMethodHandle wrappers (CountingWrapper,
// IntrinsicMethodHandle, WrappedMember, AsVarargsCollector …) to reach the
// payload.  Returns the original oop if it is a DMH or has no `target` field.
static oop sg_unwrap_delegating(oop mh_oop, int depth) {
  if (!sg_oop_valid(mh_oop) || depth <= 0) return mh_oop;
  if (java_lang_invoke_DirectMethodHandle::is_instance(mh_oop)) return mh_oop;
  oop inner = sg_find_field_oop(mh_oop, "target", "Ljava/lang/invoke/MethodHandle;");
  if (sg_oop_valid(inner)) return sg_unwrap_delegating(inner, depth - 1);
  return mh_oop;
}

// Read MethodHandle.type() → MethodType.methodDescriptor cached String.
// Returns ResourceMark-allocated C string, or nullptr if the descriptor
// has not yet been cached in the MethodType object.
static const char* sg_mh_descriptor(oop mh_oop) {
  if (!sg_oop_valid(mh_oop)) return nullptr;
  if (!java_lang_invoke_MethodHandle::is_instance(mh_oop)) return nullptr;
  oop mt = java_lang_invoke_MethodHandle::type(mh_oop);
  if (!sg_oop_valid(mt)) return nullptr;
  oop desc_str = sg_find_field_oop(mt, "methodDescriptor", "Ljava/lang/String;");
  if (!sg_oop_valid(desc_str) || !java_lang_String::is_instance(desc_str)) return nullptr;
  return java_lang_String::as_utf8_string(desc_str);
}

// Count L-type (reference) bound fields in a BMH species from its class name.
// "...BoundMethodHandle$Species_LLL" → 3.  Returns 0 if not a BMH species.
static int sg_bmh_species_l_count(oop mh_oop) {
  if (!sg_oop_valid(mh_oop)) return 0;
  const char* kn = mh_oop->klass()->name()->as_C_string();
  const char* sp = strstr(kn, "$Species_");
  if (!sp) return 0;
  int count = 0;
  for (const char* p = sp + 9; *p; p++) {
    if (*p == 'L') count++;
  }
  return count;
}

// Return true if klass is definitively java.lang.invoke.* or sun.invoke.* machinery —
// i.e., never a user/business callable target.
static bool sg_is_invoke_internal(const char* klass) {
  if (!klass) return false;
  return strncmp(klass, "java/lang/invoke/", 17) == 0 ||
         strncmp(klass, "sun/invoke/", 11) == 0;
}

// Classify an exact DMH target by package.
static const char* sg_classify_exact(const char* klass) {
  if (!klass) return "unknown";
  if (strncmp(klass, "sun/invoke/", 11) == 0)          return "helper_boxing";
  if (strncmp(klass, "java/lang/reflect/", 18) == 0 ||
      strcmp(klass, "java/lang/Class") == 0)            return "helper_reflection";
  if (sg_is_invoke_internal(klass)                   ||
      strncmp(klass, "java/", 5) == 0                ||
      strncmp(klass, "javax/", 6) == 0               ||
      strncmp(klass, "sun/", 4) == 0                 ||
      strncmp(klass, "jdk/", 4) == 0)                  return "internal_jdk";
  return "user_target";
}

// Classify a non-exact adapter node by its adapter class name.
static const char* sg_classify_nonexact(const char* adapter_class) {
  if (!adapter_class) return "unknown";
  if (strcmp(adapter_class, "java/lang/invoke/SimpleMethodHandle") == 0)
    return "helper_invoker";
  if (strstr(adapter_class, "BoundMethodHandle$Species_"))
    return "helper_adapter";
  if (sg_is_invoke_internal(adapter_class)) return "internal_jdk";
  return "unknown";
}

// Role labels for nested BMH slots.
static const char* sg_nested_role(int index) {
  static const char* nested_roles[] = {
    "nested_argL0","nested_argL1","nested_argL2","nested_argL3",
    "nested_argL4","nested_argL5","nested_argL6","nested_argL7"
  };
  return (index >= 0 && index < 8) ? nested_roles[index] : "nested_argLN";
}

// Recursively descend into a nested BMH, appending child nodes and "contains"
// edges to r.  parent_id is the node id whose slot contains this BMH.
// visited[0..n_visited-1] prevents infinite loops on cyclic MH graphs.
// depth decrements on each recursive call; recursion stops at 0.
// *leaf_all_exact is set to false if any reachable leaf cannot be resolved to a DMH.
static void sg_recurse_into_bmh(
    oop mh_oop, int parent_id,
    SgMhWalkResult* r,
    oop* visited, int* n_visited,
    int depth, bool* all_user_exact)
{
  // Depth/capacity guard. If the class is JDK-internal machinery, depth exhaustion
  // does NOT indicate a hidden user target — leave all_user_exact unchanged.
  if (r->n_graph_nodes >= 16) {
    if (all_user_exact && sg_oop_valid(mh_oop)) {
      if (!sg_is_invoke_internal(mh_oop->klass()->name()->as_C_string()))
        *all_user_exact = false;
    }
    return;
  }
  if (depth <= 0) {
    if (all_user_exact && sg_oop_valid(mh_oop)) {
      if (!sg_is_invoke_internal(mh_oop->klass()->name()->as_C_string()))
        *all_user_exact = false;
    }
    return;
  }
  if (!sg_oop_valid(mh_oop) || !java_lang_invoke_MethodHandle::is_instance(mh_oop)) {
    if (all_user_exact) *all_user_exact = false;
    return;
  }
  // Cycle detection.
  for (int v = 0; v < *n_visited; v++) {
    if (visited[v] == mh_oop) return;  // already visited; not a user target blocker
  }
  if (*n_visited < 8) visited[(*n_visited)++] = mh_oop;

  int n_l = sg_bmh_species_l_count(mh_oop);
  if (n_l <= 0 || n_l > 8) {
    // n_l == 0: SimpleMethodHandle (LF-driven, no bound args) — always invoke machinery.
    // n_l > 8: oversized species — conservative skip; classify by package.
    if (all_user_exact && sg_oop_valid(mh_oop)) {
      if (!sg_is_invoke_internal(mh_oop->klass()->name()->as_C_string()))
        *all_user_exact = false;
    }
    return;
  }

  for (int i = 0; i < n_l && r->n_graph_nodes < 16; i++) {
    oop f = sg_bmh_arg_at(mh_oop, i);
    // Non-MH slot: bound data value (Class object, int, etc.) — not a hidden user target.
    if (!sg_oop_valid(f) || !java_lang_invoke_MethodHandle::is_instance(f)) continue;

    int child_id = r->n_graph_nodes;
    SgAdapterNode& child = r->graph_nodes[r->n_graph_nodes++];
    memset(&child, 0, sizeof(child));
    child.id      = child_id;
    child.role    = sg_nested_role(i);
    child.to_desc = sg_mh_descriptor(f);

    // Add "contains" edge from parent to this child.
    if (r->n_graph_edges < 16) {
      r->graph_edges[r->n_graph_edges].from_id = parent_id;
      r->graph_edges[r->n_graph_edges].to_id   = child_id;
      r->graph_edges[r->n_graph_edges].kind    = "contains";
      r->n_graph_edges++;
    }

    oop unwrapped = sg_unwrap_delegating(f, 4);
    if (sg_extract_dmh_target(unwrapped, &child.target)) {
      child.has_target        = true;
      child.node_classification = sg_classify_exact(child.target.klass);
    } else if (java_lang_invoke_MethodHandle::is_instance(unwrapped)) {
      // Non-DMH MH: record species class, recurse.
      child.has_target           = false;
      child.node_adapter_class   = unwrapped->klass()->name()->as_C_string();
      child.node_classification  = sg_classify_nonexact(child.node_adapter_class);
      child.exact_false_reason   = child.node_classification;  // role as reason
      if (!sg_is_invoke_internal(child.node_adapter_class) && all_user_exact)
        *all_user_exact = false;
      sg_recurse_into_bmh(unwrapped, child_id, r, visited, n_visited,
                          depth - 1, all_user_exact);
    } else {
      // Some other non-MH value where a MH was expected — conservative.
      child.has_target          = false;
      child.node_classification = "unknown";
      child.exact_false_reason  = "not_a_methodhandle";
      if (all_user_exact) *all_user_exact = false;
    }
  }
}

// Walk a BoundMethodHandle (any LambdaForm kind) and extract its adapter graph.
//
// Strategy: enumerate argL{0..N-1} fields (the bound reference components of
// the BMH species), attempt exact DMH extraction for each, and return
// ADAPTER_GRAPH when at least one component was found.  Falls back to
// BMH_UNKNOWN when the species has no L fields.
//
// Role labels by lf_kind:
//   TRY_FINALLY → "target" (L0) / "cleanup" (L1)
//   COLLECTOR   → "collection_target" (L0)
//   LOOP        → "loop_component_N"
//   TABLE_SWITCH→ "switch_default" (L0) / "switch_case" (L1+)
//   GENERIC     → n_l==1: "adapted_target"; L0: "primary_target"; L1: "secondary_component";
//                 L2+: "component_N"
//
// graph_kind:
//   1 MH-component   → "type_conversion"   (asType-like)
//   2 MH-components  → "dual_target"       (filter/fold/filterRV — same structure)
//   3+ MH-components → "multi_target"      (filterArguments with multiple filters)
//   0 MH-components  → "bound_data"        (bindTo/insertArguments with non-MH values)
//   Named kinds      → override with kind name ("try_finally", "collector", …)
static SgMhWalkResult sg_walk_generic_bmh(oop mh_oop, int depth) {
  SgMhWalkResult r;
  memset(&r, 0, sizeof(r));
  r.shape = SgMhWalkResult::BMH_UNKNOWN;

  if (!sg_oop_valid(mh_oop) || depth <= 0) {
    snprintf(r.graph_diag, sizeof(r.graph_diag), "depth_exhausted");
    return r;
  }

  r.adapter_class = mh_oop->klass()->name()->as_C_string();
  r.lf_kind       = sg_lf_kind_name(mh_oop);
  r.outer_desc    = sg_mh_descriptor(mh_oop);

  const char* lf = r.lf_kind ? r.lf_kind : "";

  int n_l = sg_bmh_species_l_count(mh_oop);
  if (n_l == 0) {
    snprintf(r.graph_diag, sizeof(r.graph_diag), "no_l_fields_in_species");
    return r;
  }
  if (n_l > 8) n_l = 8;

  // Cycle detection buffer seeded with the outermost BMH itself.
  oop visited_buf[8];
  int n_visited = 0;
  visited_buf[n_visited++] = mh_oop;

  int mh_count = 0;
  // all_exact: true iff all user/business targets (non-JDK-internal classes) are
  // recovered exactly.  JDK-internal adapter nodes (SimpleMethodHandle, Species_*)
  // may remain non-exact without clearing this flag.
  bool all_exact = true;

  for (int i = 0; i < n_l; i++) {
    oop f = sg_bmh_arg_at(mh_oop, i);
    // Use current array position as ID — must be captured BEFORE the increment
    // so that IDs remain unique even after sg_recurse_into_bmh adds children.
    int my_id = r.n_graph_nodes;
    SgAdapterNode& node = r.graph_nodes[r.n_graph_nodes++];
    memset(&node, 0, sizeof(node));
    node.id = my_id;

    // Role assignment.
    if (strcmp(lf, "TRY_FINALLY") == 0) {
      node.role = (i == 0) ? "target" : "cleanup";
    } else if (strcmp(lf, "COLLECTOR") == 0) {
      node.role = "collection_target";
    } else if (strcmp(lf, "LOOP") == 0) {
      static const char* lroles[] = {
        "loop_component_0","loop_component_1","loop_component_2","loop_component_3",
        "loop_component_4","loop_component_5","loop_component_6","loop_component_7"
      };
      node.role = lroles[i];
    } else if (strcmp(lf, "TABLE_SWITCH") == 0) {
      node.role = (i == 0) ? "switch_default" : "switch_case";
    } else {
      // GENERIC or other
      if (n_l == 1) {
        node.role = "adapted_target";
      } else if (i == 0) {
        node.role = "primary_target";
      } else if (i == 1) {
        node.role = "secondary_component";
      } else {
        static const char* xroles[] = {
          "component_2","component_3","component_4",
          "component_5","component_6","component_7"
        };
        node.role = xroles[(i-2) < 6 ? (i-2) : 5];
      }
    }

    if (!sg_oop_valid(f)) {
      // Null/invalid argL slot — conservative: mark as unknown blocker.
      node.has_target           = false;
      node.node_classification  = "unknown";
      node.exact_false_reason   = "null_slot";
      all_exact = false;
      continue;
    }

    if (java_lang_invoke_MethodHandle::is_instance(f)) {
      mh_count++;
      oop unwrapped = sg_unwrap_delegating(f, 4);
      node.to_desc = sg_mh_descriptor(f);
      if (n_l == 1) node.from_desc = r.outer_desc; // outer→inner for single-target adapters
      if (sg_extract_dmh_target(unwrapped, &node.target)) {
        node.has_target          = true;
        node.node_classification = sg_classify_exact(node.target.klass);
      } else if (java_lang_invoke_MethodHandle::is_instance(unwrapped)) {
        // Nested BMH: record species class, recurse to find children.
        node.has_target           = false;
        node.node_adapter_class   = unwrapped->klass()->name()->as_C_string();
        node.node_classification  = sg_classify_nonexact(node.node_adapter_class);
        node.exact_false_reason   = node.node_classification;
        // Only block all_exact if this is not a known JDK-internal adapter.
        if (!sg_is_invoke_internal(node.node_adapter_class)) all_exact = false;
        sg_recurse_into_bmh(unwrapped, my_id, &r, visited_buf, &n_visited,
                            3, &all_exact);
      } else {
        // Non-BMH, non-DMH MH: unknown.
        node.has_target           = false;
        node.node_classification  = "unknown";
        node.exact_false_reason   = "unknown_mh_type";
        all_exact = false;
      }
    } else {
      // Non-MH bound value (bindTo / insertArguments with concrete values).
      // This is bound data, not a hidden user MH target.
      node.has_target           = false;
      node.node_classification  = "bound_data";
      node.exact_false_reason   = "non_mh_bound_value";
      // Do NOT clear all_exact: bound data is never a user callable target.
    }
  }

  // Determine graph_kind.
  if      (strcmp(lf, "TRY_FINALLY")  == 0) r.graph_kind = "try_finally";
  else if (strcmp(lf, "COLLECTOR")    == 0) r.graph_kind = "collector";
  else if (strcmp(lf, "LOOP")         == 0) r.graph_kind = "loop";
  else if (strcmp(lf, "TABLE_SWITCH") == 0) r.graph_kind = "table_switch";
  else if (mh_count == 0)                   r.graph_kind = "bound_data";
  else if (mh_count == 1)                   r.graph_kind = "type_conversion";
  else if (mh_count == 2)                   r.graph_kind = "dual_target";
  else                                      r.graph_kind = "multi_target";

  r.graph_all_exact = all_exact;
  r.shape = SgMhWalkResult::ADAPTER_GRAPH;
  return r;
}

// ---------------------------------------------------------------------------
// sg_recover_mh_recv_from_java_sp — runtime MH receiver recovery.
//
// When the symbolic stack simulator fails to locate the MH receiver in a local
// variable (e.g., receiver is a method-return temporary, chained expression,
// field read, array element, or the simulator's stack accounting drifts on
// complex bytecode sequences), this function recovers the ACTUAL runtime
// MethodHandle object directly from the live Java expression stack via
// JavaThread::last_Java_sp().
//
// Design: receiver-origin is IRRELEVANT.  We care only what MH object was
// actually being invoked at runtime, regardless of how it was produced.
//
// Contract (valid during call_VM, interpreter mode only):
//   JavaThread::last_Java_sp() is saved into the JavaThread anchor at
//   call_VM entry (NOT the frame's last_sp field, which is set to NULL on
//   aarch64 — that is what makes interpreter_frame_expression_stack_at
//   unsafe here).  last_Java_sp holds the Java expression-stack pointer (esp)
//   at the moment of the invokevirtual, pointing to the most recently pushed
//   Java slot (the last argument).
//
//   Stack layout at invokevirtual entry (stack grows toward lower addresses):
//
//     lower address  ← last_Java_sp  →  [arg_{n-1}]   (last arg, e.g. int 5)
//                                        [arg_{n-2}]
//                                        ...
//                                        [arg_0]        ← last_Java_sp + (arg_slots-1)
//                                        [receiver MH]  ← last_Java_sp + arg_slots
//     higher address
//
//   arg_slots = sg_count_param_slots(invocation_descriptor), i.e. the number
//   of parameter slots NOT counting the receiver.
//
// Safe for: local MH, field MH, array-element MH, method-return MH,
//           chained temporary MH, nested combinator MH.
// Not applied for Case A2 (recv_frame_valid=true) where last_Java_sp reflects
// a LF dispatch frame's stack, not the user callsite frame.
// ---------------------------------------------------------------------------
static oop sg_recover_mh_recv_from_java_sp(JavaThread* thread, int arg_slots) {
  if (thread == nullptr || arg_slots < 0) return nullptr;

  intptr_t* java_sp = (intptr_t*)thread->last_Java_sp();
  if (java_sp == nullptr) return nullptr;

  // Receiver is arg_slots slots above the stack top (higher address).
  intptr_t* recv_slot = java_sp + arg_slots;

  // Bounds check against thread's Java stack region.
  uintptr_t recv_addr  = (uintptr_t)recv_slot;
  uintptr_t stack_end  = (uintptr_t)thread->stack_end();   // lowest valid address
  uintptr_t stack_base = (uintptr_t)thread->stack_base();  // highest valid address
  if (recv_addr < stack_end || recv_addr + sizeof(intptr_t) > stack_base) return nullptr;

  oop candidate = *(oop*)recv_slot;
  if (!sg_oop_valid(candidate)) return nullptr;
  if (!java_lang_invoke_MethodHandle::is_instance(candidate)) return nullptr;

  return candidate;
}

// Walk a MethodHandle oop and classify its adapter structure.
// Depth-limited; returns shape=MH_UNKNOWN on any error or depth exhaustion.
static SgMhWalkResult sg_walk_mh(oop mh_oop, int depth) {
  SgMhWalkResult r;
  memset(&r, 0, sizeof(r));
  r.shape = SgMhWalkResult::MH_UNKNOWN;

  if (!sg_oop_valid(mh_oop) || depth <= 0) return r;

  Klass* k = mh_oop->klass();
  if (k == nullptr) return r;
  r.adapter_class = k->name()->as_C_string();

  // ---- Case 1: DirectMethodHandle (includes Special/Interface/Constructor) ----
  if (java_lang_invoke_DirectMethodHandle::is_instance(mh_oop)) {
    r.shape     = SgMhWalkResult::DIRECT;
    r.n_targets = 1;
    r.roles[0]  = "direct_target";
    r.all_exact = sg_extract_dmh_target(mh_oop, &r.targets[0]);
    return r;
  }

  // ---- Case 2: DelegatingMethodHandle subclass — transparent wrapper ----
  oop delegate = sg_find_field_oop(mh_oop, "target", "Ljava/lang/invoke/MethodHandle;");
  if (sg_oop_valid(delegate)) {
    SgMhWalkResult inner = sg_walk_mh(delegate, depth - 1);
    inner.adapter_class = r.adapter_class; // report outermost wrapper class
    return inner;
  }

  // ---- Case 3: BoundMethodHandle species ----
  const char* kn = r.adapter_class;
  bool is_bmh = (strncmp(kn, "java/lang/invoke/BoundMethodHandle", 34) == 0);
  if (!is_bmh) return r; // shape stays MH_UNKNOWN

  r.lf_kind = sg_lf_kind_name(mh_oop);
  if (r.lf_kind == nullptr) { r.shape = SgMhWalkResult::BMH_UNKNOWN; return r; }

  // ---- Case 3a: guardWithTest ----
  if (strcmp(r.lf_kind, "GUARD") == 0) {
    r.shape     = SgMhWalkResult::GWT;
    r.n_targets = 3;
    r.roles[0]  = "test";
    r.roles[1]  = "true_target";
    r.roles[2]  = "false_target";
    r.all_exact = true;

    oop t[3];
    t[0] = sg_unwrap_delegating(sg_bmh_arg_at(mh_oop, 0), 4);
    t[1] = sg_unwrap_delegating(sg_bmh_arg_at(mh_oop, 1), 4);
    t[2] = sg_unwrap_delegating(sg_bmh_arg_at(mh_oop, 2), 4);

    for (int i = 0; i < 3; i++) {
      if (!sg_extract_dmh_target(t[i], &r.targets[i])) {
        r.targets[i].klass = (sg_oop_valid(t[i]))
            ? t[i]->klass()->name()->as_C_string() : "null";
        r.all_exact = false;
      }
    }
    return r;
  }

  // ---- Case 3b: catchException ----
  if (strcmp(r.lf_kind, "GUARD_WITH_CATCH") == 0) {
    r.shape     = SgMhWalkResult::GWC;
    r.n_targets = 2;
    r.roles[0]  = "try_target";
    r.roles[1]  = "handler";
    r.all_exact = true;

    oop t0 = sg_unwrap_delegating(sg_bmh_arg_at(mh_oop, 0), 4);
    oop t2 = sg_unwrap_delegating(sg_bmh_arg_at(mh_oop, 2), 4);

    // argL1 is the exception Class object (not a MethodHandle)
    oop ex_oop = sg_bmh_arg_at(mh_oop, 1);
    if (sg_oop_valid(ex_oop)) {
      Klass* ex_k = java_lang_Class::as_Klass(ex_oop);
      if (ex_k != nullptr) r.aux_info = ex_k->name()->as_C_string();
    }

    if (!sg_extract_dmh_target(t0, &r.targets[0])) {
      r.targets[0].klass = sg_oop_valid(t0) ? t0->klass()->name()->as_C_string() : "null";
      r.all_exact = false;
    }
    if (!sg_extract_dmh_target(t2, &r.targets[1])) {
      r.targets[1].klass = sg_oop_valid(t2) ? t2->klass()->name()->as_C_string() : "null";
      r.all_exact = false;
    }
    return r;
  }

  // ---- Case 3c: GENERIC and other named BMH forms — try adapter graph extraction ----
  {
    SgMhWalkResult gr = sg_walk_generic_bmh(mh_oop, depth - 1);
    if (gr.shape == SgMhWalkResult::ADAPTER_GRAPH) return gr;
    // Extraction failed (no L fields or depth exhausted); fall through.
  }
  r.shape = SgMhWalkResult::BMH_UNKNOWN;
  return r;
}

void LinkResolver::resolve_handle_call(CallInfo& result,
                                       const LinkInfo& link_info,
                                       TRAPS) {
  // JSR 292:  this must be an implicitly generated method MethodHandle.invokeExact(*...) or similar
  Klass* resolved_klass = link_info.resolved_klass();
  assert(resolved_klass == vmClasses::MethodHandle_klass() ||
         resolved_klass == vmClasses::VarHandle_klass(), "");
  assert(MethodHandles::is_signature_polymorphic_name(link_info.name()), "");
  Handle resolved_appendix;
  Method* m = lookup_polymorphic_method(link_info, &resolved_appendix, CHECK);
  methodHandle resolved_method(THREAD, m);

  if (link_info.check_access()) {
    Symbol* name = link_info.name();
    vmIntrinsics::ID iid = MethodHandles::signature_polymorphic_name_id(name);
    if (MethodHandles::is_signature_polymorphic_intrinsic(iid)) {
      // Check if method can be accessed by the referring class.
      // MH.linkTo* invocations are not rewritten to invokehandle.
      assert(iid == vmIntrinsicID::_invokeBasic, "%s", vmIntrinsics::name_at(iid));

      Klass* current_klass = link_info.current_klass();
      assert(current_klass != nullptr , "current_klass should not be null");
      check_method_accessability(current_klass,
                                 resolved_klass,
                                 resolved_method->method_holder(),
                                 resolved_method,
                                 CHECK);
    } else {
      // Java code is free to arbitrarily link signature-polymorphic invokers.
      assert(iid == vmIntrinsics::_invokeGeneric, "not an invoker: %s", vmIntrinsics::name_at(iid));
      assert(MethodHandles::is_signature_polymorphic_public_name(resolved_klass, name), "not public");
    }
  }
  result.set_handle(resolved_klass, resolved_method, resolved_appendix, CHECK);
  if (soroush_trace_reflection_enabled()) {
    ResourceMark rm(THREAD);
    Klass* current_klass = link_info.current_klass();
    fprintf(stderr, "[JVM REFLECT] caller=%s\n",
            current_klass == nullptr ? "<unknown>" : current_klass->name()->as_C_string());
    fprintf(stderr, "[JVM REFLECT] methodhandle_dispatch=%s%s\n",
            link_info.name()->as_C_string(),
            link_info.signature()->as_C_string());
    fprintf(stderr, "[JVM REFLECT] final_resolved_method=%s.%s%s\n",
            resolved_method->method_holder()->name()->as_C_string(),
            resolved_method->name()->as_C_string(),
            resolved_method->signature()->as_C_string());
    if (resolved_appendix.not_null()) {
      fprintf(stderr, "[JVM REFLECT] mh_target=");
      soroush_print_reflect_method_handle(resolved_appendix());
      fprintf(stderr, "\n");
    }
  }
  // MethodHandle / LambdaForm execution-structure tracing (observational).
  if (resolved_appendix.not_null()) {
    soroush_trace_mh_execution(resolved_appendix(), "invokeBasic-link",
                               link_info.current_klass(), THREAD);
  }

  // ---------------------------------------------------------------------------
  // Soroush: source + runtime-receiver callsite capture for MethodHandle and
  // JDK 21 reflection.
  //
  // Fires on first linkage (resolve_handle_call called once per callsite).
  // At this point all Java arguments are still on the expression stack of the
  // calling interpreter frame, so we can read the actual MH receiver object
  // and inspect it directly — no appendix-guessing needed.
  //
  // Case A — normal user MH call (e.g. mh.invokeExact(a, b)):
  //   top frame = user's interpreter frame executing invokehandle.
  //   Source: read BCI + class/method/desc from that frame.
  //   Target: read MH receiver from expression stack at offset arg_slots;
  //           exact iff receiver is a DirectMethodHandle with vmtarget.
  //
  // Case B — JDK 21 reflection via DirectMethodHandleAccessor.invokeImpl or
  //   DirectConstructorHandleAccessor.invokeImpl (JEP 416):
  //   top frame = accessor frame; user frame is deeper in the stack.
  //   Source: walk vframeStream past reflection internals to user frame.
  //   Target: MH receiver on accessor's expression stack is the accessor's
  //           `target` field — exact iff it is a DirectMethodHandle.
  //   Category: changed to reflection_method_invoke /
  //             reflection_constructor_newInstance.
  //
  // Deferred (not handled here, see SOROUSH_JVM_SPEC.md):
  //   - Adapter MH chains (boxing/unboxing) — receiver is not a DMH.
  //   - Compiled-frame BCI recovery.
  //   - Virtual/interface dispatch callsite probes.
  //   - Redefine/retransform lineage.
  //   - Full adapter unwrapping.
  //
  // Hard rules: if source or target is not exactly determinable,
  // emit a diagnostic record — never emit a callsite_target with guessed data.
  // ---------------------------------------------------------------------------
  if (soroush_graph_enabled()) {
    ResourceMark rm_mh(THREAD);

    Symbol* mh_name = link_info.name();
    const char* cat = "methodhandle_invoke";
    if (mh_name == vmSymbols::invokeExact_name()) {
      cat = "methodhandle_invokeExact";
    }

    const char* src_class  = nullptr;
    const char* src_method = nullptr;
    const char* src_desc   = nullptr;
    int  src_bci     = -1;
    int  src_opcode  = 0;
    int  src_cp      = -1;
    uint64_t src_loader  = 0;
    bool src_ok = false;

    const char* tgt_class  = nullptr;
    const char* tgt_method = nullptr;
    const char* tgt_desc   = nullptr;
    uint64_t    tgt_loader = 0;
    bool tgt_ok = false;

    bool is_reflection_accessor = false;
    bool top_frame_valid = false;
    frame top;
    // recv_frame: the frame from which we read the MH receiver local variable.
    // Equals `top` for direct user callsites; equals the walked-to user frame when
    // resolve_handle_call fires from inside a java/lang/invoke/* LambdaForm.
    frame recv_frame;
    bool  recv_frame_valid = false;
    Method* recv_frame_method = nullptr;
    // is_lf_dispatch_case: true iff the top interpreter frame is a java/lang/invoke
    // LambdaForm/BMH internal and we are in Case A2.  Used by Step 2 to choose
    // between reading from last_Java_sp (Case A) vs top.local_at(0) (Case A2).
    bool is_lf_dispatch_case = false;

    if (THREAD->has_last_Java_frame()) {
      top = THREAD->last_frame();
      if (top.is_interpreted_frame()) {
        top_frame_valid = true;
        Method* cm = top.interpreter_frame_method();
        if (cm != nullptr) {
          const char* holder = cm->method_holder()->name()->as_C_string();
          bool is_dmha = (strncmp(holder,
              "jdk/internal/reflect/DirectMethodHandleAccessor",   47) == 0);
          bool is_dcha = (strncmp(holder,
              "jdk/internal/reflect/DirectConstructorHandleAccessor", 51) == 0);

          if (is_dmha || is_dcha) {
            // ---- Case B: JDK 21 reflection accessor ----
            is_reflection_accessor = true;
            cat = is_dmha ? "reflection_method_invoke"
                          : "reflection_constructor_newInstance";

            // Walk vframeStream past reflection internals to user's frame.
            for (vframeStream vfst(THREAD); !vfst.at_end(); vfst.next()) {
              Method* m = vfst.method();
              if (m == nullptr) continue;
              const char* h = m->method_holder()->name()->as_C_string();
              if (strncmp(h, "jdk/internal/reflect/", 21) == 0 ||
                  strncmp(h, "java/lang/reflect/",    18) == 0 ||
                  strncmp(h, "sun/reflect/",          12) == 0) {
                continue;
              }
              int bci = vfst.bci();
              if (bci < 0) break;
              src_class  = h;
              src_loader = (uint64_t)(uintptr_t)m->method_holder()->class_loader_data();
              src_method = m->name()->as_C_string();
              src_desc   = m->signature()->as_C_string();
              src_bci    = bci;
              if (bci < m->code_size()) {
                address bcp = m->bcp_from(bci);
                src_opcode = (int)(uint8_t)Bytecodes::java_code_at(m, bcp);
                if (bci + 2 < m->code_size()) {
                  int cc_idx = (((int)(uint8_t)m->code_base()[bci + 1]) << 8)
                             |  ((int)(uint8_t)m->code_base()[bci + 2]);
                  ConstantPool* cpool = m->constants();
                  if (cpool != nullptr && cpool->cache() != nullptr
                      && cc_idx >= 0 && cc_idx < cpool->cache()->length()) {
                    src_cp = cpool->cache()->entry_at(cc_idx)->constant_pool_index();
                  }
                }
              }
              src_ok = true;
              break;
            }

          } else if (strncmp(holder, "java/lang/invoke/", 17) == 0) {
            // ---- Case A2: top frame is a java/lang/invoke LambdaForm/BMH internal.
            // resolve_handle_call fires from inside an adapter chain (e.g. tryFinally
            // LF dispatching to a component MH).  Walk the vframeStream past ALL
            // java/lang/invoke frames — including intermediate stub/adapter frames
            // that raw frame walking cannot skip — to recover the original user
            // callsite AND to read the actual MH receiver from user frame locals.
            // vframeStream handles compiled/deopt/stub frames transparently.
            is_lf_dispatch_case = true;
            bool found_user_frame = false;
            for (vframeStream vfst(THREAD); !vfst.at_end(); vfst.next()) {
              Method* sm = vfst.method();
              if (sm == nullptr) continue;
              const char* sh = sm->method_holder()->name()->as_C_string();
              if (strncmp(sh, "java/lang/invoke/", 17) == 0) continue;
              int u_bci = vfst.bci();
              if (u_bci < 0) break;
              src_class  = sh;
              src_loader = (uint64_t)(uintptr_t)sm->method_holder()->class_loader_data();
              src_method = sm->name()->as_C_string();
              src_desc   = sm->signature()->as_C_string();
              src_bci    = u_bci;
              if (u_bci < sm->code_size()) {
                address bcp = sm->bcp_from(u_bci);
                src_opcode = (int)(uint8_t)Bytecodes::java_code_at(sm, bcp);
                if (u_bci + 2 < sm->code_size()) {
                  int cc_idx = sg_u2at(bcp + 1);
                  ConstantPool* cpool = sm->constants();
                  if (cpool != nullptr && cpool->cache() != nullptr
                      && cc_idx >= 0 && cc_idx < cpool->cache()->length()) {
                    src_cp = cpool->cache()->entry_at(cc_idx)->constant_pool_index();
                  }
                }
              }
              src_ok = true;
              recv_frame_method = sm;
              found_user_frame  = true;
              // Get the raw frame for interpreter_frame_local_at.
              // Walk raw frames from top to find the matching interpreted frame.
              {
                RegisterMap rmap2(THREAD,
                                  RegisterMap::UpdateMap::skip,
                                  RegisterMap::ProcessFrames::skip,
                                  RegisterMap::WalkContinuation::skip);
                frame rscan = THREAD->last_frame();
                while (true) {
                  if (rscan.is_interpreted_frame()) {
                    Method* rm = rscan.interpreter_frame_method();
                    if (rm == sm && rscan.interpreter_frame_bci() == u_bci) {
                      recv_frame       = rscan;
                      recv_frame_valid = true;
                      break;
                    }
                  }
                  if (rscan.is_first_frame()) break;
                  rscan = rscan.sender(&rmap2);
                }
              }
              break;
            }
            if (!src_ok) {
              // Could not recover user frame — attribute to the LF frame itself.
              src_bci = top.interpreter_frame_bci();
              if (src_bci >= 0) {
                src_class  = holder;
                src_loader = (uint64_t)(uintptr_t)cm->method_holder()->class_loader_data();
                src_method = cm->name()->as_C_string();
                src_desc   = cm->signature()->as_C_string();
                if (src_bci < cm->code_size()) {
                  address bcp = cm->bcp_from(src_bci);
                  src_opcode = (int)(uint8_t)Bytecodes::java_code_at(cm, bcp);
                  if (src_bci + 2 < cm->code_size()) {
                    int cc_idx = sg_u2at(bcp + 1);
                    ConstantPool* cpool = cm->constants();
                    if (cpool != nullptr && cpool->cache() != nullptr
                        && cc_idx >= 0 && cc_idx < cpool->cache()->length()) {
                      src_cp = cpool->cache()->entry_at(cc_idx)->constant_pool_index();
                    }
                  }
                }
                src_ok = true;
                recv_frame        = top;
                recv_frame_valid  = top_frame_valid;
                recv_frame_method = cm;
              }
            }
            (void)found_user_frame;

          } else {
            // ---- Case A: normal user MH callsite (top frame IS the user frame) ----
            src_bci = top.interpreter_frame_bci();
            if (src_bci >= 0) {
              src_class  = holder;
              src_loader = (uint64_t)(uintptr_t)cm->method_holder()->class_loader_data();
              src_method = cm->name()->as_C_string();
              src_desc   = cm->signature()->as_C_string();
              if (src_bci < cm->code_size()) {
                address bcp = cm->bcp_from(src_bci);
                src_opcode = (int)(uint8_t)Bytecodes::java_code_at(cm, bcp);
                if (src_bci + 2 < cm->code_size()) {
                  int cc_idx = (((int)(uint8_t)cm->code_base()[src_bci + 1]) << 8)
                             |  ((int)(uint8_t)cm->code_base()[src_bci + 2]);
                  ConstantPool* cpool = cm->constants();
                  if (cpool != nullptr && cpool->cache() != nullptr
                      && cc_idx >= 0 && cc_idx < cpool->cache()->length()) {
                    src_cp = cpool->cache()->entry_at(cc_idx)->constant_pool_index();
                  }
                }
              }
              src_ok = true;
              recv_frame        = top;
              recv_frame_valid  = top_frame_valid;
              recv_frame_method = cm;
            }
          }
        }
      }
    }

    // ---- Runtime receiver inspection ----
    //
    // Case A / A2 (user or recovered user frame): recover the MH receiver oop
    //   using a two-step strategy:
    //
    //   Step 1 (primary): symbolic stack simulator locates the receiver in a
    //     local variable; read via interpreter_frame_local_at (FP-relative,
    //     safe during call_VM on aarch64 regardless of last_sp state).
    //
    //   Step 2 (fallback): when the simulator fails — receiver is a stack
    //     temporary from a method return, chained expression, field read, or
    //     array element — recover the actual receiver oop from the live Java
    //     expression stack via sg_recover_mh_recv_from_java_sp().
    //     thread->last_Java_sp() was saved into the JavaThread anchor at
    //     call_VM entry; it is NOT the frame's last_sp field (which IS null).
    //     This makes receiver origin IRRELEVANT: local, field, array, method
    //     return, or inline temporary are all recovered identically.
    //     Skip for Case A2 (recv_frame_valid=true) where last_Java_sp reflects
    //     a LF dispatch frame's stack, not the user callsite frame.
    //
    // Case B (JDK 21 reflection accessor): read accessor `this` from local 0,
    //   then find the `target` MethodHandle field via JavaFieldStream.
    bool multi_target_emitted = false;
    char adapter_diag[384] = "";
    // primary_recv_local: local slot of the MH receiver at src_bci.  Used by
    // the sibling BCI scan (Part B) to distinguish same-receiver vs distinct.
    int  primary_recv_local = -1;
    int  primary_arg_slots  = sg_count_param_slots(link_info.signature());
    if ((recv_frame_valid || top_frame_valid) && !is_reflection_accessor) {
      // Case A / A2: walk the runtime MH receiver oop.
      // Use recv_frame (user frame) and recv_frame_method (user method) for
      // receiver reading.  Fall back to top/top's method if recv_frame not set.
      frame&  use_frame  = recv_frame_valid  ? recv_frame  : top;
      Method* tcm        = recv_frame_valid  ? recv_frame_method
                                             : top.interpreter_frame_method();
      if (src_ok && tcm != nullptr) {
        int arg_slots = primary_arg_slots;
        if (arg_slots >= 0) {
          // Step 1: symbolic stack simulator — tries to find receiver in a local.
          SgRecvResult recv_r = sg_analyze_mh_receiver(tcm, src_bci, arg_slots);
          oop mh_recv = nullptr;
          if (recv_r.mode == SG_RECV_LOCAL) {
            primary_recv_local = recv_r.local_n;
            intptr_t* local_ptr = use_frame.interpreter_frame_local_at(recv_r.local_n);
            if (local_ptr != nullptr) {
              oop candidate = *(oop*)local_ptr;
              if (sg_oop_valid(candidate) &&
                  java_lang_invoke_MethodHandle::is_instance(candidate)) {
                mh_recv = candidate;
              }
            }
          }
          // Step 2: runtime stack fallback — Case A (top frame IS the user frame).
          // Read the actual MH receiver from the live Java expression stack via
          // thread->last_Java_sp() (saved in the JavaThread anchor at call_VM
          // entry, NOT the frame's last_sp which is NULL on aarch64).
          // Stack at invokevirtual entry (grows toward lower addresses):
          //   last_Java_sp → [last_arg]  ...  [arg_0]  [receiver_MH]
          //                                           ↑ last_Java_sp + arg_slots
          // Skip for Case A2 (is_lf_dispatch_case=true): last_Java_sp there
          // reflects the LF dispatch frame's stack, not the user callsite frame.
          if (mh_recv == nullptr && !is_lf_dispatch_case) {
            oop candidate = sg_recover_mh_recv_from_java_sp(
                JavaThread::cast(THREAD), arg_slots);
            if (candidate != nullptr) {
              mh_recv = candidate;
            }
          }
          // Step 2a: Case A2 — read MH from local[0] of the LF dispatch top frame.
          // In Case A2, `top` is the innermost java/lang/invoke LF frame whose
          // local[0] is always the MethodHandle being invoked (the `this` of
          // invokeExact/invoke).  FP-relative, safe during call_VM on aarch64.
          if (mh_recv == nullptr && is_lf_dispatch_case) {
            intptr_t* local0 = top.interpreter_frame_local_at(0);
            if (local0 != nullptr) {
              oop candidate = *(oop*)local0;
              if (sg_oop_valid(candidate) &&
                  java_lang_invoke_MethodHandle::is_instance(candidate)) {
                mh_recv = candidate;
              }
            }
          }
          // Step 3: walk whichever MH oop we obtained.
          if (mh_recv != nullptr) {
            SgMhWalkResult walk = sg_walk_mh(mh_recv, 6);
            if (walk.shape == SgMhWalkResult::DIRECT && walk.all_exact) {
              // Single exact target → existing callsite_target path.
              tgt_class  = walk.targets[0].klass;
              tgt_loader = walk.targets[0].loader_id;
              tgt_method = walk.targets[0].method;
              tgt_desc   = walk.targets[0].descriptor;
              tgt_ok = true;
            } else if ((walk.shape == SgMhWalkResult::GWT ||
                         walk.shape == SgMhWalkResult::GWC) && walk.all_exact) {
              // Multi-target: emit callsite_target_set and skip generic path.
              SgMhTargetEntry entries[4];
              memset(entries, 0, sizeof(entries));
              for (int i = 0; i < walk.n_targets && i < 4; i++) {
                entries[i].klass      = walk.targets[i].klass;
                entries[i].loader_id  = walk.targets[i].loader_id;
                entries[i].method     = walk.targets[i].method;
                entries[i].descriptor = walk.targets[i].descriptor;
                entries[i].role       = walk.roles[i];
                entries[i].valid      = walk.targets[i].valid;
              }
              const char* shape_str = (walk.shape == SgMhWalkResult::GWT) ? "GWT" : "GWC";
              if (soroush_graph_target_set_callsite(
                  cat, shape_str, walk.adapter_class, walk.lf_kind, walk.aux_info,
                  src_class, src_loader, src_method, src_desc,
                  src_bci, src_opcode, src_cp,
                  entries, walk.n_targets)) {
                multi_target_emitted = true;
              } else {
                snprintf(adapter_diag, sizeof(adapter_diag), "record_storage_oom");
              }
            } else if (walk.shape == SgMhWalkResult::ADAPTER_GRAPH) {
              // GENERIC/named BMH: adapter graph extracted.
              SgAdapterNodeEntry node_entries[16];
              memset(node_entries, 0, sizeof(node_entries));
              int ne = (walk.n_graph_nodes < 16) ? walk.n_graph_nodes : 16;
              for (int i = 0; i < ne; i++) {
                node_entries[i].id                 = walk.graph_nodes[i].id;
                node_entries[i].role               = walk.graph_nodes[i].role;
                node_entries[i].from_desc          = walk.graph_nodes[i].from_desc;
                node_entries[i].to_desc            = walk.graph_nodes[i].to_desc;
                node_entries[i].node_adapter_class = walk.graph_nodes[i].node_adapter_class;
                node_entries[i].node_classification= walk.graph_nodes[i].node_classification;
                node_entries[i].exact_false_reason = walk.graph_nodes[i].exact_false_reason;
                if (walk.graph_nodes[i].has_target) {
                  node_entries[i].klass      = walk.graph_nodes[i].target.klass;
                  node_entries[i].loader_id  = walk.graph_nodes[i].target.loader_id;
                  node_entries[i].method     = walk.graph_nodes[i].target.method;
                  node_entries[i].descriptor = walk.graph_nodes[i].target.descriptor;
                  node_entries[i].exact      = true;
                }
              }
              SgAdapterEdgeEntry edge_entries[16];
              memset(edge_entries, 0, sizeof(edge_entries));
              int nee = (walk.n_graph_edges < 16) ? walk.n_graph_edges : 16;
              for (int i = 0; i < nee; i++) {
                edge_entries[i].from_id = walk.graph_edges[i].from_id;
                edge_entries[i].to_id   = walk.graph_edges[i].to_id;
                edge_entries[i].kind    = walk.graph_edges[i].kind;
              }
              if (soroush_graph_adapter_graph_callsite(
                cat,
                walk.adapter_class, walk.graph_kind, walk.lf_kind, walk.outer_desc,
                src_class, src_loader, src_method, src_desc,
                src_bci, src_opcode, src_cp,
                node_entries, ne, edge_entries, nee, walk.graph_all_exact)) {
                multi_target_emitted = true;
              } else {
                snprintf(adapter_diag, sizeof(adapter_diag), "record_storage_oom");
              }
            } else {
              // BMH_UNKNOWN / MH_UNKNOWN: shape not recognised or extraction failed.
              const char* ac = walk.adapter_class ? walk.adapter_class : "unknown";
              if (walk.lf_kind != nullptr) {
                snprintf(adapter_diag, sizeof(adapter_diag),
                         "adapter_not_all_exact lf_kind=%s adapter_class=%s",
                         walk.lf_kind, ac);
              } else {
                snprintf(adapter_diag, sizeof(adapter_diag),
                         "adapter_unknown_shape adapter_class=%s", ac);
              }
            }
          } else {
            // All recovery paths exhausted — emit explicit diagnostic.
            if (recv_r.mode != SG_RECV_LOCAL) {
              snprintf(adapter_diag, sizeof(adapter_diag),
                       "recv_analyzer_%s", recv_r.diag);
            } else {
              snprintf(adapter_diag, sizeof(adapter_diag), "recv_local_oop_invalid");
            }
          }
        }
      }
    } else if (top_frame_valid && is_reflection_accessor) {
      // Case B: accessor `this` is local 0; find its `target` field.
      //
      // The accessor's `target` field (set by MethodHandleAccessorFactory) is a
      // MethodHandle that may be:
      //   (a) a DirectMethodHandle   — 0-param or trivial asType returns raw DMH
      //   (b) a BoundMethodHandle adapter wrapping a DMH
      //       MethodHandleAccessorFactory.makeSpecializedTarget always applies
      //       dropArguments (static methods), asType (type adaptation), or
      //       asSpreader (>3-param methods/constructors), all of which produce BMHs.
      //
      // Strategy: use sg_walk_mh to traverse any adapter chain and extract the
      // underlying reflected Method*.  This makes target recovery independent of
      // whether the accessor used a plain DMH or any depth of BMH wrapping.
      intptr_t* local0 = top.interpreter_frame_local_at(0);
      if (local0 != nullptr) {
        oop accessor_obj = *(oop*)local0;
        if (accessor_obj != nullptr && oopDesc::is_oop_or_null(accessor_obj)) {
          Klass* ak = accessor_obj->klass();
          if (ak != nullptr && ak->is_instance_klass()) {
            InstanceKlass* ik = InstanceKlass::cast(ak);
            for (JavaFieldStream jfs(ik); !jfs.done(); jfs.next()) {
              Symbol* fn = jfs.name();
              Symbol* fs = jfs.signature();
              if (fn == nullptr || fs == nullptr) continue;
              if (strcmp(fn->as_C_string(), "target") == 0 &&
                  strncmp(fs->as_C_string(), "Ljava/lang/invoke/MethodHandle;", 31) == 0) {
                fieldDescriptor& fd = jfs.field_descriptor();
                oop target_mh = accessor_obj->obj_field(fd.offset());
                if (target_mh != nullptr && sg_oop_valid(target_mh) &&
                    java_lang_invoke_MethodHandle::is_instance(target_mh)) {
                  SgMhWalkResult walk = sg_walk_mh(target_mh, 6);
                  if (walk.shape == SgMhWalkResult::DIRECT &&
                      walk.n_targets > 0 && walk.targets[0].valid) {
                    // Fast path: target is already a DMH (0-param, trivial asType).
                    tgt_class  = walk.targets[0].klass;
                    tgt_loader = walk.targets[0].loader_id;
                    tgt_method = walk.targets[0].method;
                    tgt_desc   = walk.targets[0].descriptor;
                    tgt_ok = true;
                  } else if (walk.shape == SgMhWalkResult::ADAPTER_GRAPH) {
                    // Adapter path: target is a BMH chain (asType/dropArguments/
                    // asSpreader).  Walk graph nodes to find the first valid DMH
                    // — that is the actual reflected method.
                    for (int ni = 0; ni < walk.n_graph_nodes && !tgt_ok; ni++) {
                      const SgAdapterNode& gn = walk.graph_nodes[ni];
                      if (gn.has_target && gn.target.valid) {
                        tgt_class  = gn.target.klass;
                        tgt_loader = gn.target.loader_id;
                        tgt_method = gn.target.method;
                        tgt_desc   = gn.target.descriptor;
                        tgt_ok = true;
                      }
                    }
                  }
                }
                break;
              }
            }
          }
        }
      }
    }

    if (!multi_target_emitted) {
      char reason[256] = "";
      bool exact = src_ok && tgt_ok;
      if (!exact) {
        if (!src_ok && !tgt_ok)
          snprintf(reason, sizeof(reason),
              "source_compiled_frame_unavailable,target_not_resolved");
        else if (!src_ok)
          snprintf(reason, sizeof(reason), is_reflection_accessor
              ? "reflection_user_frame_not_found_in_stack"
              : "source_compiled_frame_bci_unavailable");
        else if (adapter_diag[0])
          snprintf(reason, sizeof(reason), "%s", adapter_diag);
        else
          snprintf(reason, sizeof(reason), is_reflection_accessor
              ? "reflection_target_adapter_mh_deferred"
              : "runtime_receiver_not_direct_mh_deferred");
      }
      if (!soroush_graph_generic_callsite(cat,
          src_class, src_loader, src_method, src_desc,
          src_bci, src_opcode, src_cp,
          tgt_class, tgt_loader, tgt_method, tgt_desc,
          src_ok, tgt_ok, exact ? nullptr : reason)) {
        fprintf(stderr, "[soroush] record_storage_oom: generic callsite lost src=%s.%s bci=%d\n",
                src_class ? src_class : "?", src_method ? src_method : "?", src_bci);
      }
    }

    // ---- Part B: sibling BCI scan ----
    // Emit diagnostics for all sibling BCIs in the user method that share the
    // same CP constant pool index but will never trigger resolve_handle_call.
    // Only for non-reflection, non-jli-internal attributions where we have the
    // user method available.
    if (src_ok && src_cp >= 0 && !is_reflection_accessor) {
      Method* sibling_method = recv_frame_valid  ? recv_frame_method
                             : (top_frame_valid  ? top.interpreter_frame_method()
                                                 : nullptr);
      sg_emit_sibling_bcis(cat, sibling_method, src_bci, primary_recv_local,
                           src_cp, src_class, src_loader, src_method, src_desc,
                           primary_arg_slots);
    }
  }

  JFR_ONLY(Jfr::on_resolution(result, CHECK);)
}

void LinkResolver::resolve_invokedynamic(CallInfo& result, const constantPoolHandle& pool, int indy_index, TRAPS) {
  int index = pool->decode_invokedynamic_index(indy_index);
  int pool_index = pool->resolved_indy_entry_at(index)->constant_pool_index();

  // Resolve the bootstrap specifier (BSM + optional arguments).
  BootstrapInfo bootstrap_specifier(pool, pool_index, index);

  // Check if CallSite has been bound already or failed already, and short circuit:
  {
    bool is_done = bootstrap_specifier.resolve_previously_linked_invokedynamic(result, CHECK);
    if (is_done) return;
  }

  // The initial step in Call Site Specifier Resolution is to resolve the symbolic
  // reference to a method handle which will be the bootstrap method for a dynamic
  // call site.  If resolution for the java.lang.invoke.MethodHandle for the bootstrap
  // method fails, then a MethodHandleInError is stored at the corresponding bootstrap
  // method's CP index for the CONSTANT_MethodHandle_info.
  // Any subsequent invokedynamic instruction which shares
  // this bootstrap method will encounter the resolution of MethodHandleInError.

  resolve_dynamic_call(result, bootstrap_specifier, CHECK);

  LogTarget(Debug, methodhandles, indy) lt_indy;
  if (lt_indy.is_enabled()) {
    LogStream ls(lt_indy);
    bootstrap_specifier.print_msg_on(&ls, "resolve_invokedynamic");
  }

  // The returned linkage result is provisional up to the moment
  // the interpreter or runtime performs a serialized check of
  // the relevant ResolvedIndyEntry::method field.  This is done by the caller
  // of this method, via CPC::set_dynamic_call, which uses
  // an ObjectLocker to do the final serialization of updates
  // to ResolvedIndyEntry state, including method.

  // Log dynamic info to CDS classlist.
  ArchiveUtils::log_to_classlist(&bootstrap_specifier, CHECK);
}

void LinkResolver::resolve_dynamic_call(CallInfo& result,
                                        BootstrapInfo& bootstrap_specifier,
                                        TRAPS) {
  // JSR 292:  this must resolve to an implicitly generated method
  // such as MH.linkToCallSite(*...) or some other call-site shape.
  // The appendix argument is likely to be a freshly-created CallSite.
  // It may also be a MethodHandle from an unwrapped ConstantCallSite,
  // or any other reference.  The resolved_method as well as the appendix
  // are both recorded together via CallInfo::set_handle.
  SystemDictionary::invoke_bootstrap_method(bootstrap_specifier, THREAD);
  Exceptions::wrap_dynamic_exception(/* is_indy */ true, THREAD);

  if (HAS_PENDING_EXCEPTION) {
    if (!PENDING_EXCEPTION->is_a(vmClasses::LinkageError_klass())) {
      // Let any random low-level IE or SOE or OOME just bleed through.
      // Basically we pretend that the bootstrap method was never called,
      // if it fails this way:  We neither record a successful linkage,
      // nor do we memorize a LE for posterity.
      return;
    }
    // JVMS 5.4.3 says: If an attempt by the Java Virtual Machine to resolve
    // a symbolic reference fails because an error is thrown that is an
    // instance of LinkageError (or a subclass), then subsequent attempts to
    // resolve the reference always fail with the same error that was thrown
    // as a result of the initial resolution attempt.
    bool recorded_res_status = bootstrap_specifier.save_and_throw_indy_exc(CHECK);
    if (!recorded_res_status) {
      // Another thread got here just before we did.  So, either use the method
      // that it resolved or throw the LinkageError exception that it threw.
      bool is_done = bootstrap_specifier.resolve_previously_linked_invokedynamic(result, CHECK);
      if (is_done) return;
    }
    assert(bootstrap_specifier.pool()->resolved_indy_entry_at(bootstrap_specifier.indy_index())->resolution_failed(),
          "Resolution should have failed");
  }

  bootstrap_specifier.resolve_newly_linked_invokedynamic(result, CHECK);
  // Exceptions::wrap_dynamic_exception not used because
  // set_handle doesn't throw linkage errors
  JFR_ONLY(Jfr::on_resolution(result, CHECK);)
}

// Selected method is abstract.
void LinkResolver::throw_abstract_method_error(const methodHandle& resolved_method,
                                               const methodHandle& selected_method,
                                               Klass *recv_klass, TRAPS) {
  Klass *resolved_klass = resolved_method->method_holder();
  ResourceMark rm(THREAD);
  stringStream ss;

  if (recv_klass != nullptr) {
    ss.print("Receiver class %s does not define or inherit an "
             "implementation of the",
             recv_klass->external_name());
  } else {
    ss.print("Missing implementation of");
  }

  assert(resolved_method.not_null(), "Sanity");
  ss.print(" resolved method '%s%s",
           resolved_method->is_abstract() ? "abstract " : "",
           resolved_method->is_private()  ? "private "  : "");
  resolved_method->signature()->print_as_signature_external_return_type(&ss);
  ss.print(" %s(", resolved_method->name()->as_C_string());
  resolved_method->signature()->print_as_signature_external_parameters(&ss);
  ss.print(")' of %s %s.",
           resolved_klass->external_kind(),
           resolved_klass->external_name());

  if (selected_method.not_null() && !(resolved_method == selected_method)) {
    ss.print(" Selected method is '%s%s",
             selected_method->is_abstract() ? "abstract " : "",
             selected_method->is_private()  ? "private "  : "");
    selected_method->print_external_name(&ss);
    ss.print("'.");
  }

  THROW_MSG(vmSymbols::java_lang_AbstractMethodError(), ss.as_string());
}
