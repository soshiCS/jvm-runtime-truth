#include "precompiled.hpp"
#include "classfile/soroushClassfileRewriter.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u2 soroush_read_u2(const u1* p) {
  return ((u2)p[0] << 8) | p[1];
}

static u4 soroush_read_u4(const u1* p) {
  return ((u4)p[0] << 24) | ((u4)p[1] << 16) | ((u4)p[2] << 8) | p[3];
}

static void soroush_write_u2(u1* p, u2 v) {
  p[0] = (u1)(v >> 8);
  p[1] = (u1)v;
}

static void soroush_write_u4(u1* p, u4 v) {
  p[0] = (u1)(v >> 24);
  p[1] = (u1)(v >> 16);
  p[2] = (u1)(v >> 8);
  p[3] = (u1)v;
}

class SoroushByteWriter {
  u1* _buf;
  int _len;
  int _cap;
  bool _ok;

  bool grow(int needed) {
    if (!_ok) return false;
    if (needed <= _cap) return true;
    int new_cap = _cap == 0 ? 1024 : _cap;
    while (new_cap < needed) {
      if (new_cap > max_jint / 2) {
        _ok = false;
        return false;
      }
      new_cap *= 2;
    }
    u1* new_buf = (u1*)realloc(_buf, new_cap);
    if (new_buf == nullptr) {
      _ok = false;
      return false;
    }
    _buf = new_buf;
    _cap = new_cap;
    return true;
  }

 public:
  SoroushByteWriter() : _buf(nullptr), _len(0), _cap(0), _ok(true) {}
  ~SoroushByteWriter() { free(_buf); }

  bool ok() const { return _ok; }
  int length() const { return _len; }
  u1* bytes() const { return _buf; }

  void append_u1(u1 v) {
    if (!grow(_len + 1)) return;
    _buf[_len++] = v;
  }

  void append_u2(u2 v) {
    if (!grow(_len + 2)) return;
    soroush_write_u2(_buf + _len, v);
    _len += 2;
  }

  void append_u4(u4 v) {
    if (!grow(_len + 4)) return;
    soroush_write_u4(_buf + _len, v);
    _len += 4;
  }

  void append_s2(int v) {
    append_u2((u2)(u4)v);
  }

  void append_s4(int v) {
    append_u4((u4)v);
  }

  void append_bytes(const u1* bytes, int len) {
    if (len < 0 || bytes == nullptr) {
      _ok = false;
      return;
    }
    if (!grow(_len + len)) return;
    memcpy(_buf + _len, bytes, len);
    _len += len;
  }

  void patch_u4(int pos, u4 v) {
    if (pos < 0 || pos + 4 > _len) {
      _ok = false;
      return;
    }
    soroush_write_u4(_buf + pos, v);
  }

  void patch_u2(int pos, u2 v) {
    if (pos < 0 || pos + 2 > _len) {
      _ok = false;
      return;
    }
    soroush_write_u2(_buf + pos, v);
  }

  u1* release() {
    u1* result = _buf;
    _buf = nullptr;
    _len = 0;
    _cap = 0;
    return result;
  }
};

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
  u2 index1; // Class: name_index; Fieldref/Methodref/InterfaceMethodref: class_index; NameAndType: name_index
  u2 index2; // Fieldref/Methodref/InterfaceMethodref: name_and_type_index; NameAndType: descriptor_index
};

struct SoroushInstruction {
  int old_pc;
  int old_len;
  int new_pc;
  int new_len;
  u1 op;
  bool widened; // set by layout convergence: this branch is emitted in its _w form
};

struct SoroushMethodTracePlan {
  bool rewrite;
  bool is_constructor; // <init>: ENTER must go after super()/this() delegation
  u2 enter_string_index;
  u2 exit_string_index;
  char enter_msg[512];
  char exit_msg[512];
};

struct SoroushVerificationType {
  u1 tag;
  u2 data;
};

