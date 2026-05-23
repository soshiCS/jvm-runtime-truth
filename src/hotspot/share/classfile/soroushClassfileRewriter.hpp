/*
 * Phase 1 infrastructure for Soroush runtime graph classfile rewriting.
 *
 * This module is intentionally non-mutating for now: it parses a classfile,
 * decodes method bytecode, and can emit an identical byte-for-byte copy.
 * Later phases should build verifier-safe instrumentation on top of this
 * structured parser rather than extending the ad hoc copier in klassFactory.cpp.
 */

#ifndef SHARE_CLASSFILE_SOROUSHCLASSFILEREWRITER_HPP
#define SHARE_CLASSFILE_SOROUSHCLASSFILEREWRITER_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class SoroushClassfileRewriter : AllStatic {
 public:
  struct RoundTripResult {
    bool ok;
    u1* bytes;
    int length;
    const char* error;
    int decoded_methods;
    int decoded_instructions;
  };

  struct TransformResult {
    bool ok;
    u1* bytes;
    int length;
    const char* error;
    int transformed_methods;
    int decoded_instructions;
    int code_methods;          // methods with a Code attribute (considered)
    int constructor_methods;   // <init>/<clinit> with Code, skipped by design
  };

  static RoundTripResult roundtrip_copy(const u1* bytes, int length);
  static void free_roundtrip(RoundTripResult* result);

  static TransformResult insert_entry_nops(const u1* bytes, int length, int nop_count);
  static TransformResult insert_entry_trace(const u1* bytes, int length, const char* class_name);
  static TransformResult insert_entry_exit_trace(const u1* bytes, int length, const char* class_name);
  static void free_transform(TransformResult* result);
};

#endif // SHARE_CLASSFILE_SOROUSHCLASSFILEREWRITER_HPP
