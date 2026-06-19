// Copyright (c) 2026, the FCB project authors.
//
// FCB patch runtime for the Dart VM fork. The runtime owns the bytecode and
// source-map loader, the interpreter core, and VM ObjectPtr roots for values
// materialized while a Dart mutator thread is current. Standalone tests keep the
// scalar fallback path so the bytecode semantics can be verified without a VM.

#ifndef RUNTIME_VM_FCB_PATCH_RUNTIME_H_
#define RUNTIME_VM_FCB_PATCH_RUNTIME_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "vm/tagged_pointer.h"

namespace dart {

class ObjectPointerVisitor;

namespace fcb {

enum class PatchState {
  kOriginalOnly,
  kPatchedInterpreted,
  kDisabledBadPatch,
};

enum class ValueKind {
  kNull,
  kInt,
  kDouble,
  kBool,
  kString,
  kList,
  kMap,
  kBytecodeClosure,
};

enum class ReturnConvention : uint8_t {
  kTagged = 0,
  kUnboxedInt64 = 1,
};

struct Value {
  ValueKind kind = ValueKind::kNull;
  ObjectPtr object_value = nullptr;
  int64_t int_value = 0;
  double double_value = 0.0;
  bool bool_value = false;
  std::string string_value;
  std::vector<Value> list_value;
  // Flat key/value storage: even indexes are keys, odd indexes are values.
  std::vector<Value> map_entries;
  std::string closure_function_id;
  std::vector<Value> closure_captures;
  intptr_t closure_optional_positional_count = 0;
  intptr_t closure_type_parameter_count = 0;
  std::vector<std::string> closure_named_parameters;

  static Value Null();
  static Value Int(int64_t value);
  static Value Double(double value);
  static Value Bool(bool value);
  static Value String(std::string value);
  static Value List(std::vector<Value> value);
  static Value Map(std::vector<Value> entries);
  static Value BytecodeClosure(
      std::string function_id,
      std::vector<Value> captures,
      intptr_t optional_positional_count = 0,
      intptr_t type_parameter_count = 0,
      std::vector<std::string> named_parameters = {});
  static Value FromDart(ObjectPtr value);

  ObjectPtr ToDart();
  void VisitObjectPointers(ObjectPointerVisitor* visitor);
};

struct InterpretResult {
  bool ok = false;
  Value value;
  std::string error;

  static InterpretResult Ok(Value value);
  static InterpretResult Error(std::string error);
};

struct SourceMapEntry {
  uint32_t bytecode_offset = 0;
  std::string source_location;
};

struct DebugLocalEntry {
  uint16_t slot = 0;
  std::string name;
};

struct BytecodeFunction {
  std::string function_id;
  ReturnConvention return_convention = ReturnConvention::kTagged;
  uint8_t parameter_count = 0;
  uint8_t register_count = 0;
  uint32_t bytecode_offset = 0;
  uint32_t bytecode_length = 0;
  std::vector<Value> constants;
  std::vector<SourceMapEntry> source_map;
  std::vector<DebugLocalEntry> debug_locals;

  void VisitObjectPointers(ObjectPointerVisitor* visitor);
};

struct BytecodeModule {
  uint32_t version = 1;
  std::vector<uint8_t> bytecode;
  std::vector<BytecodeFunction> functions;
};

struct PatchEntry {
  PatchState state = PatchState::kOriginalOnly;
  BytecodeFunction function;
};

struct DispatchDecision {
  PatchState state = PatchState::kOriginalOnly;
  const BytecodeFunction* function = nullptr;
};

class PatchTable {
 public:
  bool Install(const BytecodeModule& module, std::string* error);
  DispatchDecision Resolve(const std::string& function_id) const;
  DispatchDecision ResolveUniqueByArity(std::size_t parameter_count,
                                        std::string* function_id) const;
  bool Disable(const std::string& function_id);
  void Clear();
  std::size_t size() const { return entries_.size(); }
  void VisitObjectPointers(ObjectPointerVisitor* visitor);

 private:
  friend class PatchRuntime;
  std::vector<uint8_t> bytecode_;
  std::unordered_map<std::string, PatchEntry> entries_;
};

class PatchRuntime {
 public:
  bool LoadModule(const BytecodeModule& module, std::string* error);
  bool LoadModuleFromFile(const std::string& path, std::string* error);
  DispatchDecision Resolve(const std::string& function_id) const;
  DispatchDecision ResolveUniqueByArity(std::size_t parameter_count,
                                        std::string* function_id) const;
  bool FunctionUsesArgument(const BytecodeFunction& function,
                            uint8_t argument_index) const;
  InterpretResult Interpret(
      const std::string& function_id,
      const std::vector<Value>& arguments,
      std::size_t captured_argument_count = 0) const;
  bool DisablePatch(const std::string& function_id);
  void Clear();
  std::size_t patch_count() const { return table_.size(); }
  void VisitObjectPointers(ObjectPointerVisitor* visitor);

 private:
  InterpretResult InterpretFunction(const std::string& function_id,
                                    const std::vector<Value>& arguments,
                                    uint32_t depth,
                                    std::size_t captured_argument_count) const;

  PatchTable table_;
};

bool ValidateModule(const BytecodeModule& module, std::string* error);
bool LoadBytecodeModuleFromFile(const std::string& path,
                                BytecodeModule* module,
                                std::string* error);

}  // namespace fcb
}  // namespace dart

#endif  // RUNTIME_VM_FCB_PATCH_RUNTIME_H_