struct SoroushExceptionExitHandler {
  int old_start_pc;
  int old_end_pc;
  int handler_pc;
  SoroushVerificationType* locals;
  int locals_count;
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

// Resolve an invokespecial Methodref cp index: returns true iff it names an
// instance constructor "<init>". Sets *is_this_delegation when the target
// class is this class (i.e. this(...) chaining) vs the superclass (super(...)).
// Requires the cp index fields populated (soroush_insert_trace path only).
static bool soroush_methodref_is_init(const SoroushCpInfo* cp,
                                      u2 cp_count,
                                      u2 methodref_index,
                                      u2 this_class_index,
                                      bool* is_this_delegation) {
  if (methodref_index == 0 || methodref_index >= cp_count) return false;
  if (cp[methodref_index].tag != 10) return false; // Methodref
  u2 class_index = cp[methodref_index].index1;
  u2 nt_index = cp[methodref_index].index2;
  if (nt_index == 0 || nt_index >= cp_count || cp[nt_index].tag != 12) return false;
  u2 name_index = cp[nt_index].index1;
  if (!soroush_utf8_equals(cp, cp_count, name_index, "<init>")) return false;
  *is_this_delegation = (class_index == this_class_index);
  return true;
}

// The complete set of standard attributes that may appear inside a method's
// Code attribute (JVMS Table 4.7-C). Every one of them carries bytecode PCs or
// verifier-sensitive metadata and is remapped explicitly by this rewriter:
//   StackMapTable                 -> remapped (offsets + synthesized frames)
//   LineNumberTable               -> remapped (start_pc)
//   LocalVariableTable            -> remapped (start_pc + length)
//   LocalVariableTypeTable        -> remapped (start_pc + length)
//   RuntimeVisibleTypeAnnotations -> remapped (offset/localvar targets)
//   RuntimeInvisibleTypeAnnotations-> remapped (offset/localvar targets)
// Any OTHER Code sub-attribute is non-standard. Because instrumentation shifts
// bytecode PCs, silently copying an unknown attribute could leave stale PCs, so
// the caller must conservatively safe-skip rather than copy it unchanged.
static bool soroush_is_known_code_attr(const SoroushCpInfo* cp, u2 cp_count, u2 name_index) {
  return soroush_utf8_equals(cp, cp_count, name_index, "StackMapTable") ||
         soroush_utf8_equals(cp, cp_count, name_index, "LineNumberTable") ||
         soroush_utf8_equals(cp, cp_count, name_index, "LocalVariableTable") ||
         soroush_utf8_equals(cp, cp_count, name_index, "LocalVariableTypeTable") ||
         soroush_utf8_equals(cp, cp_count, name_index, "RuntimeVisibleTypeAnnotations") ||
         soroush_utf8_equals(cp, cp_count, name_index, "RuntimeInvisibleTypeAnnotations");
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

static int soroush_read_s2(const u1* p) {
  return (int)(int16_t)soroush_read_u2(p);
}

static int soroush_read_s4(const u1* p) {
  return (int)(int32_t)soroush_read_u4(p);
}

static int soroush_switch_padding(int pc) {
  int p = pc + 1;
  int aligned = (p + 3) & ~3;
  return aligned - p;
}

static bool soroush_is_return_opcode(u1 op) {
  return op >= 0xac && op <= 0xb1;
}

static int soroush_local_access_len(int local_index) {
  return local_index <= 255 ? 2 : 4;
}

static int soroush_exit_prefix_len(u1 op, int temp_local, bool instrument_athrow) {
  if (op == 0xbf) {
    // athrow EXIT (exception-path tracing) is disabled for constructors.
    return instrument_athrow ? (soroush_local_access_len(temp_local) + 6 + soroush_local_access_len(temp_local)) : 0;
  }
  if (!soroush_is_return_opcode(op)) return 0;
  if (op == 0xb1) return 6;
  return soroush_local_access_len(temp_local) + 6 + soroush_local_access_len(temp_local);
}

static int soroush_exception_exit_handler_len(int temp_local) {
  return soroush_local_access_len(temp_local) + 6 + soroush_local_access_len(temp_local) + 1;
}

static void soroush_append_local_access(SoroushByteWriter* out, u1 op, int local_index) {
  if (local_index <= 255) {
    out->append_u1(op);
    out->append_u1((u1)local_index);
  } else {
    out->append_u1(0xc4); // wide
    out->append_u1(op);
    out->append_u2((u2)local_index);
  }
}

static bool soroush_append_exit_prefix(SoroushByteWriter* out,
                                       u1 return_op,
                                       int temp_local,
                                       u2 exit_string_index,
                                       u2 trace_methodref,
                                       const char** error) {
  switch (return_op) {
    case 0xac: soroush_append_local_access(out, 0x36, temp_local); break; // istore
    case 0xad: soroush_append_local_access(out, 0x37, temp_local); break; // lstore
    case 0xae: soroush_append_local_access(out, 0x38, temp_local); break; // fstore
    case 0xaf: soroush_append_local_access(out, 0x39, temp_local); break; // dstore
    case 0xb0: soroush_append_local_access(out, 0x3a, temp_local); break; // astore
    case 0xb1: break;
    case 0xbf: soroush_append_local_access(out, 0x3a, temp_local); break; // astore
    default:
      *error = "not a return or athrow opcode";
      return false;
  }

  out->append_u1(0x13); // ldc_w
  out->append_u2(exit_string_index);
  out->append_u1(0xb8); // invokestatic
  out->append_u2(trace_methodref);

  switch (return_op) {
    case 0xac: soroush_append_local_access(out, 0x15, temp_local); break; // iload
    case 0xad: soroush_append_local_access(out, 0x16, temp_local); break; // lload
    case 0xae: soroush_append_local_access(out, 0x17, temp_local); break; // fload
    case 0xaf: soroush_append_local_access(out, 0x18, temp_local); break; // dload
    case 0xb0: soroush_append_local_access(out, 0x19, temp_local); break; // aload
    case 0xb1: break;
    case 0xbf: soroush_append_local_access(out, 0x19, temp_local); break; // aload
  }
  if (!out->ok()) {
    *error = "out of memory while emitting EXIT prefix";
    return false;
  }
  return true;
}

static bool soroush_append_exception_exit_handler(SoroushByteWriter* out,
                                                  int temp_local,
                                                  u2 exit_string_index,
                                                  u2 trace_methodref,
                                                  const char** error) {
  soroush_append_local_access(out, 0x3a, temp_local); // astore
  out->append_u1(0x13); // ldc_w
  out->append_u2(exit_string_index);
  out->append_u1(0xb8); // invokestatic
  out->append_u2(trace_methodref);
  soroush_append_local_access(out, 0x19, temp_local); // aload
  out->append_u1(0xbf); // athrow
  if (!out->ok()) {
    *error = "out of memory while emitting exception EXIT handler";
    return false;
  }
  return true;
}

static bool soroush_build_pc_map(const u1* code,
                                 int code_len,
                                 int entry_len,
                                 bool insert_exits,
                                 int temp_local,
                                 bool is_constructor,
                                 int enter_after_old_pc,
                                 SoroushInstruction** instructions_out,
                                 int* instruction_count_out,
                                 int** pc_map_out,
                                 int* new_code_len_out,
                                 int* decoded_instructions,
                                 const char** error) {
  SoroushInstruction* instructions =
      (SoroushInstruction*)calloc(code_len + 1, sizeof(SoroushInstruction));
  int* pc_map = (int*)malloc((code_len + 1) * sizeof(int));
  if (instructions == nullptr || pc_map == nullptr) {
    free(instructions);
    free(pc_map);
    *error = "out of memory while building pc map";
    return false;
  }
  for (int i = 0; i <= code_len; i++) {
    pc_map[i] = -1;
  }

  // Normal methods prepend ENTER at pc 0. Constructors instead insert ENTER
  // right AFTER the delegation invokespecial (enter_after_old_pc), because
  // `this` is uninitializedThis until super()/this() returns.
  bool instrument_athrow = !is_constructor;
  int old_pc = 0;
  int new_pc = is_constructor ? 0 : entry_len;
  int count = 0;
  while (old_pc < code_len) {
    int old_len = soroush_instruction_length(code, code_len, old_pc, error);
    if (old_len <= 0 || old_pc + old_len > code_len) {
      if (*error == nullptr) *error = "instruction exceeds code length";
      free(instructions);
      free(pc_map);
      return false;
    }

    u1 op = code[old_pc];
    int prefix_len = insert_exits ? soroush_exit_prefix_len(op, temp_local, instrument_athrow) : 0;
    int new_len = prefix_len + old_len;
    if (op == 0xaa) {
      int old_p = old_pc + 1;
      while ((old_p & 3) != 0) old_p++;
      int low = soroush_read_s4(code + old_p + 4);
      int high = soroush_read_s4(code + old_p + 8);
      int entries = high - low + 1;
      new_len = prefix_len + 1 + soroush_switch_padding(new_pc + prefix_len) + 12 + entries * 4;
    } else if (op == 0xab) {
      int old_p = old_pc + 1;
      while ((old_p & 3) != 0) old_p++;
      int pairs = soroush_read_s4(code + old_p + 4);
      new_len = prefix_len + 1 + soroush_switch_padding(new_pc + prefix_len) + 8 + pairs * 8;
    }

    instructions[count].old_pc = old_pc;
    instructions[count].old_len = old_len;
    instructions[count].new_pc = new_pc;
    instructions[count].new_len = new_len;
    instructions[count].op = op;
    pc_map[old_pc] = new_pc;

    new_pc += new_len;
    if (is_constructor && old_pc == enter_after_old_pc) {
      new_pc += entry_len; // ENTER bytes occupy [delegation_end, +entry_len)
    }
    old_pc += old_len;
    count++;
    (*decoded_instructions)++;
  }
  pc_map[code_len] = new_pc;

  *instructions_out = instructions;
  *instruction_count_out = count;
  *pc_map_out = pc_map;
  *new_code_len_out = new_pc;
  return true;
}

static bool soroush_remap_pc(int old_pc,
                             int code_len,
                             const int* pc_map,
                             int* new_pc,
                             const char** error) {
  if (old_pc < 0 || old_pc > code_len || pc_map[old_pc] < 0) {
    *error = "branch or metadata target is not an instruction boundary";
    return false;
  }
  *new_pc = pc_map[old_pc];
  return true;
}

static bool soroush_append_s2_checked(SoroushByteWriter* out,
                                      int value,
                                      const char** error) {
  if (value < -32768 || value > 32767) {
    *error = "rewritten branch offset exceeds s2 range";
    return false;
  }
  out->append_s2(value);
  return out->ok();
}

// ===========================================================================
// Phase 6A: branch widening detection (design + diagnostics only).
//
// Once soroush_build_pc_map has assigned a new pc to every instruction, the
// offset of each short branch (if<cond>, ifnull, ifnonnull, goto, jsr) must
// be recomputed against the new pcs. Inserting ENTER and per-return EXIT
// prefixes can push a target past the signed 16-bit boundary even when the
// original javac-emitted branch fit comfortably in s2.
//
// Phase 6A only DETECTS and CLASSIFIES such overflows. The rewriter continues
// to fail safely -- the surrounding caller falls back to the original,
// untransformed classfile (see klassFactory.cpp PHASE5/PHASE3 paths). Phase
// 6B will perform the actual widening on the classified candidates.
//
// Classification of an overflowing short branch:
//   * goto (0xa7) -> goto_w (0xc8): rewrite in place. 5 bytes vs 3, no
//     semantic change. SOROUSH_BRANCH_WIDEN_DIRECT.
//   * jsr  (0xa8) -> jsr_w  (0xc9): rewrite in place. Same shape.
//     SOROUSH_BRANCH_WIDEN_DIRECT.
//   * if<cond> (0x99..0xa6), ifnull (0xc6), ifnonnull (0xc7): the conditional
//     itself only has an s2 operand. Phase 6B will invert the condition and
//     have it skip past a goto_w whose s4 operand carries the real target.
//     Pattern (8 bytes total, +5 vs the original 3-byte conditional):
//         <inverted_if> +8        ; if the inverted cond holds, skip ahead
//         goto_w   <real_target>
//     SOROUSH_BRANCH_WIDEN_INVERT.
//
// Phase 6B inversion table (kept here for reference and to make Phase 6B a
// pure code change):
//   0x99 ifeq      <-> 0x9a ifne
//   0x9b iflt      <-> 0x9c ifge
//   0x9d ifgt      <-> 0x9e ifle
//   0x9f if_icmpeq <-> 0xa0 if_icmpne
//   0xa1 if_icmplt <-> 0xa2 if_icmpge
//   0xa3 if_icmpgt <-> 0xa4 if_icmple
//   0xa5 if_acmpeq <-> 0xa6 if_acmpne
//   0xc6 ifnull    <-> 0xc7 ifnonnull
//
// Switches (0xaa tableswitch, 0xab lookupswitch) already use 32-bit offsets,
// so they cannot overflow the same way and are intentionally out of scope.
// ===========================================================================

enum SoroushBranchWideningKind {
  SOROUSH_BRANCH_OK = 0,
  SOROUSH_BRANCH_WIDEN_DIRECT,
  SOROUSH_BRANCH_WIDEN_INVERT,
};

struct SoroushBranchWideningCandidate {
  int new_pc;        // pc of the branch instruction in the rewritten code
  int new_target;    // absolute target pc in the rewritten code
  int new_offset;    // new_target - new_pc; outside [-32768, 32767]
  int old_pc;        // original pc, for diagnostics only
  u1  op;
  SoroushBranchWideningKind kind;
};

static const char* soroush_branch_op_name(u1 op) {
  switch (op) {
    case 0x99: return "ifeq";
    case 0x9a: return "ifne";
    case 0x9b: return "iflt";
    case 0x9c: return "ifge";
    case 0x9d: return "ifgt";
    case 0x9e: return "ifle";
    case 0x9f: return "if_icmpeq";
    case 0xa0: return "if_icmpne";
    case 0xa1: return "if_icmplt";
    case 0xa2: return "if_icmpge";
    case 0xa3: return "if_icmpgt";
    case 0xa4: return "if_icmple";
    case 0xa5: return "if_acmpeq";
    case 0xa6: return "if_acmpne";
    case 0xa7: return "goto";
    case 0xa8: return "jsr";
    case 0xc6: return "ifnull";
    case 0xc7: return "ifnonnull";
    default:   return "?";
  }
}

static bool soroush_is_short_branch_op(u1 op) {
  return (op >= 0x99 && op <= 0xa8) || op == 0xc6 || op == 0xc7;
}

static SoroushBranchWideningKind soroush_classify_branch_widening(u1 op) {
  // goto / jsr are unconditional and can be widened to their _w counterparts
  // directly. Everything else in soroush_is_short_branch_op is conditional
  // and needs the inversion-plus-goto_w pattern in Phase 6B.
  if (op == 0xa7 || op == 0xa8) return SOROUSH_BRANCH_WIDEN_DIRECT;
  return SOROUSH_BRANCH_WIDEN_INVERT;
}

// Scan the rewritten instruction stream and collect every short branch whose
// recomputed offset no longer fits in s2. Caller frees *candidates_out with
// free(). Returns false only on a hard parse/remap error; an empty result
// (zero candidates) is the normal "no widening required" case.
static bool soroush_detect_branch_widening(const u1* code,
                                           int code_len,
                                           const SoroushInstruction* instructions,
                                           int instruction_count,
                                           const int* pc_map,
                                           SoroushBranchWideningCandidate** candidates_out,
                                           int* candidate_count_out,
                                           int* direct_out,
                                           int* invert_out,
                                           const char** error) {
  *candidates_out = nullptr;
  *candidate_count_out = 0;
  *direct_out = 0;
  *invert_out = 0;

  SoroushBranchWideningCandidate* candidates = nullptr;
  int count = 0;
  int capacity = 0;

  for (int i = 0; i < instruction_count; i++) {
    const SoroushInstruction* ins = &instructions[i];
    u1 op = ins->op;
    if (!soroush_is_short_branch_op(op)) continue;

    int old_target = ins->old_pc + soroush_read_s2(code + ins->old_pc + 1);
    int new_target = 0;
    if (!soroush_remap_pc(old_target, code_len, pc_map, &new_target, error)) {
      free(candidates);
      return false;
    }
    int new_offset = new_target - ins->new_pc;
    if (new_offset >= -32768 && new_offset <= 32767) continue;

    if (count >= capacity) {
      int new_capacity = capacity == 0 ? 8 : capacity * 2;
      SoroushBranchWideningCandidate* resized =
          (SoroushBranchWideningCandidate*)realloc(
              candidates,
              new_capacity * sizeof(SoroushBranchWideningCandidate));
      if (resized == nullptr) {
        *error = "out of memory while collecting branch widening candidates";
        free(candidates);
        return false;
      }
      candidates = resized;
      capacity = new_capacity;
    }

    SoroushBranchWideningCandidate* c = &candidates[count++];
    c->new_pc = ins->new_pc;
    c->new_target = new_target;
    c->new_offset = new_offset;
    c->old_pc = ins->old_pc;
    c->op = op;
    c->kind = soroush_classify_branch_widening(op);
    if (c->kind == SOROUSH_BRANCH_WIDEN_DIRECT) {
      (*direct_out)++;
    } else {
      (*invert_out)++;
    }
  }

  *candidates_out = candidates;
  *candidate_count_out = count;
  return true;
}

static void soroush_log_branch_widening(const SoroushBranchWideningCandidate* candidates,
                                        int count,
                                        int direct,
                                        int invert) {
  for (int i = 0; i < count; i++) {
    const SoroushBranchWideningCandidate* c = &candidates[i];
    fprintf(stderr,
            "[JVM REWRITER WIDEN] op=0x%02x name=%s old_pc=%d new_pc=%d target=%d offset=%d kind=%s\n",
            c->op,
            soroush_branch_op_name(c->op),
            c->old_pc,
            c->new_pc,
            c->new_target,
            c->new_offset,
            c->kind == SOROUSH_BRANCH_WIDEN_DIRECT ? "direct" : "invert");
  }
  fprintf(stderr,
          "[JVM REWRITER WIDEN] summary candidates=%d direct=%d invert=%d "
          "(Phase 6A detection only; widening not yet implemented, falling back safely)\n",
          count, direct, invert);
}

// ===========================================================================
// Phase 6B precondition: layout convergence (fixed-point) analysis.
//
// The current size/PC computation (soroush_build_pc_map) is a single forward
// pass: every instruction's length is fixed (prefix_len + old_len), and the
// only layout-dependent term -- switch padding -- is resolvable in that one
// pass because it depends solely on *preceding* instruction sizes.
//
// Branch widening breaks that assumption. Widening an instruction changes its
// size, which shifts every later instruction, which can push another branch
// past the s2 boundary, which triggers further widening. A branch's size thus
// depends on the full layout, so a single pass cannot produce stable PCs --
// an iterative fixed-point relaxation is required.
//
// This pass MODELS widening to prove the layout converges to stable PCs and
// sizes, and reports diagnostics, WITHOUT emitting any widened instructions.
// Widening is monotonic (a short branch only ever grows to its _w form and is
// never un-widened) and the widened set is bounded by the branch count, so the
// iteration is guaranteed to terminate; we still cap iterations and fail safely
// if a (theoretically impossible) non-convergence is observed.
//
// Per-instruction size delta when widened:
//   * goto (0xa7) -> goto_w (0xc8): +2 bytes
//   * jsr  (0xa8) -> jsr_w  (0xc9): +2 bytes
//   * conditional (if<cond>/ifnull/ifnonnull) -> inverted conditional that
//     skips a goto_w: 3 -> 8 bytes, i.e. +5 bytes
// ===========================================================================

static int soroush_branch_widen_delta(u1 op) {
  if (op == 0xa7 || op == 0xa8) return 2; // goto/jsr -> goto_w/jsr_w
  return 5;                               // if<cond> -> inverted if + goto_w
}

// Inversion table for conditional branches (Phase 6C). Returns the inverted
// opcode, or 0 if `op` is not an invertible conditional. The widened form of a
// conditional is: <inverted_if> over an inserted goto_w that carries the real
// (wide) target -- so the inverted condition reproduces the original
// fall-through, and the goto_w reproduces the original taken branch.
static u1 soroush_invert_conditional_op(u1 op) {
  switch (op) {
    case 0x99: return 0x9a; // ifeq      <-> ifne
    case 0x9a: return 0x99;
    case 0x9b: return 0x9c; // iflt      <-> ifge
    case 0x9c: return 0x9b;
    case 0x9d: return 0x9e; // ifgt      <-> ifle
    case 0x9e: return 0x9d;
    case 0x9f: return 0xa0; // if_icmpeq <-> if_icmpne
    case 0xa0: return 0x9f;
    case 0xa1: return 0xa2; // if_icmplt <-> if_icmpge
    case 0xa2: return 0xa1;
    case 0xa3: return 0xa4; // if_icmpgt <-> if_icmple
    case 0xa4: return 0xa3;
    case 0xa5: return 0xa6; // if_acmpeq <-> if_acmpne
    case 0xa6: return 0xa5;
    case 0xc6: return 0xc7; // ifnull    <-> ifnonnull
    case 0xc7: return 0xc6;
    default:   return 0;
  }
}

struct SoroushLayoutSlot {
  int  old_pc;
  int  old_target;   // absolute old target pc (branches only)
  int  base_no_pad;  // size excluding switch padding and any widen delta
  int  new_pc;       // current modeled pc
  int  new_len;      // current modeled length (incl. switch padding / widen delta)
  u1   op;
  bool is_branch;    // short branch (if<cond>/goto/jsr/ifnull/ifnonnull)
  bool is_switch;    // tableswitch / lookupswitch (padding-bearing)
  bool widened;      // marked for widening this run (monotonic)
};

// Build the per-instruction layout model used by the convergence loop. The
// base length excludes switch padding (recomputed each iteration from the
// current pc) and any widen delta (added when a branch is marked widened).
static bool soroush_init_layout_slots(const u1* code,
                                      int code_len,
                                      bool insert_exits,
                                      int temp_local,
                                      bool is_constructor,
                                      const SoroushInstruction* instructions,
                                      int instruction_count,
                                      SoroushLayoutSlot* slots,
                                      const char** error) {
  bool instrument_athrow = !is_constructor;
  for (int i = 0; i < instruction_count; i++) {
    const SoroushInstruction* ins = &instructions[i];
    u1 op = ins->op;
    SoroushLayoutSlot* s = &slots[i];
    s->old_pc = ins->old_pc;
    s->old_target = 0;
    s->op = op;
    s->is_branch = false;
    s->is_switch = false;
    s->widened = false;

    int prefix_len = insert_exits ? soroush_exit_prefix_len(op, temp_local, instrument_athrow) : 0;
    if (op == 0xaa) { // tableswitch
      int old_p = ins->old_pc + 1;
      while ((old_p & 3) != 0) old_p++;
      int low = soroush_read_s4(code + old_p + 4);
      int high = soroush_read_s4(code + old_p + 8);
      int entries = high - low + 1;
      s->base_no_pad = prefix_len + 1 + 12 + entries * 4;
      s->is_switch = true;
    } else if (op == 0xab) { // lookupswitch
      int old_p = ins->old_pc + 1;
      while ((old_p & 3) != 0) old_p++;
      int pairs = soroush_read_s4(code + old_p + 4);
      s->base_no_pad = prefix_len + 1 + 8 + pairs * 8;
      s->is_switch = true;
    } else {
      s->base_no_pad = prefix_len + ins->old_len;
    }

    if (soroush_is_short_branch_op(op)) {
      s->is_branch = true;
      s->old_target = ins->old_pc + soroush_read_s2(code + ins->old_pc + 1);
    }
  }
  (void)code_len;
  (void)error;
  return true;
}

// Iteratively relax the layout until branch sizes and PCs stabilize. Models
// widening only -- emits nothing. On convergence the stabilized layout is
// written back into instructions[] (new_pc / new_len / widened) and pc_map[]
// so that emission and metadata remapping share one stable layout. Reports
// convergence status, widened branch counts (direct/invert), iteration count,
// and the final code length.
static bool soroush_converge_layout(const u1* code,
                                    int code_len,
                                    int entry_len,
                                    bool insert_exits,
                                    int temp_local,
                                    bool is_constructor,
                                    int enter_after_old_pc,
                                    SoroushInstruction* instructions,
                                    int instruction_count,
                                    int* pc_map,
                                    bool* converged_out,
                                    int* widened_out,
                                    int* direct_out,
                                    int* invert_out,
                                    int* iterations_out,
                                    int* final_len_out,
                                    const char** error) {
  *converged_out = false;
  *widened_out = 0;
  *direct_out = 0;
  *invert_out = 0;
  *iterations_out = 0;
  *final_len_out = 0;

  if (instruction_count <= 0) {
    *converged_out = true;
    return true;
  }

  SoroushLayoutSlot* slots =
      (SoroushLayoutSlot*)calloc(instruction_count, sizeof(SoroushLayoutSlot));
  int* map = (int*)malloc((code_len + 1) * sizeof(int));
  if (slots == nullptr || map == nullptr) {
    free(slots);
    free(map);
    *error = "out of memory while building layout convergence model";
    return false;
  }
  if (!soroush_init_layout_slots(code, code_len, insert_exits, temp_local, is_constructor,
                                 instructions, instruction_count, slots, error)) {
    free(slots);
    free(map);
    return false;
  }

  int branch_count = 0;
  for (int i = 0; i < instruction_count; i++) {
    if (slots[i].is_branch) branch_count++;
  }
  // A branch widens at most once, so the widened set grows by >=1 on every
  // non-terminal iteration; branch_count + 2 bounds the loop with margin.
  int cap = branch_count + 2;

  int start_pc = is_constructor ? 0 : entry_len;
  int widened_total = 0;
  int total_len = start_pc;
  bool converged = false;
  int iter = 0;
  while (iter < cap) {
    iter++;

    // Layout sweep: assign new pcs from current modeled sizes. Constructors
    // insert ENTER after the delegation instruction rather than at pc 0.
    int pc = start_pc;
    for (int i = 0; i < instruction_count; i++) {
      slots[i].new_pc = pc;
      int len = slots[i].base_no_pad;
      if (slots[i].is_switch) {
        // Switch opcode sits at new_pc (EXIT prefix never applies to switches).
        len += soroush_switch_padding(pc);
      } else if (slots[i].is_branch && slots[i].widened) {
        len += soroush_branch_widen_delta(slots[i].op);
      }
      slots[i].new_len = len;
      pc += len;
      if (is_constructor && slots[i].old_pc == enter_after_old_pc) {
        pc += entry_len; // ENTER inserted after delegation
      }
    }
    total_len = pc;

    for (int j = 0; j <= code_len; j++) map[j] = -1;
    for (int i = 0; i < instruction_count; i++) {
      map[slots[i].old_pc] = slots[i].new_pc;
    }
    map[code_len] = total_len;

    // Overflow sweep: any not-yet-widened branch whose offset no longer fits
    // in s2 under the current layout is marked for widening.
    int newly = 0;
    for (int i = 0; i < instruction_count; i++) {
      if (!slots[i].is_branch || slots[i].widened) continue;
      int ot = slots[i].old_target;
      if (ot < 0 || ot > code_len || map[ot] < 0) {
        free(slots);
        free(map);
        *error = "branch target is not an instruction boundary during convergence";
        return false;
      }
      int off = map[ot] - slots[i].new_pc;
      if (off < -32768 || off > 32767) {
        slots[i].widened = true;
        widened_total++;
        newly++;
      }
    }

    fprintf(stderr,
            "[JVM REWRITER CONVERGE] iter=%d newly_widened=%d widened_total=%d modeled_code_len=%d\n",
            iter, newly, widened_total, total_len);

    if (newly == 0) {
      converged = true;
      break;
    }
  }

  int direct = 0;
  int invert = 0;
  for (int i = 0; i < instruction_count; i++) {
    if (!slots[i].widened) continue;
    if (soroush_classify_branch_widening(slots[i].op) == SOROUSH_BRANCH_WIDEN_DIRECT) {
      direct++;
    } else {
      invert++;
    }
  }

  fprintf(stderr,
          "[JVM REWRITER CONVERGE] converged=%s iterations=%d widened=%d direct=%d invert=%d final_code_len=%d\n",
          converged ? "yes" : "no", iter, widened_total, direct, invert, total_len);

  // Write the stabilized layout back so emission and metadata remapping use
  // the exact same stable PCs. Only on convergence -- otherwise the caller
  // fails safe and the base layout is left untouched.
  if (converged) {
    for (int i = 0; i < instruction_count; i++) {
      instructions[i].new_pc = slots[i].new_pc;
      instructions[i].new_len = slots[i].new_len;
      instructions[i].widened = slots[i].widened;
    }
    for (int j = 0; j <= code_len; j++) pc_map[j] = -1;
    for (int i = 0; i < instruction_count; i++) {
      pc_map[slots[i].old_pc] = slots[i].new_pc;
    }
    pc_map[code_len] = total_len;
  }

  free(slots);
  free(map);

  *converged_out = converged;
  *widened_out = widened_total;
  *direct_out = direct;
  *invert_out = invert;
  *iterations_out = iter;
  *final_len_out = total_len;
  return true;
}

static bool soroush_emit_rewritten_code(SoroushByteWriter* out,
                                        const u1* entry_code,
                                        int entry_len,
                                        bool insert_exits,
                                        int temp_local,
                                        bool is_constructor,
                                        int enter_after_old_pc,
                                        u2 exit_string_index,
                                        u2 trace_methodref,
                                        const u1* code,
                                        int code_len,
                                        const SoroushInstruction* instructions,
                                        int instruction_count,
                                        const int* pc_map,
                                        const char** error) {
  // The layout fed in here is already stabilized: the caller ran detection
  // (Phase 6A) and convergence (Phase 6B) so that instructions[].new_pc /
  // new_len / widened and pc_map[] reflect the final, stable layout. This
  // function only consumes that layout -- it never recomputes a layout or
  // decides widening, and it emits widened branches solely from the `widened`
  // flag. Final offsets are therefore computed against the stable PCs.
  //
  // Normal methods prepend ENTER at pc 0. Constructors instead emit ENTER right
  // after the delegation invokespecial (enter_after_old_pc) and never
  // instrument athrow (no exception-path EXIT before `this` rules are met).
  bool instrument_athrow = !is_constructor;
  if (!is_constructor && entry_len > 0) {
    out->append_bytes(entry_code, entry_len);
  }

  for (int i = 0; i < instruction_count; i++) {
    const SoroushInstruction* ins = &instructions[i];
    int old_pc = ins->old_pc;
    int new_pc = ins->new_pc;
    u1 op = ins->op;
    int instruction_pc = new_pc;
    bool wants_exit = insert_exits &&
        (soroush_is_return_opcode(op) || (op == 0xbf && instrument_athrow));
    if (wants_exit) {
      if (!soroush_append_exit_prefix(out, op, temp_local, exit_string_index,
                                      trace_methodref, error)) {
        return false;
      }
      instruction_pc += soroush_exit_prefix_len(op, temp_local, instrument_athrow);
    }

    if ((op >= 0x99 && op <= 0xa8) || op == 0xc6 || op == 0xc7) {
      int old_target = old_pc + soroush_read_s2(code + old_pc + 1);
      int new_target = 0;
      if (!soroush_remap_pc(old_target, code_len, pc_map, &new_target, error)) return false;
      int offset = new_target - instruction_pc;
      if (ins->widened) {
        // Emit the widened form using the stabilized layout. Branches never
        // carry an EXIT prefix, so instruction_pc == new_pc and the emitted
        // size must match ins->new_len exactly (consistency guard below).
        int branch_before = out->length();
        if (op == 0xa7) {            // Phase 6B: goto -> goto_w
          out->append_u1(0xc8);
          out->append_s4(offset);
        } else if (op == 0xa8) {     // Phase 6B: jsr -> jsr_w
          out->append_u1(0xc9);
          out->append_s4(offset);
        } else {                     // Phase 6C: if<cond> -> inverted if + goto_w
          u1 inverted = soroush_invert_conditional_op(op);
          if (inverted == 0) {
            *error = "no inversion mapping for conditional branch (fail-safe)";
            return false;
          }
          // if_inverse_cond AFTER_GOTO : when the original condition is false,
          // skip the inserted goto_w (offset +8 = 3-byte if + 5-byte goto_w).
          out->append_u1(inverted);
          out->append_s2(8);
          // goto_w TARGET : when the original condition is true, take the
          // (wide) branch. goto_w sits 3 bytes after the conditional opcode.
          out->append_u1(0xc8);
          out->append_s4(new_target - (instruction_pc + 3));
        }
        if (!out->ok()) {
          *error = "out of memory while emitting widened branch";
          return false;
        }
        if (out->length() - branch_before != ins->new_len) {
          *error = "widened branch size disagrees with stabilized layout (fail-safe)";
          return false;
        }
      } else {
        out->append_u1(op);
        if (!soroush_append_s2_checked(out, offset, error)) return false;
      }
    } else if (op == 0xc8 || op == 0xc9) {
      int old_target = old_pc + soroush_read_s4(code + old_pc + 1);
      int new_target = 0;
      if (!soroush_remap_pc(old_target, code_len, pc_map, &new_target, error)) return false;
      out->append_u1(op);
      out->append_s4(new_target - instruction_pc);
    } else if (op == 0xaa) {
      int old_p = old_pc + 1;
      while ((old_p & 3) != 0) old_p++;
      int default_target = old_pc + soroush_read_s4(code + old_p);
      int low = soroush_read_s4(code + old_p + 4);
      int high = soroush_read_s4(code + old_p + 8);
      int entries = high - low + 1;
      int new_target = 0;

      out->append_u1(op);
      for (int pad = soroush_switch_padding(instruction_pc); pad > 0; pad--) {
        out->append_u1(0);
      }
      if (!soroush_remap_pc(default_target, code_len, pc_map, &new_target, error)) return false;
      out->append_s4(new_target - instruction_pc);
      out->append_s4(low);
      out->append_s4(high);
      for (int j = 0; j < entries; j++) {
        int old_target = old_pc + soroush_read_s4(code + old_p + 12 + j * 4);
        if (!soroush_remap_pc(old_target, code_len, pc_map, &new_target, error)) return false;
        out->append_s4(new_target - instruction_pc);
      }
    } else if (op == 0xab) {
      int old_p = old_pc + 1;
      while ((old_p & 3) != 0) old_p++;
      int default_target = old_pc + soroush_read_s4(code + old_p);
      int pairs = soroush_read_s4(code + old_p + 4);
      int new_target = 0;

      out->append_u1(op);
      for (int pad = soroush_switch_padding(instruction_pc); pad > 0; pad--) {
        out->append_u1(0);
      }
      if (!soroush_remap_pc(default_target, code_len, pc_map, &new_target, error)) return false;
      out->append_s4(new_target - instruction_pc);
      out->append_s4(pairs);
      for (int j = 0; j < pairs; j++) {
        int match = soroush_read_s4(code + old_p + 8 + j * 8);
        int old_target = old_pc + soroush_read_s4(code + old_p + 12 + j * 8);
        if (!soroush_remap_pc(old_target, code_len, pc_map, &new_target, error)) return false;
        out->append_s4(match);
        out->append_s4(new_target - instruction_pc);
      }
    } else {
      out->append_bytes(code + old_pc, ins->old_len);
    }

    // Constructor ENTER: emit immediately after the delegation invokespecial,
    // i.e. once `this` is initialized. Branches to the following instruction
    // map past these bytes, so ENTER fires only on the fall-through.
    if (is_constructor && old_pc == enter_after_old_pc && entry_len > 0) {
      out->append_bytes(entry_code, entry_len);
    }

    if (!out->ok()) {
      *error = "out of memory while emitting rewritten code";
      return false;
    }
  }
  return true;
}

static bool soroush_copy_verification_type(SoroushByteWriter* out,
                                           SoroushClassReader* r,
                                           int pc_delta,
                                           const char** error) {
  u1 tag = r->get_u1("truncated verification_type_info tag");
  if (!r->ok()) {
    *error = r->error();
    return false;
  }
  out->append_u1(tag);
  switch (tag) {
    case 0: // Top
    case 1: // Integer
    case 2: // Float
    case 3: // Double
    case 4: // Long
    case 5: // Null
    case 6: // UninitializedThis
      return out->ok();
    case 7: { // Object
      u2 cpool_index = r->get_u2("truncated Object_variable_info");
      out->append_u2(cpool_index);
      return r->ok() && out->ok();
    }
    case 8: { // Uninitialized
      u2 offset = r->get_u2("truncated Uninitialized_variable_info");
      out->append_u2((u2)(offset + pc_delta));
      return r->ok() && out->ok();
    }
    default:
      *error = "bad verification_type_info tag";
      return false;
  }
}

static bool soroush_copy_stack_map_frame(SoroushByteWriter* out,
                                         SoroushClassReader* r,
                                         bool first_frame,
                                         int pc_delta,
                                         const char** error) {
  u1 frame_type = r->get_u1("truncated stack map frame type");
  if (!r->ok()) {
    *error = r->error();
    return false;
  }

  int offset_delta_adjust = first_frame ? pc_delta : 0;
  if (frame_type <= 63) {
    int new_delta = frame_type + offset_delta_adjust;
    if (new_delta <= 63) {
      out->append_u1((u1)new_delta);
    } else {
      out->append_u1(251);
      out->append_u2((u2)new_delta);
    }
    return out->ok();
  }

  if (frame_type <= 127) {
    int old_delta = frame_type - 64;
    int new_delta = old_delta + offset_delta_adjust;
    if (new_delta <= 63) {
      out->append_u1((u1)(64 + new_delta));
    } else {
      out->append_u1(247);
      out->append_u2((u2)new_delta);
    }
    return soroush_copy_verification_type(out, r, pc_delta, error);
  }

  if (frame_type == 247) {
    u2 offset_delta = r->get_u2("truncated same_locals_1_stack_item_frame_extended");
    out->append_u1(frame_type);
    out->append_u2((u2)(offset_delta + offset_delta_adjust));
    return soroush_copy_verification_type(out, r, pc_delta, error);
  }

  if (frame_type >= 248 && frame_type <= 250) {
    u2 offset_delta = r->get_u2("truncated chop_frame");
    out->append_u1(frame_type);
    out->append_u2((u2)(offset_delta + offset_delta_adjust));
    return r->ok() && out->ok();
  }

  if (frame_type == 251) {
    u2 offset_delta = r->get_u2("truncated same_frame_extended");
    out->append_u1(frame_type);
    out->append_u2((u2)(offset_delta + offset_delta_adjust));
    return r->ok() && out->ok();
  }

  if (frame_type >= 252 && frame_type <= 254) {
    u2 offset_delta = r->get_u2("truncated append_frame");
    out->append_u1(frame_type);
    out->append_u2((u2)(offset_delta + offset_delta_adjust));
    int locals = frame_type - 251;
    for (int i = 0; i < locals; i++) {
      if (!soroush_copy_verification_type(out, r, pc_delta, error)) return false;
    }
    return r->ok() && out->ok();
  }

  if (frame_type == 255) {
    u2 offset_delta = r->get_u2("truncated full_frame");
    out->append_u1(frame_type);
    out->append_u2((u2)(offset_delta + offset_delta_adjust));
    u2 number_of_locals = r->get_u2("truncated full_frame locals");
    out->append_u2(number_of_locals);
    for (u2 i = 0; i < number_of_locals; i++) {
      if (!soroush_copy_verification_type(out, r, pc_delta, error)) return false;
    }
    u2 number_of_stack_items = r->get_u2("truncated full_frame stack");
    out->append_u2(number_of_stack_items);
    for (u2 i = 0; i < number_of_stack_items; i++) {
      if (!soroush_copy_verification_type(out, r, pc_delta, error)) return false;
    }
    return r->ok() && out->ok();
  }

  *error = "bad stack map frame type";
  return false;
}

static bool soroush_transform_stack_map_table(SoroushByteWriter* out,
                                              const u1* body,
                                              int length,
                                              int pc_delta,
                                              const char** error) {
  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated StackMapTable number_of_entries");
  out->append_u2(entries);
  for (u2 i = 0; i < entries; i++) {
    if (!soroush_copy_stack_map_frame(out, &r, i == 0, pc_delta, error)) {
      return false;
    }
  }
  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "StackMapTable did not consume full input" : r.error();
    return false;
  }
  return out->ok();
}

static bool soroush_transform_line_number_table(SoroushByteWriter* out,
                                                const u1* body,
                                                int length,
                                                int pc_delta,
                                                const char** error) {
  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated LineNumberTable length");
  out->append_u2(entries);
  for (u2 i = 0; i < entries; i++) {
    u2 start_pc = r.get_u2("truncated LineNumberTable start_pc");
    u2 line = r.get_u2("truncated LineNumberTable line_number");
    out->append_u2((u2)(start_pc + pc_delta));
    out->append_u2(line);
  }
  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "LineNumberTable did not consume full input" : r.error();
    return false;
  }
  return out->ok();
}

