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

  static RoundTripResult roundtrip_copy(const u1* bytes, int length);
  static void free_roundtrip(RoundTripResult* result);
};

#endif // SHARE_CLASSFILE_SOROUSHCLASSFILEREWRITER_HPP
