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
  // The trace inserters use the exact method-token ABI: each instrumented method
  // is assigned a stable token (registered with its class/name/descriptor and the
  // given loader_id/hidden/artifact_crc) that is baked into the injected
  // System.soroushTraceEnter/Exit(int) calls. loader_id is the defining
  // ClassLoaderData pointer (0 if unknown), artifact_crc the original-bytes crc.
  static TransformResult insert_entry_trace(const u1* bytes, int length, const char* class_name,
                                            uint64_t loader_id, int hidden, uint32_t artifact_crc);
  static TransformResult insert_entry_exit_trace(const u1* bytes, int length, const char* class_name,
                                                 uint64_t loader_id, int hidden, uint32_t artifact_crc);
  static void free_transform(TransformResult* result);
};

#endif // SHARE_CLASSFILE_SOROUSHCLASSFILEREWRITER_HPP