static bool soroush_transform_local_variable_table(SoroushByteWriter* out,
                                                   const u1* body,
                                                   int length,
                                                   int pc_delta,
                                                   const char** error) {
  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated local variable table length");
  out->append_u2(entries);
  for (u2 i = 0; i < entries; i++) {
    u2 start_pc = r.get_u2("truncated local variable start_pc");
    u2 len = r.get_u2("truncated local variable length");
    u2 name_index = r.get_u2("truncated local variable name_index");
    u2 descriptor_index = r.get_u2("truncated local variable descriptor_index");
    u2 index = r.get_u2("truncated local variable index");
    out->append_u2((u2)(start_pc + pc_delta));
    out->append_u2(len);
    out->append_u2(name_index);
    out->append_u2(descriptor_index);
    out->append_u2(index);
  }
  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "local variable table did not consume full input" : r.error();
    return false;
  }
  return out->ok();
}

static bool soroush_copy_verification_type_mapped(SoroushByteWriter* out,
                                                  SoroushClassReader* r,
                                                  int code_len,
                                                  const int* pc_map,
                                                  const char** error) {
  u1 tag = r->get_u1("truncated verification_type_info tag");
  if (!r->ok()) {
    *error = r->error();
    return false;
  }
  out->append_u1(tag);
  switch (tag) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      return out->ok();
    case 7: {
      u2 cpool_index = r->get_u2("truncated Object_variable_info");
      out->append_u2(cpool_index);
      return r->ok() && out->ok();
    }
    case 8: {
      u2 offset = r->get_u2("truncated Uninitialized_variable_info");
      int mapped = 0;
      if (!soroush_remap_pc(offset, code_len, pc_map, &mapped, error)) return false;
      out->append_u2((u2)mapped);
      return r->ok() && out->ok();
    }
    default:
      *error = "bad verification_type_info tag";
      return false;
  }
}

static bool soroush_read_verification_type(SoroushClassReader* r,
                                           SoroushVerificationType* vt,
                                           const char** error) {
  vt->tag = r->get_u1("truncated verification_type_info tag");
  vt->data = 0;
  if (!r->ok()) {
    *error = r->error();
    return false;
  }
  switch (vt->tag) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      return true;
    case 7:
    case 8:
      vt->data = r->get_u2("truncated verification_type_info payload");
      if (!r->ok()) {
        *error = r->error();
        return false;
      }
      return true;
    default:
      *error = "bad verification_type_info tag";
      return false;
  }
}

static void soroush_append_verification_type(SoroushByteWriter* out,
                                             const SoroushVerificationType* vt) {
  out->append_u1(vt->tag);
  if (vt->tag == 7 || vt->tag == 8) {
    out->append_u2(vt->data);
  }
}

static int soroush_trim_stack_map_locals(const SoroushVerificationType* locals, int locals_count) {
  while (locals_count > 0 && locals[locals_count - 1].tag == 0) {
    locals_count--;
  }
  return locals_count;
}

static bool soroush_copy_locals(SoroushExceptionExitHandler* handler,
                                const SoroushVerificationType* locals,
                                int locals_count,
                                const char** error) {
  locals_count = soroush_trim_stack_map_locals(locals, locals_count);
  handler->locals = nullptr;
  handler->locals_count = locals_count;
  if (locals_count == 0) {
    return true;
  }
  handler->locals = (SoroushVerificationType*)malloc(locals_count * sizeof(SoroushVerificationType));
  if (handler->locals == nullptr) {
    *error = "out of memory while copying exception handler locals";
    return false;
  }
  memcpy(handler->locals, locals, locals_count * sizeof(SoroushVerificationType));
  return true;
}

static void soroush_free_exception_handlers(SoroushExceptionExitHandler* handlers, int count) {
  if (handlers == nullptr) return;
  for (int i = 0; i < count; i++) {
    free(handlers[i].locals);
    handlers[i].locals = nullptr;
  }
  free(handlers);
}

static bool soroush_copy_stack_map_frame_mapped(SoroushByteWriter* out,
                                                SoroushClassReader* r,
                                                int old_frame_pc,
                                                int* previous_new_frame_pc,
                                                bool first_frame,
                                                int code_len,
                                                const int* pc_map,
                                                const char** error) {
  u1 frame_type = r->get_u1("truncated stack map frame type");
  if (!r->ok()) {
    *error = r->error();
    return false;
  }

  int new_frame_pc = 0;
  if (!soroush_remap_pc(old_frame_pc, code_len, pc_map, &new_frame_pc, error)) {
    return false;
  }
  int new_delta = first_frame ? new_frame_pc : new_frame_pc - *previous_new_frame_pc - 1;
  if (new_delta < 0 || new_delta > 65535) {
    *error = "StackMapTable offset_delta out of range";
    return false;
  }
  *previous_new_frame_pc = new_frame_pc;

  if (frame_type <= 63) {
    if (new_delta <= 63) {
      out->append_u1((u1)new_delta);
    } else {
      out->append_u1(251);
      out->append_u2((u2)new_delta);
    }
    return out->ok();
  }

  if (frame_type <= 127) {
    if (new_delta <= 63) {
      out->append_u1((u1)(64 + new_delta));
    } else {
      out->append_u1(247);
      out->append_u2((u2)new_delta);
    }
    return soroush_copy_verification_type_mapped(out, r, code_len, pc_map, error);
  }

  if (frame_type == 247) {
    r->skip(2, "truncated same_locals_1_stack_item_frame_extended");
    out->append_u1(frame_type);
    out->append_u2((u2)new_delta);
    return r->ok() &&
        soroush_copy_verification_type_mapped(out, r, code_len, pc_map, error);
  }

  if (frame_type >= 248 && frame_type <= 250) {
    r->skip(2, "truncated chop_frame");
    out->append_u1(frame_type);
    out->append_u2((u2)new_delta);
    return r->ok() && out->ok();
  }

  if (frame_type == 251) {
    r->skip(2, "truncated same_frame_extended");
    out->append_u1(frame_type);
    out->append_u2((u2)new_delta);
    return r->ok() && out->ok();
  }

  if (frame_type >= 252 && frame_type <= 254) {
    r->skip(2, "truncated append_frame");
    out->append_u1(frame_type);
    out->append_u2((u2)new_delta);
    int locals = frame_type - 251;
    for (int i = 0; i < locals; i++) {
      if (!soroush_copy_verification_type_mapped(out, r, code_len, pc_map, error)) return false;
    }
    return r->ok() && out->ok();
  }

  if (frame_type == 255) {
    r->skip(2, "truncated full_frame");
    out->append_u1(frame_type);
    out->append_u2((u2)new_delta);
    u2 number_of_locals = r->get_u2("truncated full_frame locals");
    out->append_u2(number_of_locals);
    for (u2 i = 0; i < number_of_locals; i++) {
      if (!soroush_copy_verification_type_mapped(out, r, code_len, pc_map, error)) return false;
    }
    u2 number_of_stack_items = r->get_u2("truncated full_frame stack");
    out->append_u2(number_of_stack_items);
    for (u2 i = 0; i < number_of_stack_items; i++) {
      if (!soroush_copy_verification_type_mapped(out, r, code_len, pc_map, error)) return false;
    }
    return r->ok() && out->ok();
  }

  *error = "bad stack map frame type";
  return false;
}

static bool soroush_append_exception_handler_full_frame(SoroushByteWriter* out,
                                                        const SoroushExceptionExitHandler* handler,
                                                        int* previous_new_frame_pc,
                                                        bool first_frame,
                                                        u2 throwable_class_index,
                                                        const char** error);

static bool soroush_emit_widened_stack_map_table(SoroushByteWriter* out,
                                                 const u1* body,
                                                 int length,
                                                 const u1* code,
                                                 int code_len,
                                                 const int* pc_map,
                                                 const SoroushInstruction* instructions,
                                                 int instruction_count,
                                                 int max_locals,
                                                 int max_stack,
                                                 bool is_static,
                                                 u2 this_class_index,
                                                 u2 object_class_index,
                                                 const char* descriptor,
                                                 int descriptor_len,
                                                 const SoroushExceptionExitHandler* exception_handlers,
                                                 int exception_handler_count,
                                                 u2 throwable_class_index,
                                                 const char** error);

static bool soroush_transform_stack_map_table_mapped(SoroushByteWriter* out,
                                                     const u1* body,
                                                     int length,
                                                     const u1* code,
                                                     int code_len,
                                                     const int* pc_map,
                                                     const SoroushInstruction* instructions,
                                                     int instruction_count,
                                                     int max_locals,
                                                     int max_stack,
                                                     bool is_static,
                                                     u2 this_class_index,
                                                     u2 object_class_index,
                                                     const char* descriptor,
                                                     int descriptor_len,
                                                     const SoroushExceptionExitHandler* exception_handlers,
                                                     int exception_handler_count,
                                                     u2 throwable_class_index,
                                                     const char** error) {
  // If any conditional was widened, the inserted goto_w forces a frame at the
  // fall-through that javac may not have emitted. Re-emit the whole table from
  // absolute frame states (which also synthesizes those frames). Otherwise use
  // the original incremental remapping path unchanged.
  bool has_widened_conditional = false;
  for (int i = 0; i < instruction_count; i++) {
    if (instructions[i].widened &&
        soroush_classify_branch_widening(instructions[i].op) == SOROUSH_BRANCH_WIDEN_INVERT) {
      has_widened_conditional = true;
      break;
    }
  }
  if (has_widened_conditional) {
    return soroush_emit_widened_stack_map_table(out, body, length, code, code_len, pc_map,
                                                instructions, instruction_count,
                                                max_locals, max_stack, is_static,
                                                this_class_index, object_class_index,
                                                descriptor, descriptor_len,
                                                exception_handlers, exception_handler_count,
                                                throwable_class_index, error);
  }

  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated StackMapTable number_of_entries");
  if (exception_handler_count < 0 || entries > 65535 - exception_handler_count) {
    *error = "StackMapTable too large for exception EXIT handler frame";
    return false;
  }
  out->append_u2((u2)(entries + exception_handler_count));
  int previous_old_frame_pc = -1;
  int previous_new_frame_pc = -1;
  for (u2 i = 0; i < entries; i++) {
    const u1* frame_start = r.current();
    SoroushClassReader scan_reader(frame_start, (int)(body + length - frame_start));
    u1 frame_type = scan_reader.get_u1("truncated stack map frame type");
    if (!scan_reader.ok()) {
      *error = scan_reader.error();
      return false;
    }
    int offset_delta = 0;
    if (frame_type <= 63) {
      offset_delta = frame_type;
    } else if (frame_type <= 127) {
      offset_delta = frame_type - 64;
    } else {
      offset_delta = scan_reader.get_u2("truncated stack map offset_delta");
      if (!scan_reader.ok()) {
        *error = scan_reader.error();
        return false;
      }
    }
    int old_frame_pc = (i == 0) ? offset_delta : previous_old_frame_pc + offset_delta + 1;
    previous_old_frame_pc = old_frame_pc;

    SoroushClassReader frame_reader(frame_start, (int)(body + length - frame_start));
    if (!soroush_copy_stack_map_frame_mapped(out, &frame_reader, old_frame_pc,
                                            &previous_new_frame_pc, i == 0,
                                            code_len, pc_map, error)) {
      return false;
    }
    r.skip((int)(frame_reader.current() - frame_start), "truncated StackMapTable frame");
  }
  for (int i = 0; i < exception_handler_count; i++) {
    if (!soroush_append_exception_handler_full_frame(out, &exception_handlers[i],
                                                     &previous_new_frame_pc,
                                                     entries == 0 && i == 0,
                                                     throwable_class_index,
                                                     error)) {
      return false;
    }
  }
  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "StackMapTable did not consume full input" : r.error();
    return false;
  }
  return out->ok();
}

