#include "precompiled.hpp"
#include "classfile/soroushClassfileRewriter.hpp"
#include <stdlib.h>
#include <string.h>

static u2 soroush_read_u2(const u1* p) {
  return ((u2)p[0] << 8) | p[1];
}

static u4 soroush_read_u4(const u1* p) {
  return ((u4)p[0] << 24) | ((u4)p[1] << 16) | ((u4)p[2] << 8) | p[3];
}

class SoroushClassReader {
  const u1* const _start;
  const u1* const _end;
  const u1* _p;
  const char* _error;

 public:
  SoroushClassReader(const u1* bytes, int length)
      : _start(bytes), _end(bytes + length), _p(bytes), _error(nullptr) {}

  const char* error() const { return _error; }
  bool ok() const { return _error == nullptr; }
  const u1* current() const { return _p; }

  bool require(int n, const char* error) {
    if (_error != nullptr) return false;
    if (n < 0 || _p + n < _p || _p + n > _end) {
      _error = error;
      return false;
    }
    return true;
  }

  u1 get_u1(const char* error) {
    if (!require(1, error)) return 0;
    return *_p++;
  }

  u2 get_u2(const char* error) {
    if (!require(2, error)) return 0;
    u2 value = soroush_read_u2(_p);
    _p += 2;
    return value;
  }

  u4 get_u4(const char* error) {
    if (!require(4, error)) return 0;
    u4 value = soroush_read_u4(_p);
    _p += 4;
    return value;
  }

  void skip(int n, const char* error) {
    if (require(n, error)) _p += n;
  }
};

struct SoroushCpInfo {
  u1 tag;
  const char* utf8;
  u2 utf8_len;
};

static bool soroush_utf8_equals(const SoroushCpInfo* cp,
                                u2 cp_count,
                                u2 index,
                                const char* value) {
  if (index == 0 || index >= cp_count || cp[index].tag != 1 || value == nullptr) {
    return false;
  }
  size_t len = strlen(value);
  return cp[index].utf8_len == len && memcmp(cp[index].utf8, value, len) == 0;
}

static int soroush_instruction_length(const u1* code,
                                      int code_len,
                                      int pc,
                                      const char** error) {
  if (pc < 0 || pc >= code_len) {
    *error = "bad bytecode pc";
    return -1;
  }

  u1 op = code[pc];
  switch (op) {
    case 0x10: return 2; // bipush
    case 0x11: return 3; // sipush
    case 0x12: return 2; // ldc
    case 0x13: // ldc_w
    case 0x14: // ldc2_w
    case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
    case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
    case 0xa5: case 0xa6: case 0xa7: case 0xa8:
    case 0xc6: case 0xc7:
    case 0xb2: case 0xb3: case 0xb4: case 0xb5:
    case 0xb6: case 0xb7: case 0xb8:
    case 0xbb: case 0xbd: case 0xc0: case 0xc1:
      return 3;
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
    case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:
    case 0xa9:
    case 0xbc:
      return 2;
    case 0x84:
      return 3;
    case 0xb9:
    case 0xba:
    case 0xc8:
    case 0xc9:
      return 5;
    case 0xc5:
      return 4;
    case 0xaa: { // tableswitch
      int p = pc + 1;
      while ((p & 3) != 0) p++;
      if (p + 12 > code_len) {
        *error = "truncated tableswitch";
        return -1;
      }
      int low = (int)soroush_read_u4(code + p + 4);
      int high = (int)soroush_read_u4(code + p + 8);
      if (high < low) {
        *error = "bad tableswitch bounds";
        return -1;
      }
      int entries = high - low + 1;
      if (entries > (code_len - (p + 12)) / 4) {
        *error = "truncated tableswitch entries";
        return -1;
      }
      return (p + 12 + entries * 4) - pc;
    }
    case 0xab: { // lookupswitch
      int p = pc + 1;
      while ((p & 3) != 0) p++;
      if (p + 8 > code_len) {
        *error = "truncated lookupswitch";
        return -1;
      }
      int pairs = (int)soroush_read_u4(code + p + 4);
      if (pairs < 0 || pairs > (code_len - (p + 8)) / 8) {
        *error = "truncated lookupswitch pairs";
        return -1;
      }
      return (p + 8 + pairs * 8) - pc;
    }
    case 0xc4: { // wide
      if (pc + 2 > code_len) {
        *error = "truncated wide";
        return -1;
      }
      u1 wide_op = code[pc + 1];
      if (wide_op == 0x84) return 6;
      if (wide_op == 0x15 || wide_op == 0x16 || wide_op == 0x17 ||
          wide_op == 0x18 || wide_op == 0x19 || wide_op == 0x36 ||
          wide_op == 0x37 || wide_op == 0x38 || wide_op == 0x39 ||
          wide_op == 0x3a || wide_op == 0xa9) {
        return 4;
      }
      *error = "bad wide opcode";
      return -1;
    }
    default:
      return 1;
  }
}