static bool soroush_append_single_exception_handler_stack_map(SoroushByteWriter* out,
                                                              int exception_handler_pc,
                                                              u2 throwable_class_index,
                                                              const char** error) {
  if (exception_handler_pc < 0 || exception_handler_pc > 65535) {
    *error = "exception EXIT StackMapTable offset_delta out of range";
    return false;
  }
  out->append_u2(1);
  if (exception_handler_pc <= 63) {
    out->append_u1((u1)(64 + exception_handler_pc));
  } else {
    out->append_u1(247); // same_locals_1_stack_item_frame_extended
    out->append_u2((u2)exception_handler_pc);
  }
  out->append_u1(7); // Object_variable_info
  out->append_u2(throwable_class_index);
  if (!out->ok()) {
    *error = "out of memory while building exception EXIT StackMapTable";
    return false;
  }
  return true;
}

static bool soroush_append_exception_handler_full_frame(SoroushByteWriter* out,
                                                        const SoroushExceptionExitHandler* handler,
                                                        int* previous_new_frame_pc,
                                                        bool first_frame,
                                                        u2 throwable_class_index,
                                                        const char** error) {
  int new_delta = first_frame ? handler->handler_pc :
      handler->handler_pc - *previous_new_frame_pc - 1;
  if (new_delta < 0 || new_delta > 65535) {
    *error = "exception EXIT StackMapTable offset_delta out of range";
    return false;
  }
  *previous_new_frame_pc = handler->handler_pc;
  out->append_u1(255); // full_frame
  out->append_u2((u2)new_delta);
  out->append_u2((u2)handler->locals_count);
  for (int i = 0; i < handler->locals_count; i++) {
    soroush_append_verification_type(out, &handler->locals[i]);
  }
  out->append_u2(1);
  out->append_u1(7); // Object_variable_info
  out->append_u2(throwable_class_index);
  if (!out->ok()) {
    *error = "out of memory while building exception EXIT StackMapTable frame";
    return false;
  }
  return true;
}

static bool soroush_init_method_locals(SoroushVerificationType* locals,
                                       int* locals_count,
                                       int max_locals,
                                       bool is_static,
                                       u2 this_class_index,
                                       u2 object_class_index,
                                       const char* descriptor,
                                       int descriptor_len,
                                       const char** error) {
  *locals_count = 0;
  if (!is_static) {
    if (*locals_count >= max_locals) {
      *error = "method locals exceed max_locals";
      return false;
    }
    locals[*locals_count].tag = 7;
    locals[*locals_count].data = this_class_index;
    (*locals_count)++;
  }
  if (descriptor == nullptr || descriptor_len <= 0 || descriptor[0] != '(') {
    *error = "bad method descriptor";
    return false;
  }
  int p = 1;
  while (p < descriptor_len && descriptor[p] != ')') {
    if (*locals_count >= max_locals) {
      *error = "method descriptor locals exceed max_locals";
      return false;
    }
    char c = descriptor[p++];
    switch (c) {
      case 'B':
      case 'C':
      case 'I':
      case 'S':
      case 'Z':
        locals[*locals_count].tag = 1;
        locals[*locals_count].data = 0;
        (*locals_count)++;
        break;
      case 'F':
        locals[*locals_count].tag = 2;
        locals[*locals_count].data = 0;
        (*locals_count)++;
        break;
      case 'D':
        locals[*locals_count].tag = 3;
        locals[*locals_count].data = 0;
        (*locals_count)++;
        break;
      case 'J':
        locals[*locals_count].tag = 4;
        locals[*locals_count].data = 0;
        (*locals_count)++;
        break;
      case 'L':
        while (p < descriptor_len && descriptor[p] != ';') p++;
        if (p >= descriptor_len) {
          *error = "bad object method descriptor";
          return false;
        }
        p++;
        locals[*locals_count].tag = 7;
        locals[*locals_count].data = object_class_index;
        (*locals_count)++;
        break;
      case '[':
        while (p < descriptor_len && descriptor[p] == '[') p++;
        if (p >= descriptor_len) {
          *error = "bad array method descriptor";
          return false;
        }
        if (descriptor[p] == 'L') {
          while (p < descriptor_len && descriptor[p] != ';') p++;
          if (p >= descriptor_len) {
            *error = "bad array object method descriptor";
            return false;
          }
        }
        p++;
        locals[*locals_count].tag = 7;
        locals[*locals_count].data = object_class_index;
        (*locals_count)++;
        break;
      default:
        *error = "unsupported method descriptor local type";
        return false;
    }
  }
  if (p >= descriptor_len || descriptor[p] != ')') {
    *error = "bad method descriptor";
    return false;
  }
  return true;
}

static bool soroush_has_uninitialized_local(const SoroushVerificationType* locals, int locals_count) {
  for (int i = 0; i < locals_count; i++) {
    if (locals[i].tag == 6 || locals[i].tag == 8) return true;
  }
  return false;
}

// If the instruction at old_pc writes a local-variable slot, report the slot
// and the verification tag it writes (1=int, 2=float, 3=double, 4=long,
// 7=reference). iinc is intentionally excluded: it preserves the int type, so
// it never changes a slot's verification type.
static bool soroush_store_writes_slot(const u1* code, int code_len, int old_pc,
                                      int* slot, int* tag) {
  if (old_pc < 0 || old_pc >= code_len) return false;
  static const int store_tags[] = {1, 4, 2, 3, 7}; // istore,lstore,fstore,dstore,astore
  u1 op = code[old_pc];
  if (op >= 0x36 && op <= 0x3a) {            // istore..astore <index>
    if (old_pc + 1 >= code_len) return false;
    *tag = store_tags[op - 0x36];
    *slot = code[old_pc + 1];
    return true;
  }
  if (op >= 0x3b && op <= 0x3e) { *tag = 1; *slot = op - 0x3b; return true; } // istore_0..3
  if (op >= 0x3f && op <= 0x42) { *tag = 4; *slot = op - 0x3f; return true; } // lstore_0..3
  if (op >= 0x43 && op <= 0x46) { *tag = 2; *slot = op - 0x43; return true; } // fstore_0..3
  if (op >= 0x47 && op <= 0x4a) { *tag = 3; *slot = op - 0x47; return true; } // dstore_0..3
  if (op >= 0x4b && op <= 0x4e) { *tag = 7; *slot = op - 0x4b; return true; } // astore_0..3
  if (op == 0xc4) {                          // wide <store> <u2 index>
    if (old_pc + 3 >= code_len) return false;
    u1 wop = code[old_pc + 1];
    if (wop < 0x36 || wop > 0x3a) return false; // wide iinc / wide load: no type change
    *tag = store_tags[wop - 0x36];
    *slot = ((int)code[old_pc + 2] << 8) | code[old_pc + 3];
    return true;
  }
  return false;
}

static bool soroush_add_exception_handler_regions(SoroushExceptionExitHandler** handlers,
                                                  int* handler_count,
                                                  int* handler_capacity,
                                                  const SoroushVerificationType* locals,
                                                  int locals_count,
                                                  int old_start,
                                                  int old_end,
                                                  const SoroushInstruction* instructions,
                                                  int instruction_count,
                                                  const u1* code,
                                                  int code_len,
                                                  const char* method_id,
                                                  const char** error) {
  if (old_end <= old_start) return true;
  if (soroush_has_uninitialized_local(locals, locals_count)) {
    *error = "exception EXIT does not support uninitialized locals";
    return false;
  }

  // Conservative verifier-safety guard. The synthetic catch-all EXIT handler
  // frame for this region uses the single locals snapshot valid at old_start.
  // If a store inside [old_start, old_end) overwrites a slot that already holds
  // a non-Top verification type, that slot's type may change within the region,
  // so one frame cannot soundly represent it (this is the real-world
  // TomcatWebServer.start() failure: slot reused as Connector then Context).
  // Storing into a Top slot (defining a fresh local) stays safe because Top in
  // the handler frame accepts any later type. Fail safe rather than emit a
  // frame from an arbitrary single snapshot.
  for (int i = 0; i < instruction_count; i++) {
    int pc = instructions[i].old_pc;
    if (pc < old_start || pc >= old_end) continue;
    int slot = 0;
    int store_tag = 0;
    if (!soroush_store_writes_slot(code, code_len, pc, &slot, &store_tag)) continue;
    if (slot < 0 || slot >= locals_count) continue;   // beyond tracked locals -> Top -> safe
    u1 cur = locals[slot].tag;
    if (cur == 0) continue;                            // Top snapshot -> defining a local -> safe
    // A reference store (tag 7) can change the slot's class even when the tag
    // stays "reference", and we cannot verify the stored class here, so treat
    // it conservatively. A primitive store only conflicts if it differs.
    bool conflict = (store_tag == 7) ? true : (cur != (u1)store_tag);
    if (conflict) {
      fprintf(stderr,
              "[JVM REWRITER SAFE-SKIP] method=%s slot=%d first_type=%d conflicting_type=%d "
              "decision=skip-method-variable-locals-across-synthetic-handler-region\n",
              method_id == nullptr ? "<unknown>" : method_id,
              slot, (int)cur, store_tag);
      *error = "exception EXIT unsafe: local slot type varies across synthetic handler region (fail-safe)";
      return false;
    }
  }

  int segment_start = old_start;
  for (int i = 0; i < instruction_count; i++) {
    int pc = instructions[i].old_pc;
    if (pc < old_start || pc >= old_end || instructions[i].op != 0xbf) {
      continue;
    }
    if (segment_start < pc) {
      if (*handler_count >= *handler_capacity) {
        int new_capacity = *handler_capacity == 0 ? 8 : *handler_capacity * 2;
        SoroushExceptionExitHandler* resized =
            (SoroushExceptionExitHandler*)realloc(*handlers,
                                                  new_capacity * sizeof(SoroushExceptionExitHandler));
        if (resized == nullptr) {
          *error = "out of memory while growing exception EXIT handlers";
          return false;
        }
        *handlers = resized;
        *handler_capacity = new_capacity;
      }
      SoroushExceptionExitHandler* handler = &(*handlers)[(*handler_count)++];
      memset(handler, 0, sizeof(*handler));
      handler->old_start_pc = segment_start;
      handler->old_end_pc = pc;
      handler->handler_pc = -1;
      if (!soroush_copy_locals(handler, locals, locals_count, error)) return false;
    }
    segment_start = pc + instructions[i].old_len;
  }

  if (segment_start < old_end) {
    if (*handler_count >= *handler_capacity) {
      int new_capacity = *handler_capacity == 0 ? 8 : *handler_capacity * 2;
      SoroushExceptionExitHandler* resized =
          (SoroushExceptionExitHandler*)realloc(*handlers,
                                                new_capacity * sizeof(SoroushExceptionExitHandler));
      if (resized == nullptr) {
        *error = "out of memory while growing exception EXIT handlers";
        return false;
      }
      *handlers = resized;
      *handler_capacity = new_capacity;
    }
    SoroushExceptionExitHandler* handler = &(*handlers)[(*handler_count)++];
    memset(handler, 0, sizeof(*handler));
    handler->old_start_pc = segment_start;
    handler->old_end_pc = old_end;
    handler->handler_pc = -1;
    if (!soroush_copy_locals(handler, locals, locals_count, error)) return false;
  }
  return true;
}

static bool soroush_build_stack_map_exception_handlers(const u1* body,
                                                       int length,
                                                       const SoroushInstruction* instructions,
                                                       int instruction_count,
                                                       const u1* code,
                                                       int code_len,
                                                       int max_locals,
                                                       bool is_static,
                                                       u2 this_class_index,
                                                       u2 object_class_index,
                                                       const char* descriptor,
                                                       int descriptor_len,
                                                       const char* method_id,
                                                       SoroushExceptionExitHandler** handlers_out,
                                                       int* handler_count_out,
                                                       const char** error) {
  *handlers_out = nullptr;
  *handler_count_out = 0;
  SoroushVerificationType* locals =
      (SoroushVerificationType*)calloc(max_locals + 1, sizeof(SoroushVerificationType));
  if (locals == nullptr) {
    *error = "out of memory while building exception EXIT locals";
    return false;
  }
  int locals_count = 0;
  if (!soroush_init_method_locals(locals, &locals_count, max_locals, is_static,
                                  this_class_index, object_class_index,
                                  descriptor, descriptor_len, error)) {
    free(locals);
    return false;
  }

  SoroushExceptionExitHandler* handlers = nullptr;
  int handler_count = 0;
  int handler_capacity = 0;
  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated StackMapTable number_of_entries");
  int previous_old_frame_pc = -1;
  int region_start = 0;
  for (u2 i = 0; i < entries; i++) {
    u1 frame_type = r.get_u1("truncated stack map frame type");
    if (!r.ok()) {
      *error = r.error();
      soroush_free_exception_handlers(handlers, handler_count);
      free(locals);
      return false;
    }
    int offset_delta = 0;
    if (frame_type <= 63) {
      offset_delta = frame_type;
    } else if (frame_type <= 127) {
      offset_delta = frame_type - 64;
    } else {
      offset_delta = r.get_u2("truncated stack map offset_delta");
      if (!r.ok()) {
        *error = r.error();
        soroush_free_exception_handlers(handlers, handler_count);
        free(locals);
        return false;
      }
    }
    int frame_pc = (i == 0) ? offset_delta : previous_old_frame_pc + offset_delta + 1;
    if (frame_pc < region_start || frame_pc > code_len) {
      *error = "bad StackMapTable frame pc";
      soroush_free_exception_handlers(handlers, handler_count);
      free(locals);
      return false;
    }
    if (!soroush_add_exception_handler_regions(&handlers, &handler_count, &handler_capacity,
                                               locals, locals_count,
                                               region_start, frame_pc,
                                               instructions, instruction_count,
                                               code, code_len, method_id,
                                               error)) {
      soroush_free_exception_handlers(handlers, handler_count);
      free(locals);
      return false;
    }

    if (frame_type <= 63) {
      // same_frame: locals unchanged.
    } else if (frame_type <= 127) {
      SoroushVerificationType ignored;
      if (!soroush_read_verification_type(&r, &ignored, error)) {
        soroush_free_exception_handlers(handlers, handler_count);
        free(locals);
        return false;
      }
    } else if (frame_type == 247) {
      SoroushVerificationType ignored;
      if (!soroush_read_verification_type(&r, &ignored, error)) {
        soroush_free_exception_handlers(handlers, handler_count);
        free(locals);
        return false;
      }
    } else if (frame_type >= 248 && frame_type <= 250) {
      int chop = 251 - frame_type;
      if (chop > locals_count) {
        *error = "bad StackMapTable chop_frame";
        soroush_free_exception_handlers(handlers, handler_count);
        free(locals);
        return false;
      }
      locals_count -= chop;
    } else if (frame_type == 251) {
      // same_frame_extended: locals unchanged.
    } else if (frame_type >= 252 && frame_type <= 254) {
      int append = frame_type - 251;
      for (int j = 0; j < append; j++) {
        if (locals_count >= max_locals ||
            !soroush_read_verification_type(&r, &locals[locals_count++], error)) {
          if (*error == nullptr) *error = "bad StackMapTable append_frame";
          soroush_free_exception_handlers(handlers, handler_count);
          free(locals);
          return false;
        }
      }
    } else if (frame_type == 255) {
      u2 number_of_locals = r.get_u2("truncated full_frame locals");
      if (number_of_locals > max_locals) {
        *error = "full_frame locals exceed max_locals";
        soroush_free_exception_handlers(handlers, handler_count);
        free(locals);
        return false;
      }
      locals_count = number_of_locals;
      for (u2 j = 0; j < number_of_locals; j++) {
        if (!soroush_read_verification_type(&r, &locals[j], error)) {
          soroush_free_exception_handlers(handlers, handler_count);
          free(locals);
          return false;
        }
      }
      u2 number_of_stack_items = r.get_u2("truncated full_frame stack");
      for (u2 j = 0; j < number_of_stack_items; j++) {
        SoroushVerificationType ignored;
        if (!soroush_read_verification_type(&r, &ignored, error)) {
          soroush_free_exception_handlers(handlers, handler_count);
          free(locals);
          return false;
        }
      }
    } else {
      *error = "bad stack map frame type";
      soroush_free_exception_handlers(handlers, handler_count);
      free(locals);
      return false;
    }
    previous_old_frame_pc = frame_pc;
    region_start = frame_pc;
  }

  if (!soroush_add_exception_handler_regions(&handlers, &handler_count, &handler_capacity,
                                             locals, locals_count,
                                             region_start, code_len,
                                             instructions, instruction_count,
                                             code, code_len, method_id,
                                             error)) {
    soroush_free_exception_handlers(handlers, handler_count);
    free(locals);
    return false;
  }
  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "StackMapTable did not consume full input" : r.error();
    soroush_free_exception_handlers(handlers, handler_count);
    free(locals);
    return false;
  }
  free(locals);
  *handlers_out = handlers;
  *handler_count_out = handler_count;
  return true;
}

// ===========================================================================
// Phase 6C StackMapTable support.
//
// Widening a conditional inserts a goto_w. Because goto_w is unconditional,
// JVMS 4.10.1 requires a StackMapTable frame at the instruction *after* it --
// i.e. at the original branch's fall-through. javac usually does not emit a
// frame at an if-then fall-through, so we must synthesize one.
//
// Key fact: for any conditional branch the taken edge (-> target) and the
// not-taken edge (-> fall-through) leave identical (locals, stack) -- the
// conditional pops its operands and changes nothing else. So the fall-through
// frame is exactly the *target's* frame. The target is a branch target, so it
// always has a frame; we copy that frame's absolute state to the fall-through.
//
// When any conditional is widened we re-emit the whole StackMapTable from
// absolute frame states (original frames + synthesized fall-through frames +
// exception-EXIT handler frames), every entry as a full_frame. full_frame is
// always valid, which avoids delta-frame bookkeeping while merging. For the
// common case (no widened conditional) the original incremental path is used
// unchanged. We fail safe on any UninitializedThis/Uninitialized (tags 6/8)
// verification type to avoid re-deriving their embedded PCs.
// ===========================================================================

struct SoroushAbsFrame {
  int old_pc;
  int new_pc;
  SoroushVerificationType* locals;
  int locals_count;
  SoroushVerificationType* stack;
  int stack_count;
};

struct SoroushOutFrame {
  int new_pc;
  const SoroushVerificationType* locals;
  int locals_count;
  const SoroushVerificationType* stack;
  int stack_count;
};

static void soroush_free_abs_frames(SoroushAbsFrame* frames, int count) {
  if (frames == nullptr) return;
  for (int i = 0; i < count; i++) {
    free(frames[i].locals);
    free(frames[i].stack);
  }
  free(frames);
}

static bool soroush_vt_array_has_uninit(const SoroushVerificationType* v, int n) {
  for (int i = 0; i < n; i++) {
    if (v[i].tag == 6 || v[i].tag == 8) return true;
  }
  return false;
}

static SoroushVerificationType* soroush_copy_vt(const SoroushVerificationType* src,
                                                int count) {
  if (count == 0) return nullptr;
  SoroushVerificationType* dst =
      (SoroushVerificationType*)malloc(count * sizeof(SoroushVerificationType));
  if (dst != nullptr) {
    memcpy(dst, src, count * sizeof(SoroushVerificationType));
  }
  return dst;
}

// Decode the original StackMapTable into absolute (old_pc, locals, stack)
// frames, carrying locals forward exactly as the verifier does.
static bool soroush_decode_absolute_frames(const u1* body,
                                           int length,
                                           int max_locals,
                                           int max_stack,
                                           bool is_static,
                                           u2 this_class_index,
                                           u2 object_class_index,
                                           const char* descriptor,
                                           int descriptor_len,
                                           SoroushAbsFrame** frames_out,
                                           int* count_out,
                                           const char** error) {
  *frames_out = nullptr;
  *count_out = 0;

  SoroushVerificationType* locals =
      (SoroushVerificationType*)calloc(max_locals + 1, sizeof(SoroushVerificationType));
  SoroushVerificationType* stack_tmp =
      (SoroushVerificationType*)calloc(max_stack + 1, sizeof(SoroushVerificationType));
  if (locals == nullptr || stack_tmp == nullptr) {
    free(locals);
    free(stack_tmp);
    *error = "out of memory while decoding absolute frames";
    return false;
  }
  int locals_count = 0;
  if (!soroush_init_method_locals(locals, &locals_count, max_locals, is_static,
                                  this_class_index, object_class_index,
                                  descriptor, descriptor_len, error)) {
    free(locals);
    free(stack_tmp);
    return false;
  }

  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated StackMapTable number_of_entries");
  if (!r.ok()) {
    *error = r.error();
    free(locals);
    free(stack_tmp);
    return false;
  }

  SoroushAbsFrame* frames =
      entries > 0 ? (SoroushAbsFrame*)calloc(entries, sizeof(SoroushAbsFrame)) : nullptr;
  if (entries > 0 && frames == nullptr) {
    free(locals);
    free(stack_tmp);
    *error = "out of memory while decoding absolute frames";
    return false;
  }
  int count = 0;
  int previous_old_frame_pc = -1;

  for (u2 i = 0; i < entries; i++) {
    u1 frame_type = r.get_u1("truncated stack map frame type");
    if (!r.ok()) { *error = r.error(); goto fail; }
    int offset_delta = 0;
    if (frame_type <= 63) {
      offset_delta = frame_type;
    } else if (frame_type <= 127) {
      offset_delta = frame_type - 64;
    } else {
      offset_delta = r.get_u2("truncated stack map offset_delta");
      if (!r.ok()) { *error = r.error(); goto fail; }
    }
    int frame_pc = (i == 0) ? offset_delta : previous_old_frame_pc + offset_delta + 1;
    previous_old_frame_pc = frame_pc;

    int stack_count = 0;
    if (frame_type <= 63) {
      // same_frame: locals unchanged, empty stack.
    } else if (frame_type <= 127) {
      if (!soroush_read_verification_type(&r, &stack_tmp[0], error)) goto fail;
      stack_count = 1;
    } else if (frame_type == 247) {
      if (!soroush_read_verification_type(&r, &stack_tmp[0], error)) goto fail;
      stack_count = 1;
    } else if (frame_type >= 248 && frame_type <= 250) {
      int chop = 251 - frame_type;
      if (chop > locals_count) { *error = "bad StackMapTable chop_frame"; goto fail; }
      locals_count -= chop;
    } else if (frame_type == 251) {
      // same_frame_extended: locals unchanged.
    } else if (frame_type >= 252 && frame_type <= 254) {
      int append = frame_type - 251;
      for (int j = 0; j < append; j++) {
        if (locals_count >= max_locals ||
            !soroush_read_verification_type(&r, &locals[locals_count++], error)) {
          if (*error == nullptr) *error = "bad StackMapTable append_frame";
          goto fail;
        }
      }
    } else if (frame_type == 255) {
      u2 number_of_locals = r.get_u2("truncated full_frame locals");
      if (!r.ok()) { *error = r.error(); goto fail; }
      if (number_of_locals > max_locals) { *error = "full_frame locals exceed max_locals"; goto fail; }
      locals_count = number_of_locals;
      for (u2 j = 0; j < number_of_locals; j++) {
        if (!soroush_read_verification_type(&r, &locals[j], error)) goto fail;
      }
      u2 number_of_stack_items = r.get_u2("truncated full_frame stack");
      if (!r.ok()) { *error = r.error(); goto fail; }
      if (number_of_stack_items > max_stack) { *error = "full_frame stack exceeds max_stack"; goto fail; }
      for (u2 j = 0; j < number_of_stack_items; j++) {
        if (!soroush_read_verification_type(&r, &stack_tmp[j], error)) goto fail;
      }
      stack_count = number_of_stack_items;
    } else {
      *error = "bad stack map frame type";
      goto fail;
    }

    frames[count].old_pc = frame_pc;
    frames[count].new_pc = -1;
    frames[count].locals_count = locals_count;
    frames[count].stack_count = stack_count;
    frames[count].locals = soroush_copy_vt(locals, locals_count);
    frames[count].stack = soroush_copy_vt(stack_tmp, stack_count);
    if ((locals_count > 0 && frames[count].locals == nullptr) ||
        (stack_count > 0 && frames[count].stack == nullptr)) {
      *error = "out of memory while snapshotting absolute frame";
      count++; // so the partially-filled entry is freed
      goto fail;
    }
    count++;
  }

  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "StackMapTable did not consume full input" : r.error();
    goto fail;
  }

  free(locals);
  free(stack_tmp);
  *frames_out = frames;
  *count_out = count;
  return true;

fail:
  soroush_free_abs_frames(frames, count);
  free(locals);
  free(stack_tmp);
  return false;
}

static int soroush_find_abs_frame(const SoroushAbsFrame* frames, int count, int old_pc) {
  for (int i = 0; i < count; i++) {
    if (frames[i].old_pc == old_pc) return i;
  }
  return -1;
}

static int soroush_compare_out_frames(const void* a, const void* b) {
  int pa = ((const SoroushOutFrame*)a)->new_pc;
  int pb = ((const SoroushOutFrame*)b)->new_pc;
  return (pa > pb) - (pa < pb);
}

// Re-emit the full StackMapTable from absolute frame states when at least one
// conditional was widened. See the block comment above for the rationale.
static bool soroush_emit_widened_stack_map_table(SoroushByteWriter* out,
                                                 const u1* body,
                                                 int length,
                                                 const u1* code,
                                                 int code_len,
                                                 const int* pc_map,
                                                 const SoroushInstruction* instructions,
                                                 int instruction_count,
                                                 int max_locals,
                                                 int max_stack,
                                                 bool is_static,
                                                 u2 this_class_index,
                                                 u2 object_class_index,
                                                 const char* descriptor,
                                                 int descriptor_len,
                                                 const SoroushExceptionExitHandler* exception_handlers,
                                                 int exception_handler_count,
                                                 u2 throwable_class_index,
                                                 const char** error) {
  SoroushAbsFrame* abs_frames = nullptr;
  int abs_count = 0;
  if (!soroush_decode_absolute_frames(body, length, max_locals, max_stack, is_static,
                                      this_class_index, object_class_index,
                                      descriptor, descriptor_len,
                                      &abs_frames, &abs_count, error)) {
    return false;
  }

  // Fail safe on any uninitialized verification type (tags 6/8): re-deriving
  // their embedded PCs is out of scope and rare in widenable methods.
  for (int i = 0; i < abs_count; i++) {
    if (soroush_vt_array_has_uninit(abs_frames[i].locals, abs_frames[i].locals_count) ||
        soroush_vt_array_has_uninit(abs_frames[i].stack, abs_frames[i].stack_count)) {
      soroush_free_abs_frames(abs_frames, abs_count);
      *error = "uninitialized verification type with conditional widening (fail-safe)";
      return false;
    }
  }

  // Synthesize a fall-through frame for each widened conditional.
  int widened_cond = 0;
  for (int i = 0; i < instruction_count; i++) {
    if (instructions[i].widened &&
        soroush_classify_branch_widening(instructions[i].op) == SOROUSH_BRANCH_WIDEN_INVERT) {
      widened_cond++;
    }
  }
  SoroushAbsFrame* synth = widened_cond > 0
      ? (SoroushAbsFrame*)calloc(widened_cond, sizeof(SoroushAbsFrame)) : nullptr;
  if (widened_cond > 0 && synth == nullptr) {
    soroush_free_abs_frames(abs_frames, abs_count);
    *error = "out of memory while synthesizing fall-through frames";
    return false;
  }
  int synth_count = 0;
  for (int i = 0; i < instruction_count; i++) {
    const SoroushInstruction* ins = &instructions[i];
    if (!ins->widened ||
        soroush_classify_branch_widening(ins->op) != SOROUSH_BRANCH_WIDEN_INVERT) {
      continue;
    }
    int old_target = ins->old_pc + soroush_read_s2(code + ins->old_pc + 1);
    int target_idx = soroush_find_abs_frame(abs_frames, abs_count, old_target);
    if (target_idx < 0) {
      soroush_free_abs_frames(synth, synth_count);
      soroush_free_abs_frames(abs_frames, abs_count);
      *error = "widened conditional target has no StackMapTable frame (fail-safe)";
      return false;
    }
    int fall_through_old = ins->old_pc + ins->old_len;
    // If the fall-through already has a frame, the existing (remapped) frame
    // covers it -- do not synthesize a duplicate.
    if (soroush_find_abs_frame(abs_frames, abs_count, fall_through_old) >= 0) {
      continue;
    }
    int fall_through_new = 0;
    if (!soroush_remap_pc(fall_through_old, code_len, pc_map, &fall_through_new, error)) {
      soroush_free_abs_frames(synth, synth_count);
      soroush_free_abs_frames(abs_frames, abs_count);
      return false;
    }
    const SoroushAbsFrame* tf = &abs_frames[target_idx];
    SoroushAbsFrame* s = &synth[synth_count];
    s->old_pc = fall_through_old;
    s->new_pc = fall_through_new;
    s->locals_count = tf->locals_count;
    s->stack_count = tf->stack_count;
    s->locals = soroush_copy_vt(tf->locals, tf->locals_count);
    s->stack = soroush_copy_vt(tf->stack, tf->stack_count);
    if ((tf->locals_count > 0 && s->locals == nullptr) ||
        (tf->stack_count > 0 && s->stack == nullptr)) {
      synth_count++;
      soroush_free_abs_frames(synth, synth_count);
      soroush_free_abs_frames(abs_frames, abs_count);
      *error = "out of memory while copying synthesized fall-through frame";
      return false;
    }
    synth_count++;
  }

  // Build the combined, sorted output frame list: original (remapped) +
  // synthesized + exception-EXIT handlers, each emitted as a full_frame.
  SoroushVerificationType throwable_stack;
  throwable_stack.tag = 7;
  throwable_stack.data = throwable_class_index;

  int total = abs_count + synth_count + exception_handler_count;
  if (total < 0 || total > 65535) {
    soroush_free_abs_frames(synth, synth_count);
    soroush_free_abs_frames(abs_frames, abs_count);
    *error = "too many StackMapTable frames after widening (fail-safe)";
    return false;
  }
  SoroushOutFrame* outf = total > 0
      ? (SoroushOutFrame*)calloc(total, sizeof(SoroushOutFrame)) : nullptr;
  if (total > 0 && outf == nullptr) {
    soroush_free_abs_frames(synth, synth_count);
    soroush_free_abs_frames(abs_frames, abs_count);
    *error = "out of memory while building StackMapTable output frames";
    return false;
  }
  int n = 0;
  for (int i = 0; i < abs_count; i++) {
    int mapped = 0;
    if (!soroush_remap_pc(abs_frames[i].old_pc, code_len, pc_map, &mapped, error)) {
      free(outf);
      soroush_free_abs_frames(synth, synth_count);
      soroush_free_abs_frames(abs_frames, abs_count);
      return false;
    }
    outf[n].new_pc = mapped;
    outf[n].locals = abs_frames[i].locals;
    outf[n].locals_count = abs_frames[i].locals_count;
    outf[n].stack = abs_frames[i].stack;
    outf[n].stack_count = abs_frames[i].stack_count;
    n++;
  }
  for (int i = 0; i < synth_count; i++) {
    outf[n].new_pc = synth[i].new_pc;
    outf[n].locals = synth[i].locals;
    outf[n].locals_count = synth[i].locals_count;
    outf[n].stack = synth[i].stack;
    outf[n].stack_count = synth[i].stack_count;
    n++;
  }
  for (int i = 0; i < exception_handler_count; i++) {
    outf[n].new_pc = exception_handlers[i].handler_pc;
    outf[n].locals = exception_handlers[i].locals;
    outf[n].locals_count = exception_handlers[i].locals_count;
    outf[n].stack = &throwable_stack;
    outf[n].stack_count = 1;
    n++;
  }

  if (n > 1) {
    qsort(outf, n, sizeof(SoroushOutFrame), soroush_compare_out_frames);
  }

  bool ok = true;
  out->append_u2((u2)n);
  int previous_pc = -1;
  for (int i = 0; i < n && ok; i++) {
    if (i > 0 && outf[i].new_pc == outf[i - 1].new_pc) {
      *error = "duplicate StackMapTable frame pc after widening (fail-safe)";
      ok = false;
      break;
    }
    int delta = (i == 0) ? outf[i].new_pc : outf[i].new_pc - previous_pc - 1;
    if (delta < 0 || delta > 65535) {
      *error = "StackMapTable offset_delta out of range after widening (fail-safe)";
      ok = false;
      break;
    }
    previous_pc = outf[i].new_pc;
    out->append_u1(255); // full_frame
    out->append_u2((u2)delta);
    out->append_u2((u2)outf[i].locals_count);
    for (int j = 0; j < outf[i].locals_count; j++) {
      soroush_append_verification_type(out, &outf[i].locals[j]);
    }
    out->append_u2((u2)outf[i].stack_count);
    for (int j = 0; j < outf[i].stack_count; j++) {
      soroush_append_verification_type(out, &outf[i].stack[j]);
    }
  }
  if (ok && !out->ok()) {
    *error = "out of memory while emitting widened StackMapTable";
    ok = false;
  }

  free(outf);
  soroush_free_abs_frames(synth, synth_count);
  soroush_free_abs_frames(abs_frames, abs_count);
  return ok;
}

static bool soroush_transform_line_number_table_mapped(SoroushByteWriter* out,
                                                       const u1* body,
                                                       int length,
                                                       int code_len,
                                                       const int* pc_map,
                                                       const char** error) {
  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated LineNumberTable length");
  out->append_u2(entries);
  for (u2 i = 0; i < entries; i++) {
    u2 start_pc = r.get_u2("truncated LineNumberTable start_pc");
    u2 line = r.get_u2("truncated LineNumberTable line_number");
    int mapped = 0;
    if (!soroush_remap_pc(start_pc, code_len, pc_map, &mapped, error)) return false;
    out->append_u2((u2)mapped);
    out->append_u2(line);
  }
  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "LineNumberTable did not consume full input" : r.error();
    return false;
  }
  return out->ok();
}

static bool soroush_transform_local_variable_table_mapped(SoroushByteWriter* out,
                                                          const u1* body,
                                                          int length,
                                                          int code_len,
                                                          const int* pc_map,
                                                          const char** error) {
  SoroushClassReader r(body, length);
  u2 entries = r.get_u2("truncated local variable table length");
  out->append_u2(entries);
  for (u2 i = 0; i < entries; i++) {
    u2 start_pc = r.get_u2("truncated local variable start_pc");
    u2 len = r.get_u2("truncated local variable length");
    u2 name_index = r.get_u2("truncated local variable name_index");
    u2 descriptor_index = r.get_u2("truncated local variable descriptor_index");
    u2 index = r.get_u2("truncated local variable index");
    int mapped_start = 0;
    int mapped_end = 0;
    if (!soroush_remap_pc(start_pc, code_len, pc_map, &mapped_start, error) ||
        !soroush_remap_pc(start_pc + len, code_len, pc_map, &mapped_end, error)) {
      return false;
    }
    out->append_u2((u2)mapped_start);
    out->append_u2((u2)(mapped_end - mapped_start));
    out->append_u2(name_index);
    out->append_u2(descriptor_index);
    out->append_u2(index);
  }
  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "local variable table did not consume full input" : r.error();
    return false;
  }
  return out->ok();
}

static bool soroush_copy_annotation_element_value(SoroushByteWriter* out,
                                                  SoroushClassReader* r,
                                                  const char** error);