static bool soroush_decode_code(const u1* code,
                                int code_len,
                                int* decoded_instructions,
                                const char** error) {
  int pc = 0;
  while (pc < code_len) {
    int len = soroush_instruction_length(code, code_len, pc, error);
    if (len <= 0 || pc + len > code_len) {
      if (*error == nullptr) *error = "instruction exceeds code length";
      return false;
    }
    pc += len;
    (*decoded_instructions)++;
  }
  return pc == code_len;
}

static bool soroush_skip_member(SoroushClassReader* r,
                                const SoroushCpInfo* cp,
                                u2 cp_count,
                                int* decoded_methods,
                                int* decoded_instructions,
                                const char** error) {
  r->skip(2, "truncated member access_flags");
  r->skip(2, "truncated member name_index");
  r->skip(2, "truncated member descriptor_index");
  u2 attr_count = r->get_u2("truncated member attributes_count");
  if (!r->ok()) return false;

  for (u2 i = 0; i < attr_count; i++) {
    u2 attr_name = r->get_u2("truncated attribute name_index");
    u4 attr_len = r->get_u4("truncated attribute length");
    const u1* attr_body = r->current();
    r->skip((int)attr_len, "truncated attribute body");
    if (!r->ok()) return false;

    if (soroush_utf8_equals(cp, cp_count, attr_name, "Code")) {
      SoroushClassReader cr(attr_body, (int)attr_len);
      cr.skip(2, "truncated Code max_stack");
      cr.skip(2, "truncated Code max_locals");
      u4 code_len = cr.get_u4("truncated Code code_length");
      const u1* code = cr.current();
      cr.skip((int)code_len, "truncated Code bytes");
      if (!cr.ok()) {
        *error = cr.error();
        return false;
      }
      if (!soroush_decode_code(code, (int)code_len, decoded_instructions, error)) {
        return false;
      }
      (*decoded_methods)++;

      u2 exception_table_len = cr.get_u2("truncated exception table length");
      cr.skip(exception_table_len * 8, "truncated exception table");
      u2 code_attr_count = cr.get_u2("truncated Code attributes_count");
      for (u2 j = 0; j < code_attr_count; j++) {
        cr.skip(2, "truncated Code attribute name_index");
        u4 code_attr_len = cr.get_u4("truncated Code attribute length");
        cr.skip((int)code_attr_len, "truncated Code attribute body");
      }
      if (!cr.ok() || cr.current() != attr_body + attr_len) {
        *error = cr.ok() ? "Code attribute did not round-trip" : cr.error();
        return false;
      }
    }
  }

  return true;
}