static bool soroush_copy_annotation(SoroushByteWriter* out,
                                    SoroushClassReader* r,
                                    const char** error) {
  u2 type_index = r->get_u2("truncated annotation type_index");
  u2 pairs = r->get_u2("truncated annotation element_value_pairs");
  out->append_u2(type_index);
  out->append_u2(pairs);
  for (u2 i = 0; i < pairs; i++) {
    u2 element_name_index = r->get_u2("truncated annotation element_name_index");
    out->append_u2(element_name_index);
    if (!soroush_copy_annotation_element_value(out, r, error)) {
      return false;
    }
  }
  if (!r->ok()) {
    *error = r->error();
    return false;
  }
  return out->ok();
}

static bool soroush_copy_annotation_element_value(SoroushByteWriter* out,
                                                  SoroushClassReader* r,
                                                  const char** error) {
  u1 tag = r->get_u1("truncated annotation element_value tag");
  out->append_u1(tag);
  switch (tag) {
    case 'B':
    case 'C':
    case 'D':
    case 'F':
    case 'I':
    case 'J':
    case 'S':
    case 'Z':
    case 's': {
      u2 const_value_index = r->get_u2("truncated annotation const_value_index");
      out->append_u2(const_value_index);
      break;
    }
    case 'e': {
      u2 type_name_index = r->get_u2("truncated annotation enum type_name_index");
      u2 const_name_index = r->get_u2("truncated annotation enum const_name_index");
      out->append_u2(type_name_index);
      out->append_u2(const_name_index);
      break;
    }
    case 'c': {
      u2 class_info_index = r->get_u2("truncated annotation class_info_index");
      out->append_u2(class_info_index);
      break;
    }
    case '@':
      return soroush_copy_annotation(out, r, error);
    case '[': {
      u2 values = r->get_u2("truncated annotation array length");
      out->append_u2(values);
      for (u2 i = 0; i < values; i++) {
        if (!soroush_copy_annotation_element_value(out, r, error)) {
          return false;
        }
      }
      break;
    }
    default:
      *error = "bad annotation element_value tag";
      return false;
  }
  if (!r->ok()) {
    *error = r->error();
    return false;
  }
  return out->ok();
}

static bool soroush_copy_type_path(SoroushByteWriter* out,
                                   SoroushClassReader* r,
                                   const char** error) {
  u1 path_len = r->get_u1("truncated type_path length");
  out->append_u1(path_len);
  for (u1 i = 0; i < path_len; i++) {
    u1 kind = r->get_u1("truncated type_path kind");
    u1 arg = r->get_u1("truncated type_path argument_index");
    out->append_u1(kind);
    out->append_u1(arg);
  }
  if (!r->ok()) {
    *error = r->error();
    return false;
  }
  return out->ok();
}

static bool soroush_remap_u2_pc(u2 old_pc,
                                int code_len,
                                const int* pc_map,
                                u2* new_pc,
                                const char** error) {
  int mapped = 0;
  if (!soroush_remap_pc(old_pc, code_len, pc_map, &mapped, error)) {
    return false;
  }
  if (mapped < 0 || mapped > 65535) {
    *error = "mapped bytecode pc out of u2 range";
    return false;
  }
  *new_pc = (u2)mapped;
  return true;
}

static bool soroush_transform_type_annotations_mapped(SoroushByteWriter* out,
                                                      const u1* body,
                                                      int length,
                                                      int code_len,
                                                      const int* pc_map,
                                                      const char** error) {
  SoroushClassReader r(body, length);
  u2 annotations = r.get_u2("truncated type annotations count");
  out->append_u2(annotations);

  for (u2 i = 0; i < annotations; i++) {
    u1 target_type = r.get_u1("truncated type annotation target_type");
    out->append_u1(target_type);

    switch (target_type) {
      case 0x00:
      case 0x01: {
        u1 type_parameter_index = r.get_u1("truncated type_parameter_target");
        out->append_u1(type_parameter_index);
        break;
      }
      case 0x10: {
        u2 supertype_index = r.get_u2("truncated supertype_target");
        out->append_u2(supertype_index);
        break;
      }
      case 0x11:
      case 0x12: {
        u1 type_parameter_index = r.get_u1("truncated type_parameter_bound_target");
        u1 bound_index = r.get_u1("truncated type_parameter_bound_target");
        out->append_u1(type_parameter_index);
        out->append_u1(bound_index);
        break;
      }
      case 0x13:
      case 0x14:
      case 0x15:
        break;
      case 0x16: {
        u1 formal_parameter_index = r.get_u1("truncated formal_parameter_target");
        out->append_u1(formal_parameter_index);
        break;
      }
      case 0x17: {
        u2 throws_type_index = r.get_u2("truncated throws_target");
        out->append_u2(throws_type_index);
        break;
      }
      case 0x40:
      case 0x41: {
        u2 table_length = r.get_u2("truncated localvar_target table_length");
        out->append_u2(table_length);
        for (u2 j = 0; j < table_length; j++) {
          u2 start_pc = r.get_u2("truncated localvar_target start_pc");
          u2 span = r.get_u2("truncated localvar_target length");
          u2 index = r.get_u2("truncated localvar_target index");
          int old_end = (int)start_pc + (int)span;
          if (old_end > 65535) {
            *error = "localvar_target range overflow";
            return false;
          }
          u2 mapped_start = 0;
          u2 mapped_end = 0;
          if (!soroush_remap_u2_pc(start_pc, code_len, pc_map, &mapped_start, error) ||
              !soroush_remap_u2_pc((u2)old_end, code_len, pc_map, &mapped_end, error)) {
            return false;
          }
          out->append_u2(mapped_start);
          out->append_u2((u2)(mapped_end - mapped_start));
          out->append_u2(index);
        }
        break;
      }
      case 0x42: {
        u2 exception_table_index = r.get_u2("truncated catch_target");
        out->append_u2(exception_table_index);
        break;
      }
      case 0x43:
      case 0x44:
      case 0x45:
      case 0x46: {
        u2 offset = r.get_u2("truncated offset_target");
        u2 mapped = 0;
        if (!soroush_remap_u2_pc(offset, code_len, pc_map, &mapped, error)) return false;
        out->append_u2(mapped);
        break;
      }
      case 0x47:
      case 0x48:
      case 0x49:
      case 0x4A:
      case 0x4B: {
        u2 offset = r.get_u2("truncated type_argument_target offset");
        u1 type_argument_index = r.get_u1("truncated type_argument_target index");
        u2 mapped = 0;
        if (!soroush_remap_u2_pc(offset, code_len, pc_map, &mapped, error)) return false;
        out->append_u2(mapped);
        out->append_u1(type_argument_index);
        break;
      }
      default:
        *error = "bad type annotation target_type";
        return false;
    }

    if (!soroush_copy_type_path(out, &r, error)) return false;
    if (!soroush_copy_annotation(out, &r, error)) return false;
  }

  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "type annotations did not consume full input" : r.error();
    return false;
  }
  return out->ok();
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

static bool soroush_transform_code_attribute(SoroushByteWriter* out,
                                             const SoroushCpInfo* cp,
                                             u2 cp_count,
                                             u2 attr_name,
                                             const u1* body,
                                             int length,
                                             int nop_count,
                                             int* decoded_instructions,
                                             const char** error) {
  SoroushClassReader r(body, length);
  u2 max_stack = r.get_u2("truncated Code max_stack");
  u2 max_locals = r.get_u2("truncated Code max_locals");
  u4 code_len = r.get_u4("truncated Code code_length");
  const u1* code = r.current();
  r.skip((int)code_len, "truncated Code bytes");
  if (!r.ok()) {
    *error = r.error();
    return false;
  }
  if (code_len > 65535 || code_len + nop_count > 65535) {
    *error = "Code attribute too large for entry insertion";
    return false;
  }
  if (!soroush_decode_code(code, (int)code_len, decoded_instructions, error)) {
    return false;
  }

  SoroushByteWriter code_attr;
  code_attr.append_u2(max_stack);
  code_attr.append_u2(max_locals);
  code_attr.append_u4(code_len + nop_count);
  for (int i = 0; i < nop_count; i++) {
    code_attr.append_u1(0x00); // nop
  }
  code_attr.append_bytes(code, (int)code_len);

  u2 exception_table_len = r.get_u2("truncated exception table length");
  code_attr.append_u2(exception_table_len);
  for (u2 i = 0; i < exception_table_len; i++) {
    u2 start_pc = r.get_u2("truncated exception start_pc");
    u2 end_pc = r.get_u2("truncated exception end_pc");
    u2 handler_pc = r.get_u2("truncated exception handler_pc");
    u2 catch_type = r.get_u2("truncated exception catch_type");
    if (start_pc + nop_count > 65535 ||
        end_pc + nop_count > 65535 ||
        handler_pc + nop_count > 65535) {
      *error = "exception table pc overflow";
      return false;
    }
    code_attr.append_u2((u2)(start_pc + nop_count));
    code_attr.append_u2((u2)(end_pc + nop_count));
    code_attr.append_u2((u2)(handler_pc + nop_count));
    code_attr.append_u2(catch_type);
  }

  u2 code_attr_count = r.get_u2("truncated Code attributes_count");
  code_attr.append_u2(code_attr_count);
  for (u2 i = 0; i < code_attr_count; i++) {
    u2 code_attr_name = r.get_u2("truncated Code attribute name_index");
    u4 code_attr_len = r.get_u4("truncated Code attribute length");
    const u1* code_attr_body = r.current();
    r.skip((int)code_attr_len, "truncated Code attribute body");
    if (!r.ok()) {
      *error = r.error();
      return false;
    }

    SoroushByteWriter transformed_attr;
    bool transformed = false;
    if (soroush_utf8_equals(cp, cp_count, code_attr_name, "StackMapTable")) {
      transformed = true;
      if (!soroush_transform_stack_map_table(&transformed_attr, code_attr_body,
                                             (int)code_attr_len, nop_count, error)) {
        return false;
      }
    } else if (soroush_utf8_equals(cp, cp_count, code_attr_name, "LineNumberTable")) {
      transformed = true;
      if (!soroush_transform_line_number_table(&transformed_attr, code_attr_body,
                                               (int)code_attr_len, nop_count, error)) {
        return false;
      }
    } else if (soroush_utf8_equals(cp, cp_count, code_attr_name, "LocalVariableTable") ||
               soroush_utf8_equals(cp, cp_count, code_attr_name, "LocalVariableTypeTable")) {
      transformed = true;
      if (!soroush_transform_local_variable_table(&transformed_attr, code_attr_body,
                                                  (int)code_attr_len, nop_count, error)) {
        return false;
      }
    }

    code_attr.append_u2(code_attr_name);
    if (transformed) {
      code_attr.append_u4((u4)transformed_attr.length());
      code_attr.append_bytes(transformed_attr.bytes(), transformed_attr.length());
    } else {
      code_attr.append_u4(code_attr_len);
      code_attr.append_bytes(code_attr_body, (int)code_attr_len);
    }
    if (!code_attr.ok()) {
      *error = "out of memory while transforming Code attribute";
      return false;
    }
  }

  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "Code attribute did not consume full input" : r.error();
    return false;
  }
  if (!code_attr.ok()) {
    *error = "out of memory while building Code attribute";
    return false;
  }

  out->append_u2(attr_name);
  out->append_u4((u4)code_attr.length());
  out->append_bytes(code_attr.bytes(), code_attr.length());
  return out->ok();
}

static bool soroush_transform_code_attribute_entry_code(SoroushByteWriter* out,
                                                        const SoroushCpInfo* cp,
                                                        u2 cp_count,
                                                        u2 attr_name,
                                                        const u1* body,
                                                        int length,
                                                        const u1* entry_code,
                                                        int entry_len,
                                                        u2 method_access_flags,
                                                        u2 method_descriptor_index,
                                                        u2 this_class_index,
                                                        u2 object_class_index,
                                                        bool insert_exits,
                                                        u2 exit_string_index,
                                                        u2 trace_methodref,
                                                        u2 throwable_class_index,
                                                        u2 stack_map_table_name_index,
                                                        const char* method_id,
                                                        bool is_constructor,
                                                        bool* method_skipped,
                                                        int* decoded_instructions,
                                                        const char** error) {
  *method_skipped = false;
  SoroushClassReader r(body, length);
  u2 max_stack = r.get_u2("truncated Code max_stack");
  u2 max_locals = r.get_u2("truncated Code max_locals");
  u4 code_len_u4 = r.get_u4("truncated Code code_length");
  if (code_len_u4 > 65535) {
    *error = "Code attribute too large";
    return false;
  }
  int code_len = (int)code_len_u4;
  const u1* code = r.current();
  r.skip(code_len, "truncated Code bytes");
  if (!r.ok()) {
    *error = r.error();
    return false;
  }

  // Constructor handling: ENTER may run only after `this` is initialized, i.e.
  // after the super()/this() delegation invokespecial. Locate it conservatively
  // and safe-skip (copy the method unchanged) anything we cannot prove safe.
  // Nothing is written to `out` before this point, so a safe-skip return leaves
  // the caller free to copy the original method bytes.
  int enter_after_old_pc = -1;
  if (is_constructor) {
    fprintf(stderr, "[JVM REWRITER CTOR] method=%s considered\n",
            method_id == nullptr ? "<unknown>" : method_id);
    bool delegation_is_this = false;
    bool ambiguous = false;
    int scan_pc = 0;
    while (scan_pc < code_len) {
      const char* scan_err = nullptr;
      int ins_len = soroush_instruction_length(code, code_len, scan_pc, &scan_err);
      if (ins_len <= 0) { ambiguous = true; break; }
      u1 op = code[scan_pc];
      if (op == 0xbb) { // `new`: a pre-super object construction makes the
        ambiguous = true; // first invokespecial <init> ambiguous -> safe-skip
        break;
      }
      if (op == 0xb7) { // invokespecial
        u2 mref = soroush_read_u2(code + scan_pc + 1);
        bool is_this = false;
        if (soroush_methodref_is_init(cp, cp_count, mref, this_class_index, &is_this)) {
          enter_after_old_pc = scan_pc;
          delegation_is_this = is_this;
        } else {
          ambiguous = true; // invokespecial to a non-<init> before delegation
        }
        break;
      }
      scan_pc += ins_len;
    }
    if (enter_after_old_pc < 0 || ambiguous) {
      *method_skipped = true;
      fprintf(stderr, "[JVM REWRITER CTOR] method=%s delegation=not-found safe-skip reason=%s\n",
              method_id == nullptr ? "<unknown>" : method_id,
              ambiguous ? "pre-delegation-new-or-nonstandard-invokespecial"
                        : "no-init-delegation-found");
      return true; // caller copies the method unchanged
    }
    fprintf(stderr,
            "[JVM REWRITER CTOR] method=%s delegation=%s delegation_old_pc=%d\n",
            method_id == nullptr ? "<unknown>" : method_id,
            delegation_is_this ? "this" : "super", enter_after_old_pc);
  }

  bool method_has_stack_map_table = false;
  const u1* stack_map_table_body = nullptr;
  int stack_map_table_len = 0;
  bool has_unknown_code_attr = false;
  u2 unknown_code_attr_name = 0;
  u4 unknown_code_attr_len = 0;
  {
    SoroushClassReader attr_scan(r.current(), (int)(body + length - r.current()));
    u2 exception_table_len = attr_scan.get_u2("truncated exception table length");
    attr_scan.skip(exception_table_len * 8, "truncated exception table");
    u2 code_attr_count = attr_scan.get_u2("truncated Code attributes_count");
    for (u2 i = 0; i < code_attr_count && attr_scan.ok(); i++) {
      u2 code_attr_name = attr_scan.get_u2("truncated Code attribute name_index");
      u4 code_attr_len = attr_scan.get_u4("truncated Code attribute length");
      if (soroush_utf8_equals(cp, cp_count, code_attr_name, "StackMapTable")) {
        method_has_stack_map_table = true;
        stack_map_table_body = attr_scan.current();
        stack_map_table_len = (int)code_attr_len;
      }
      // Audit: any Code sub-attribute outside the known PC-bearing set is
      // non-standard. We cannot know whether it embeds bytecode PCs, so after
      // shifting PCs we must not copy it blindly -- record it for a safe-skip.
      if (!soroush_is_known_code_attr(cp, cp_count, code_attr_name) && !has_unknown_code_attr) {
        has_unknown_code_attr = true;
        unknown_code_attr_name = code_attr_name;
        unknown_code_attr_len = code_attr_len;
      }
      attr_scan.skip((int)code_attr_len, "truncated Code attribute body");
    }
    if (!attr_scan.ok()) {
      *error = attr_scan.error();
      return false;
    }
  }

  // Conservative Code-attribute hardening: an unknown Code sub-attribute may
  // carry bytecode PCs we cannot remap. Rather than emit a transformed Code
  // attribute with a stale copied sub-attribute, safe-skip this method (copy it
  // unchanged). Nothing has been written to `out` yet.
  if (has_unknown_code_attr) {
    *method_skipped = true;
    const char* attr_name_str = "<unresolved>";
    int attr_name_len = 11;
    if (unknown_code_attr_name < cp_count && cp[unknown_code_attr_name].tag == 1) {
      attr_name_str = cp[unknown_code_attr_name].utf8;
      attr_name_len = cp[unknown_code_attr_name].utf8_len;
    }
    fprintf(stderr,
            "[JVM REWRITER CODEATTR] method=%s attr=%.*s len=%u decision=safe-skip "
            "reason=unknown-code-sub-attribute-may-bear-pcs\n",
            method_id == nullptr ? "<unknown>" : method_id,
            attr_name_len, attr_name_str, (unsigned)unknown_code_attr_len);
    return true; // caller copies the method unchanged
  }

  SoroushInstruction* instructions = nullptr;
  int instruction_count = 0;
  int* pc_map = nullptr;
  int new_code_len = 0;
  int temp_local = max_locals;
  if (insert_exits && max_locals > 65533) {
    *error = "not enough local variable slots for EXIT insertion";
    return false;
  }
  if (!soroush_build_pc_map(code, code_len, entry_len, insert_exits, temp_local,
                            is_constructor, enter_after_old_pc, &instructions,
                            &instruction_count, &pc_map, &new_code_len,
                            decoded_instructions, error)) {
    return false;
  }

  // Phase 6A/6B: stabilize the layout for branch widening BEFORE any emission
  // or metadata remapping, so all consumers share one stable layout.
  {
    SoroushBranchWideningCandidate* candidates = nullptr;
    int candidate_count = 0;
    int det_direct = 0;
    int det_invert = 0;
    if (!soroush_detect_branch_widening(code, code_len, instructions, instruction_count,
                                        pc_map, &candidates, &candidate_count,
                                        &det_direct, &det_invert, error)) {
      free(candidates);
      free(instructions);
      free(pc_map);
      return false;
    }
    if (candidate_count > 0) {
      // Phase 6A: report which base-layout branches overflow and their kinds.
      soroush_log_branch_widening(candidates, candidate_count, det_direct, det_invert);
      free(candidates);
      candidates = nullptr;

      // Phase 6B: relax to a fixed point; on success the stable layout is
      // written back into instructions[] / pc_map[].
      bool converged = false;
      int conv_widened = 0;
      int conv_direct = 0;
      int conv_invert = 0;
      int conv_iterations = 0;
      int conv_final_len = 0;
      if (!soroush_converge_layout(code, code_len, entry_len, insert_exits, temp_local,
                                   is_constructor, enter_after_old_pc,
                                   instructions, instruction_count, pc_map,
                                   &converged, &conv_widened, &conv_direct,
                                   &conv_invert, &conv_iterations,
                                   &conv_final_len, error)) {
        free(instructions);
        free(pc_map);
        return false; // hard error (OOM / malformed) -> fail safe
      }
      if (!converged) {
        free(instructions);
        free(pc_map);
        *error = "branch layout did not converge under widening (fail-safe)";
        return false;
      }
      // Phase 6B (goto_w/jsr_w) and Phase 6C (conditional inversion + goto_w)
      // are both implemented. Direct and invert widenings are emitted from the
      // stabilized layout; any unsupported shape fails safe downstream (emit or
      // StackMapTable synthesis). Adopt the converged length for the guard.
      (void)conv_direct;
      (void)conv_invert;
      new_code_len = conv_final_len;
    } else {
      free(candidates);
    }
  }

  bool method_has_athrow = false;
  for (int i = 0; i < instruction_count; i++) {
    if (instructions[i].op == 0xbf) {
      method_has_athrow = true;
      break;
    }
  }
  SoroushExceptionExitHandler* exception_handlers = nullptr;
  int exception_handler_count = 0;
  // Exception-path EXIT (synthetic catch-all handlers) is intentionally NOT
  // applied to constructors: a handler covering pre-delegation code would need
  // uninitializedThis in its frame, which is not verifier-representable here.
  if (insert_exits && method_has_stack_map_table && !is_constructor) {
    const char* descriptor = method_descriptor_index < cp_count ? cp[method_descriptor_index].utf8 : nullptr;
    int descriptor_len = method_descriptor_index < cp_count ? cp[method_descriptor_index].utf8_len : 0;
    if (!soroush_build_stack_map_exception_handlers(stack_map_table_body,
                                                    stack_map_table_len,
                                                    instructions,
                                                    instruction_count,
                                                    code,
                                                    code_len,
                                                    max_locals,
                                                    (method_access_flags & 0x0008) != 0,
                                                    this_class_index,
                                                    object_class_index,
                                                    descriptor,
                                                    descriptor_len,
                                                    method_id,
                                                    &exception_handlers,
                                                    &exception_handler_count,
                                                    error)) {
      free(instructions);
      free(pc_map);
      return false;
    }
  }
  bool catch_all_exits = insert_exits && !method_has_stack_map_table &&
                         !method_has_athrow && !is_constructor;
  if (new_code_len > 65535) {
    free(instructions);
    free(pc_map);
    soroush_free_exception_handlers(exception_handlers, exception_handler_count);
    *error = "rewritten Code attribute too large";
    return false;
  }

  SoroushByteWriter new_code;
  if (!soroush_emit_rewritten_code(&new_code, entry_code, entry_len,
                                   insert_exits, temp_local,
                                   is_constructor, enter_after_old_pc,
                                   exit_string_index, trace_methodref,
                                   code, code_len,
                                   instructions, instruction_count, pc_map, error)) {
    free(instructions);
    free(pc_map);
    soroush_free_exception_handlers(exception_handlers, exception_handler_count);
    return false;
  }
  int exception_handler_pc = -1;
  if (catch_all_exits) {
    exception_handler_pc = new_code.length();
    if (!soroush_append_exception_exit_handler(&new_code, temp_local,
                                               exit_string_index,
                                               trace_methodref, error)) {
      free(instructions);
      free(pc_map);
      soroush_free_exception_handlers(exception_handlers, exception_handler_count);
      return false;
    }
  }
  for (int i = 0; i < exception_handler_count; i++) {
    exception_handlers[i].handler_pc = new_code.length();
    if (!soroush_append_exception_exit_handler(&new_code, temp_local,
                                               exit_string_index,
                                               trace_methodref, error)) {
      free(instructions);
      free(pc_map);
      soroush_free_exception_handlers(exception_handlers, exception_handler_count);
      return false;
    }
  }
  if (new_code.length() > 65535) {
    free(instructions);
    free(pc_map);
    soroush_free_exception_handlers(exception_handlers, exception_handler_count);
    *error = "rewritten Code attribute too large";
    return false;
  }

  SoroushByteWriter code_attr;
  code_attr.append_u2(max_stack < 1 ? 1 : max_stack);
  code_attr.append_u2(insert_exits ? (u2)(max_locals + 2) : max_locals);
  code_attr.append_u4((u4)new_code.length());
  code_attr.append_bytes(new_code.bytes(), new_code.length());

  u2 exception_table_len = r.get_u2("truncated exception table length");
  if ((catch_all_exits || exception_handler_count > 0) &&
      exception_table_len > 65535 - exception_handler_count - (catch_all_exits ? 1 : 0)) {
    free(instructions);
    free(pc_map);
    soroush_free_exception_handlers(exception_handlers, exception_handler_count);
    *error = "exception table too large for exception EXIT handler";
    return false;
  }
  code_attr.append_u2((u2)(exception_table_len + exception_handler_count + (catch_all_exits ? 1 : 0)));
  for (u2 i = 0; i < exception_table_len; i++) {
    u2 start_pc = r.get_u2("truncated exception start_pc");
    u2 end_pc = r.get_u2("truncated exception end_pc");
    u2 handler_pc = r.get_u2("truncated exception handler_pc");
    u2 catch_type = r.get_u2("truncated exception catch_type");
    int mapped_start = 0;
    int mapped_end = 0;
    int mapped_handler = 0;
    if (!soroush_remap_pc(start_pc, code_len, pc_map, &mapped_start, error) ||
        !soroush_remap_pc(end_pc, code_len, pc_map, &mapped_end, error) ||
        !soroush_remap_pc(handler_pc, code_len, pc_map, &mapped_handler, error)) {
      free(instructions);
      free(pc_map);
      soroush_free_exception_handlers(exception_handlers, exception_handler_count);
      return false;
    }
    code_attr.append_u2((u2)mapped_start);
    code_attr.append_u2((u2)mapped_end);
    code_attr.append_u2((u2)mapped_handler);
    code_attr.append_u2(catch_type);
  }
  if (catch_all_exits) {
    code_attr.append_u2(0);
    code_attr.append_u2((u2)exception_handler_pc);
    code_attr.append_u2((u2)exception_handler_pc);
    code_attr.append_u2(0);
  }
  for (int i = 0; i < exception_handler_count; i++) {
    int mapped_start = 0;
    int mapped_end = 0;
    if (!soroush_remap_pc(exception_handlers[i].old_start_pc, code_len, pc_map, &mapped_start, error) ||
        !soroush_remap_pc(exception_handlers[i].old_end_pc, code_len, pc_map, &mapped_end, error)) {
      free(instructions);
      free(pc_map);
      soroush_free_exception_handlers(exception_handlers, exception_handler_count);
      return false;
    }
    if (mapped_start < mapped_end) {
      code_attr.append_u2((u2)mapped_start);
      code_attr.append_u2((u2)mapped_end);
      code_attr.append_u2((u2)exception_handlers[i].handler_pc);
      code_attr.append_u2(0);
    }
  }

  u2 code_attr_count = r.get_u2("truncated Code attributes_count");
  int code_attr_count_pos = code_attr.length();
  code_attr.append_u2(0);
  u2 new_code_attr_count = code_attr_count;
  bool saw_stack_map_table = false;
  for (u2 i = 0; i < code_attr_count; i++) {
    u2 code_attr_name = r.get_u2("truncated Code attribute name_index");
    u4 code_attr_len = r.get_u4("truncated Code attribute length");
    const u1* code_attr_body = r.current();
    r.skip((int)code_attr_len, "truncated Code attribute body");
    if (!r.ok()) {
      *error = r.error();
      free(instructions);
      free(pc_map);
      soroush_free_exception_handlers(exception_handlers, exception_handler_count);
      return false;
    }

    SoroushByteWriter transformed_attr;
    bool transformed = false;
    if (soroush_utf8_equals(cp, cp_count, code_attr_name, "StackMapTable")) {
      transformed = true;
      saw_stack_map_table = true;
      const char* smt_descriptor = method_descriptor_index < cp_count ? cp[method_descriptor_index].utf8 : nullptr;
      int smt_descriptor_len = method_descriptor_index < cp_count ? cp[method_descriptor_index].utf8_len : 0;
      if (!soroush_transform_stack_map_table_mapped(&transformed_attr, code_attr_body,
                                                    (int)code_attr_len, code, code_len,
                                                    pc_map,
                                                    instructions, instruction_count,
                                                    max_locals, max_stack,
                                                    (method_access_flags & 0x0008) != 0,
                                                    this_class_index, object_class_index,
                                                    smt_descriptor, smt_descriptor_len,
                                                    exception_handlers,
                                                    exception_handler_count,
                                                    throwable_class_index,
                                                    error)) {
        free(instructions);
        free(pc_map);
        soroush_free_exception_handlers(exception_handlers, exception_handler_count);
        return false;
      }
    } else if (soroush_utf8_equals(cp, cp_count, code_attr_name, "LineNumberTable")) {
      transformed = true;
      if (!soroush_transform_line_number_table_mapped(&transformed_attr, code_attr_body,
                                                      (int)code_attr_len, code_len,
                                                      pc_map, error)) {
        free(instructions);
        free(pc_map);
        soroush_free_exception_handlers(exception_handlers, exception_handler_count);
        return false;
      }
    } else if (soroush_utf8_equals(cp, cp_count, code_attr_name, "LocalVariableTable") ||
               soroush_utf8_equals(cp, cp_count, code_attr_name, "LocalVariableTypeTable")) {
      transformed = true;
      if (!soroush_transform_local_variable_table_mapped(&transformed_attr, code_attr_body,
                                                         (int)code_attr_len, code_len,
                                                         pc_map, error)) {
        free(instructions);
        free(pc_map);
        soroush_free_exception_handlers(exception_handlers, exception_handler_count);
        return false;
      }
    } else if (soroush_utf8_equals(cp, cp_count, code_attr_name, "RuntimeVisibleTypeAnnotations") ||
               soroush_utf8_equals(cp, cp_count, code_attr_name, "RuntimeInvisibleTypeAnnotations")) {
      transformed = true;
      if (!soroush_transform_type_annotations_mapped(&transformed_attr, code_attr_body,
                                                     (int)code_attr_len, code_len,
                                                     pc_map, error)) {
        free(instructions);
        free(pc_map);
        soroush_free_exception_handlers(exception_handlers, exception_handler_count);
        return false;
      }
    }

    code_attr.append_u2(code_attr_name);
    if (transformed) {
      code_attr.append_u4((u4)transformed_attr.length());
      code_attr.append_bytes(transformed_attr.bytes(), transformed_attr.length());
    } else {
      code_attr.append_u4(code_attr_len);
      code_attr.append_bytes(code_attr_body, (int)code_attr_len);
    }
  }
  if (catch_all_exits && !saw_stack_map_table) {
    SoroushByteWriter synthetic_stack_map;
    if (!soroush_append_single_exception_handler_stack_map(&synthetic_stack_map,
                                                           exception_handler_pc,
                                                           throwable_class_index,
                                                           error)) {
      free(instructions);
      free(pc_map);
      soroush_free_exception_handlers(exception_handlers, exception_handler_count);
      return false;
    }
    if (new_code_attr_count == 65535) {
      free(instructions);
      free(pc_map);
      soroush_free_exception_handlers(exception_handlers, exception_handler_count);
      *error = "Code attributes too large for synthetic StackMapTable";
      return false;
    }
    code_attr.append_u2(stack_map_table_name_index);
    code_attr.append_u4((u4)synthetic_stack_map.length());
    code_attr.append_bytes(synthetic_stack_map.bytes(), synthetic_stack_map.length());
    new_code_attr_count++;
  }
  code_attr.patch_u2(code_attr_count_pos, new_code_attr_count);

  if (is_constructor) {
    int deleg_new_pc = (enter_after_old_pc >= 0 && enter_after_old_pc <= code_len)
        ? pc_map[enter_after_old_pc] : -1;
    fprintf(stderr,
            "[JVM REWRITER CTOR] method=%s instrumented enter_new_pc=%d normal_exit=instrumented exception_exit=disabled\n",
            method_id == nullptr ? "<unknown>" : method_id,
            deleg_new_pc < 0 ? -1 : deleg_new_pc + 3); // ENTER follows the 3-byte invokespecial
  }

  free(instructions);
  free(pc_map);
  soroush_free_exception_handlers(exception_handlers, exception_handler_count);

  if (!r.ok() || r.current() != body + length) {
    *error = r.ok() ? "Code attribute did not consume full input" : r.error();
    return false;
  }
  if (!code_attr.ok()) {
    *error = "out of memory while building Code attribute";
    return false;
  }

  out->append_u2(attr_name);
  out->append_u4((u4)code_attr.length());
  out->append_bytes(code_attr.bytes(), code_attr.length());
  return out->ok();
}

static bool soroush_transform_member_entry_nops(SoroushByteWriter* out,
                                                SoroushClassReader* r,
                                                const SoroushCpInfo* cp,
                                                u2 cp_count,
                                                int nop_count,
                                                int* transformed_methods,
                                                int* decoded_instructions,
                                                const char** error) {
  u2 access_flags = r->get_u2("truncated member access_flags");
  u2 name_index = r->get_u2("truncated member name_index");
  u2 descriptor_index = r->get_u2("truncated member descriptor_index");
  u2 attr_count = r->get_u2("truncated member attributes_count");
  if (!r->ok()) {
    *error = r->error();
    return false;
  }

  out->append_u2(access_flags);
  out->append_u2(name_index);
  out->append_u2(descriptor_index);
  out->append_u2(attr_count);

  bool transformed_code = false;
  for (u2 i = 0; i < attr_count; i++) {
    u2 attr_name = r->get_u2("truncated attribute name_index");
    u4 attr_len = r->get_u4("truncated attribute length");
    const u1* attr_body = r->current();
    r->skip((int)attr_len, "truncated attribute body");
    if (!r->ok()) {
      *error = r->error();
      return false;
    }

    if (soroush_utf8_equals(cp, cp_count, attr_name, "Code")) {
      if (!soroush_transform_code_attribute(out, cp, cp_count, attr_name,
                                            attr_body, (int)attr_len, nop_count,
                                            decoded_instructions, error)) {
        return false;
      }
      transformed_code = true;
    } else {
      out->append_u2(attr_name);
      out->append_u4(attr_len);
      out->append_bytes(attr_body, (int)attr_len);
    }
    if (!out->ok()) {
      *error = "out of memory while transforming member";
      return false;
    }
  }

  if (transformed_code) {
    (*transformed_methods)++;
  }
  return out->ok();
}

static void soroush_append_cp_utf8(SoroushByteWriter* out, const char* s) {
  int len = (int)strlen(s);
  out->append_u1(1);
  out->append_u2((u2)len);
  out->append_bytes((const u1*)s, len);
}

static void soroush_append_cp_class(SoroushByteWriter* out, u2 name_index) {
  out->append_u1(7);
  out->append_u2(name_index);
}

static void soroush_append_cp_string(SoroushByteWriter* out, u2 utf8_index) {
  out->append_u1(8);
  out->append_u2(utf8_index);
}

static void soroush_append_cp_name_and_type(SoroushByteWriter* out, u2 name_index, u2 desc_index) {
  out->append_u1(12);
  out->append_u2(name_index);
  out->append_u2(desc_index);
}

static void soroush_append_cp_methodref(SoroushByteWriter* out, u2 class_index, u2 nt_index) {
  out->append_u1(10);
  out->append_u2(class_index);
  out->append_u2(nt_index);
}