SoroushClassfileRewriter::RoundTripResult
SoroushClassfileRewriter::roundtrip_copy(const u1* bytes, int length) {
  RoundTripResult result;
  result.ok = false;
  result.bytes = nullptr;
  result.length = 0;
  result.error = "not started";
  result.decoded_methods = 0;
  result.decoded_instructions = 0;

  if (bytes == nullptr || length <= 0) {
    result.error = "empty classfile";
    return result;
  }

  SoroushClassReader r(bytes, length);
  if (r.get_u4("truncated magic") != 0xCAFEBABE) {
    result.error = "bad classfile magic";
    return result;
  }
  r.skip(2, "truncated minor_version");
  r.skip(2, "truncated major_version");
  u2 cp_count = r.get_u2("truncated constant_pool_count");
  if (!r.ok()) {
    result.error = r.error();
    return result;
  }

  SoroushCpInfo* cp = (SoroushCpInfo*)calloc(cp_count, sizeof(SoroushCpInfo));
  if (cp == nullptr) {
    result.error = "out of memory";
    return result;
  }

  for (u2 i = 1; i < cp_count; i++) {
    u1 tag = r.get_u1("truncated constant pool tag");
    cp[i].tag = tag;
    switch (tag) {
      case 1: {
        u2 len = r.get_u2("truncated Utf8 length");
        cp[i].utf8 = (const char*)r.current();
        cp[i].utf8_len = len;
        r.skip(len, "truncated Utf8 bytes");
        break;
      }
      case 3:
      case 4:
        r.skip(4, "truncated constant pool entry");
        break;
      case 5:
      case 6:
        r.skip(8, "truncated wide constant pool entry");
        i++;
        break;
      case 7:
      case 8:
      case 16:
      case 19:
      case 20:
        r.skip(2, "truncated constant pool entry");
        break;
      case 9:
      case 10:
      case 11:
      case 12:
      case 17:
      case 18:
        r.skip(4, "truncated constant pool entry");
        break;
      case 15:
        r.skip(3, "truncated MethodHandle constant pool entry");
        break;
      default:
        free(cp);
        result.error = "unknown constant pool tag";
        return result;
    }
    if (!r.ok()) {
      free(cp);
      result.error = r.error();
      return result;
    }
  }

  r.skip(2, "truncated access_flags");
  r.skip(2, "truncated this_class");
  r.skip(2, "truncated super_class");

  u2 interfaces_count = r.get_u2("truncated interfaces_count");
  r.skip(interfaces_count * 2, "truncated interfaces");

  u2 fields_count = r.get_u2("truncated fields_count");
  for (u2 i = 0; i < fields_count && r.ok(); i++) {
    const char* error = nullptr;
    if (!soroush_skip_member(&r, cp, cp_count, &result.decoded_methods, &result.decoded_instructions, &error)) {
      free(cp);
      result.error = error == nullptr ? "bad field" : error;
      return result;
    }
  }

  u2 methods_count = r.get_u2("truncated methods_count");
  for (u2 i = 0; i < methods_count && r.ok(); i++) {
    const char* error = nullptr;
    if (!soroush_skip_member(&r, cp, cp_count, &result.decoded_methods, &result.decoded_instructions, &error)) {
      free(cp);
      result.error = error == nullptr ? "bad method" : error;
      return result;
    }
  }

  u2 class_attr_count = r.get_u2("truncated class attributes_count");
  for (u2 i = 0; i < class_attr_count && r.ok(); i++) {
    r.skip(2, "truncated class attribute name_index");
    u4 attr_len = r.get_u4("truncated class attribute length");
    r.skip((int)attr_len, "truncated class attribute body");
  }

  if (!r.ok()) {
    free(cp);
    result.error = r.error();
    return result;
  }
  if (r.current() != bytes + length) {
    free(cp);
    result.error = "classfile parser did not consume full input";
    return result;
  }

  u1* copy = (u1*)malloc(length);
  if (copy == nullptr) {
    free(cp);
    result.error = "out of memory";
    return result;
  }
  memcpy(copy, bytes, length);

  free(cp);
  result.ok = true;
  result.bytes = copy;
  result.length = length;
  result.error = nullptr;
  return result;
}

void SoroushClassfileRewriter::free_roundtrip(RoundTripResult* result) {
  if (result == nullptr) return;
  free(result->bytes);
  result->bytes = nullptr;
  result->length = 0;
}