static bool soroush_transform_member_entry_trace(SoroushByteWriter* out,
                                                 SoroushClassReader* r,
                                                 const SoroushCpInfo* cp,
                                                 u2 cp_count,
                                                 const SoroushMethodTracePlan* plan,
                                                 u2 trace_methodref,
                                                 u2 throwable_class_index,
                                                 u2 stack_map_table_name_index,
                                                 u2 this_class_index,
                                                 u2 object_class_index,
                                                 bool insert_exits,
                                                 int* transformed_methods,
                                                 int* decoded_instructions,
                                                 const char** error) {
  u2 access_flags = r->get_u2("truncated member access_flags");
  u2 name_index = r->get_u2("truncated member name_index");
  u2 descriptor_index = r->get_u2("truncated member descriptor_index");
  u2 attr_count = r->get_u2("truncated member attributes_count");
  if (!r->ok()) {
    *error = r->error();
    return false;
  }

  out->append_u2(access_flags);
  out->append_u2(name_index);
  out->append_u2(descriptor_index);
  out->append_u2(attr_count);

  u1 entry_code[6];
  entry_code[0] = 0x13; // ldc_w
  soroush_write_u2(entry_code + 1, plan->enter_string_index);
  entry_code[3] = 0xb8; // invokestatic
  soroush_write_u2(entry_code + 4, trace_methodref);

  bool transformed_code = false;
  for (u2 i = 0; i < attr_count; i++) {
    u2 attr_name = r->get_u2("truncated attribute name_index");
    u4 attr_len = r->get_u4("truncated attribute length");
    const u1* attr_body = r->current();
    r->skip((int)attr_len, "truncated attribute body");
    if (!r->ok()) {
      *error = r->error();
      return false;
    }

    if (plan->rewrite && soroush_utf8_equals(cp, cp_count, attr_name, "Code")) {
      // plan->enter_msg is "ENTER <dotted-class>.<method>"; +6 skips "ENTER ".
      const char* method_id = plan->enter_msg + 6;
      bool method_skipped = false;
      if (!soroush_transform_code_attribute_entry_code(out, cp, cp_count, attr_name,
                                                       attr_body, (int)attr_len,
                                                       entry_code, sizeof(entry_code),
                                                       access_flags,
                                                       descriptor_index,
                                                       this_class_index,
                                                       object_class_index,
                                                       insert_exits,
                                                       plan->exit_string_index,
                                                       trace_methodref,
                                                       throwable_class_index,
                                                       stack_map_table_name_index,
                                                       method_id,
                                                       plan->is_constructor,
                                                       &method_skipped,
                                                       decoded_instructions, error)) {
        return false;
      }
      if (method_skipped) {
        // Constructor (or other) shape we cannot instrument safely: copy the
        // original Code attribute unchanged so the rest of the class still gets
        // instrumented (per-method safe-skip, no class-level fallback).
        out->append_u2(attr_name);
        out->append_u4(attr_len);
        out->append_bytes(attr_body, (int)attr_len);
      } else {
        transformed_code = true;
      }
    } else {
      out->append_u2(attr_name);
      out->append_u4(attr_len);
      out->append_bytes(attr_body, (int)attr_len);
    }
    if (!out->ok()) {
      *error = "out of memory while transforming member";
      return false;
    }
  }

  if (transformed_code) {
    (*transformed_methods)++;
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

SoroushClassfileRewriter::TransformResult
SoroushClassfileRewriter::insert_entry_nops(const u1* bytes, int length, int nop_count) {
  TransformResult result;
  result.ok = false;
  result.bytes = nullptr;
  result.length = 0;
  result.error = "not started";
  result.transformed_methods = 0;
  result.decoded_instructions = 0;
  result.code_methods = 0;
  result.constructor_methods = 0;

  if (bytes == nullptr || length <= 0) {
    result.error = "empty classfile";
    return result;
  }
  if (nop_count <= 0 || (nop_count & 3) != 0) {
    result.error = "entry nop count must be a positive multiple of 4";
    return result;
  }

  SoroushClassReader r(bytes, length);
  SoroushByteWriter out;

  u4 magic = r.get_u4("truncated magic");
  if (magic != 0xCAFEBABE) {
    result.error = "bad classfile magic";
    return result;
  }
  out.append_u4(magic);
  u2 minor = r.get_u2("truncated minor_version");
  u2 major = r.get_u2("truncated major_version");
  u2 cp_count = r.get_u2("truncated constant_pool_count");
  if (!r.ok()) {
    result.error = r.error();
    return result;
  }
  out.append_u2(minor);
  out.append_u2(major);
  out.append_u2(cp_count);

  SoroushCpInfo* cp = (SoroushCpInfo*)calloc(cp_count, sizeof(SoroushCpInfo));
  if (cp == nullptr) {
    result.error = "out of memory";
    return result;
  }

  for (u2 i = 1; i < cp_count; i++) {
    const u1* entry_start = r.current();
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
    out.append_bytes(entry_start, (int)(r.current() - entry_start));
  }

  const u1* fixed_start = r.current();
  r.skip(2, "truncated access_flags");
  r.skip(2, "truncated this_class");
  r.skip(2, "truncated super_class");
  u2 interfaces_count = r.get_u2("truncated interfaces_count");
  r.skip(interfaces_count * 2, "truncated interfaces");
  if (!r.ok()) {
    free(cp);
    result.error = r.error();
    return result;
  }
  out.append_bytes(fixed_start, (int)(r.current() - fixed_start));

  u2 fields_count = r.get_u2("truncated fields_count");
  if (!r.ok()) {
    free(cp);
    result.error = r.error();
    return result;
  }
  out.append_u2(fields_count);
  for (u2 i = 0; i < fields_count; i++) {
    const u1* field_start = r.current();
    const char* error = nullptr;
    int decoded_methods = 0;
    if (!soroush_skip_member(&r, cp, cp_count, &decoded_methods,
                             &result.decoded_instructions, &error)) {
      free(cp);
      result.error = error == nullptr ? "bad field" : error;
      return result;
    }
    out.append_bytes(field_start, (int)(r.current() - field_start));
  }

  u2 methods_count = r.get_u2("truncated methods_count");
  if (!r.ok()) {
    free(cp);
    result.error = r.error();
    return result;
  }
  out.append_u2(methods_count);
  for (u2 i = 0; i < methods_count; i++) {
    const char* error = nullptr;
    if (!soroush_transform_member_entry_nops(&out, &r, cp, cp_count, nop_count,
                                             &result.transformed_methods,
                                             &result.decoded_instructions,
                                             &error)) {
      free(cp);
      result.error = error == nullptr ? "bad method" : error;
      return result;
    }
  }

  const u1* class_attrs_start = r.current();
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
  out.append_bytes(class_attrs_start, (int)(r.current() - class_attrs_start));

  free(cp);
  if (!out.ok()) {
    result.error = "out of memory while transforming classfile";
    return result;
  }

  result.length = out.length();
  result.bytes = out.release();
  result.ok = true;
  result.error = nullptr;
  return result;
}

static SoroushClassfileRewriter::TransformResult
soroush_insert_trace(const u1* bytes, int length, const char* class_name, bool insert_exits) {
  SoroushClassfileRewriter::TransformResult result;
  result.ok = false;
  result.bytes = nullptr;
  result.length = 0;
  result.error = "not started";
  result.transformed_methods = 0;
  result.decoded_instructions = 0;
  result.code_methods = 0;
  result.constructor_methods = 0;

  if (bytes == nullptr || length <= 0) {
    result.error = "empty classfile";
    return result;
  }
  if (class_name == nullptr) {
    class_name = "<unknown>";
  }

  SoroushClassReader scan(bytes, length);
  if (scan.get_u4("truncated magic") != 0xCAFEBABE) {
    result.error = "bad classfile magic";
    return result;
  }
  scan.skip(2, "truncated minor_version");
  scan.skip(2, "truncated major_version");
  u2 old_cp_count = scan.get_u2("truncated constant_pool_count");
  if (!scan.ok()) {
    result.error = scan.error();
    return result;
  }

  const u1* cp_start = scan.current();
  SoroushCpInfo* cp = (SoroushCpInfo*)calloc(old_cp_count, sizeof(SoroushCpInfo));
  if (cp == nullptr) {
    result.error = "out of memory";
    return result;
  }

  for (u2 i = 1; i < old_cp_count; i++) {
    u1 tag = scan.get_u1("truncated constant pool tag");
    cp[i].tag = tag;
    switch (tag) {
      case 1: {
        u2 len = scan.get_u2("truncated Utf8 length");
        cp[i].utf8 = (const char*)scan.current();
        cp[i].utf8_len = len;
        scan.skip(len, "truncated Utf8 bytes");
        break;
      }
      case 3:
      case 4:
        scan.skip(4, "truncated constant pool entry");
        break;
      case 5:
      case 6:
        scan.skip(8, "truncated wide constant pool entry");
        i++;
        break;
      case 7:               // Class: name_index
      case 19:              // Module
      case 20:              // Package
        cp[i].index1 = scan.get_u2("truncated constant pool entry");
        break;
      case 8:               // String
      case 16:              // MethodType
        scan.skip(2, "truncated constant pool entry");
        break;
      case 9:               // Fieldref
      case 10:              // Methodref
      case 11:              // InterfaceMethodref
      case 12:              // NameAndType
        cp[i].index1 = scan.get_u2("truncated constant pool entry");
        cp[i].index2 = scan.get_u2("truncated constant pool entry");
        break;
      case 17:              // Dynamic
      case 18:              // InvokeDynamic
        scan.skip(4, "truncated constant pool entry");
        break;
      case 15:
        scan.skip(3, "truncated MethodHandle constant pool entry");
        break;
      default:
        free(cp);
        result.error = "unknown constant pool tag";
        return result;
    }
    if (!scan.ok()) {
      free(cp);
      result.error = scan.error();
      return result;
    }
  }
  const u1* cp_end = scan.current();

  scan.skip(2, "truncated access_flags");
  scan.skip(2, "truncated this_class");
  scan.skip(2, "truncated super_class");
  u2 interfaces_count = scan.get_u2("truncated interfaces_count");
  scan.skip(interfaces_count * 2, "truncated interfaces");

  u2 fields_count = scan.get_u2("truncated fields_count");
  for (u2 i = 0; i < fields_count && scan.ok(); i++) {
    const char* error = nullptr;
    int ignored_methods = 0;
    int ignored_instructions = 0;
    if (!soroush_skip_member(&scan, cp, old_cp_count, &ignored_methods, &ignored_instructions, &error)) {
      free(cp);
      result.error = error == nullptr ? "bad field" : error;
      return result;
    }
  }

  u2 methods_count = scan.get_u2("truncated methods_count");
  if (!scan.ok()) {
    free(cp);
    result.error = scan.error();
    return result;
  }

  SoroushMethodTracePlan* plans =
      (SoroushMethodTracePlan*)calloc(methods_count, sizeof(SoroushMethodTracePlan));
  if (plans == nullptr) {
    free(cp);
    result.error = "out of memory";
    return result;
  }

  int rewrite_count = 0;
  int code_method_count = 0;        // methods with a Code attribute
  int constructor_method_count = 0; // <init>/<clinit> with Code (skipped by design)
  char dotted_class[512];
  size_t class_i = 0;
  for (; class_name[class_i] != '\0' && class_i < sizeof(dotted_class) - 1; class_i++) {
    dotted_class[class_i] = class_name[class_i] == '/' ? '.' : class_name[class_i];
  }
  dotted_class[class_i] = '\0';

  for (u2 mi = 0; mi < methods_count && scan.ok(); mi++) {
    scan.skip(2, "truncated method access_flags");
    u2 name_index = scan.get_u2("truncated method name_index");
    scan.skip(2, "truncated method descriptor_index");
    u2 attr_count = scan.get_u2("truncated method attributes_count");
    bool has_code = false;
    for (u2 ai = 0; ai < attr_count && scan.ok(); ai++) {
      u2 attr_name = scan.get_u2("truncated attribute name_index");
      u4 attr_len = scan.get_u4("truncated attribute length");
      if (soroush_utf8_equals(cp, old_cp_count, attr_name, "Code")) {
        has_code = true;
      }
      scan.skip((int)attr_len, "truncated attribute body");
    }

    bool is_init = soroush_utf8_equals(cp, old_cp_count, name_index, "<init>");
    bool is_clinit = soroush_utf8_equals(cp, old_cp_count, name_index, "<clinit>");
    if (has_code) {
      code_method_count++;
      if (is_init || is_clinit) {
        constructor_method_count++;
      }
    }

    // Rewrite ordinary methods and <init> constructors. <clinit> stays skipped.
    // Constructors are attempted but safe-skip (copy unchanged) during transform
    // if their delegation cannot be located safely.
    if (has_code && !is_clinit) {
      const char* method_name = cp[name_index].utf8;
      u2 method_name_len = cp[name_index].utf8_len;
      if (method_name != nullptr) {
        plans[mi].rewrite = true;
        plans[mi].is_constructor = is_init;
        snprintf(plans[mi].enter_msg, sizeof(plans[mi].enter_msg),
                 "ENTER %s.%.*s", dotted_class, method_name_len, method_name);
        snprintf(plans[mi].exit_msg, sizeof(plans[mi].exit_msg),
                 "EXIT %s.%.*s", dotted_class, method_name_len, method_name);
        rewrite_count++;
      }
    }
  }
  result.code_methods = code_method_count;
  result.constructor_methods = constructor_method_count;

  if (!scan.ok()) {
    free(plans);
    free(cp);
    result.error = scan.error();
    return result;
  }
  if (rewrite_count == 0) {
    free(plans);
    free(cp);
    result.error = "no rewriteable methods";
    return result;
  }
  int cp_entries_per_method = insert_exits ? 4 : 2;
  int global_cp_entries = insert_exits ? 11 : 6;
  if (old_cp_count + global_cp_entries + rewrite_count * cp_entries_per_method > 65535) {
    free(plans);
    free(cp);
    result.error = "constant pool too large for trace insertion";
    return result;
  }

  u2 idx_utf8_system = old_cp_count;
  u2 idx_class_system = old_cp_count + 1;
  u2 idx_utf8_soroush_trace = old_cp_count + 2;
  u2 idx_utf8_soroush_trace_desc = old_cp_count + 3;
  u2 idx_nt_soroush_trace = old_cp_count + 4;
  u2 idx_method_soroush_trace = old_cp_count + 5;
  u2 idx_utf8_throwable = insert_exits ? (u2)(old_cp_count + 6) : 0;
  u2 idx_class_throwable = insert_exits ? (u2)(old_cp_count + 7) : 0;
  u2 idx_utf8_stack_map_table = insert_exits ? (u2)(old_cp_count + 8) : 0;
  u2 idx_utf8_object = insert_exits ? (u2)(old_cp_count + 9) : 0;
  u2 idx_class_object = insert_exits ? (u2)(old_cp_count + 10) : 0;
  u2 next_method_cp_index = old_cp_count + global_cp_entries;
  for (u2 mi = 0; mi < methods_count; mi++) {
    if (plans[mi].rewrite) {
      plans[mi].enter_string_index = next_method_cp_index + 1;
      if (insert_exits) {
        plans[mi].exit_string_index = next_method_cp_index + 3;
      }
      next_method_cp_index += cp_entries_per_method;
    }
  }

  SoroushClassReader r(bytes, length);
  SoroushByteWriter out;
  out.append_u4(r.get_u4("truncated magic"));
  out.append_u2(r.get_u2("truncated minor_version"));
  out.append_u2(r.get_u2("truncated major_version"));
  r.skip(2, "truncated constant_pool_count");
  out.append_u2((u2)(old_cp_count + global_cp_entries + rewrite_count * cp_entries_per_method));
  out.append_bytes(cp_start, (int)(cp_end - cp_start));

  soroush_append_cp_utf8(&out, "java/lang/System");
  soroush_append_cp_class(&out, idx_utf8_system);
  soroush_append_cp_utf8(&out, "soroushTrace");
  soroush_append_cp_utf8(&out, "(Ljava/lang/String;)V");
  soroush_append_cp_name_and_type(&out, idx_utf8_soroush_trace, idx_utf8_soroush_trace_desc);
  soroush_append_cp_methodref(&out, idx_class_system, idx_nt_soroush_trace);
  if (insert_exits) {
    soroush_append_cp_utf8(&out, "java/lang/Throwable");
    soroush_append_cp_class(&out, idx_utf8_throwable);
    soroush_append_cp_utf8(&out, "StackMapTable");
    soroush_append_cp_utf8(&out, "java/lang/Object");
    soroush_append_cp_class(&out, idx_utf8_object);
  }
  for (u2 mi = 0; mi < methods_count; mi++) {
    if (plans[mi].rewrite) {
      soroush_append_cp_utf8(&out, plans[mi].enter_msg);
      soroush_append_cp_string(&out, plans[mi].enter_string_index - 1);
      if (insert_exits) {
        soroush_append_cp_utf8(&out, plans[mi].exit_msg);
        soroush_append_cp_string(&out, plans[mi].exit_string_index - 1);
      }
    }
  }
  r.skip((int)(cp_end - cp_start), "truncated constant pool");

  const u1* fixed_start = r.current();
  r.skip(2, "truncated access_flags");
  u2 this_class_index = r.get_u2("truncated this_class");
  r.skip(2, "truncated super_class");
  interfaces_count = r.get_u2("truncated interfaces_count");
  r.skip(interfaces_count * 2, "truncated interfaces");
  if (!r.ok()) {
    free(plans);
    free(cp);
    result.error = r.error();
    return result;
  }
  out.append_bytes(fixed_start, (int)(r.current() - fixed_start));

  fields_count = r.get_u2("truncated fields_count");
  out.append_u2(fields_count);
  for (u2 i = 0; i < fields_count; i++) {
    const u1* field_start = r.current();
    const char* error = nullptr;
    int ignored_methods = 0;
    int ignored_instructions = 0;
    if (!soroush_skip_member(&r, cp, old_cp_count, &ignored_methods, &ignored_instructions, &error)) {
      free(plans);
      free(cp);
      result.error = error == nullptr ? "bad field" : error;
      return result;
    }
    out.append_bytes(field_start, (int)(r.current() - field_start));
  }

  methods_count = r.get_u2("truncated methods_count");
  out.append_u2(methods_count);
  for (u2 mi = 0; mi < methods_count; mi++) {
    const char* error = nullptr;
    if (!soroush_transform_member_entry_trace(&out, &r, cp, old_cp_count,
                                              &plans[mi], idx_method_soroush_trace,
                                              idx_class_throwable,
                                              idx_utf8_stack_map_table,
                                              this_class_index,
                                              idx_class_object,
                                              insert_exits,
                                              &result.transformed_methods,
                                              &result.decoded_instructions,
                                              &error)) {
      free(plans);
      free(cp);
      result.error = error == nullptr ? "bad method" : error;
      return result;
    }
  }

  const u1* class_attrs_start = r.current();
  u2 class_attr_count = r.get_u2("truncated class attributes_count");
  for (u2 i = 0; i < class_attr_count && r.ok(); i++) {
    r.skip(2, "truncated class attribute name_index");
    u4 attr_len = r.get_u4("truncated class attribute length");
    r.skip((int)attr_len, "truncated class attribute body");
  }
  if (!r.ok() || r.current() != bytes + length) {
    free(plans);
    free(cp);
    result.error = r.ok() ? "classfile parser did not consume full input" : r.error();
    return result;
  }
  out.append_bytes(class_attrs_start, (int)(r.current() - class_attrs_start));

  free(plans);
  free(cp);
  if (!out.ok()) {
    result.error = "out of memory while transforming classfile";
    return result;
  }
  result.length = out.length();
  result.bytes = out.release();
  result.ok = true;
  result.error = nullptr;
  return result;
}

SoroushClassfileRewriter::TransformResult
SoroushClassfileRewriter::insert_entry_trace(const u1* bytes, int length, const char* class_name) {
  return soroush_insert_trace(bytes, length, class_name, false);
}

SoroushClassfileRewriter::TransformResult
SoroushClassfileRewriter::insert_entry_exit_trace(const u1* bytes, int length, const char* class_name) {
  return soroush_insert_trace(bytes, length, class_name, true);
}

void SoroushClassfileRewriter::free_transform(TransformResult* result) {
  if (result == nullptr) return;
  free(result->bytes);
  result->bytes = nullptr;
  result->length = 0;
}
