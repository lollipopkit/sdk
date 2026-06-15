// Copyright (c) 2026, the FCB project authors.
//
// Phase D patch runtime skeleton for the FCB Dart VM fork. This file is kept
// header-only with no VM object dependencies for the first landing step; the
// next integration step wires DispatchDecision into real function entry
// dispatch and replaces the plain byte vectors with VM ObjectPtr values.

#ifndef RUNTIME_VM_FCB_PATCH_RUNTIME_H_
#define RUNTIME_VM_FCB_PATCH_RUNTIME_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dart {
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
};

enum class ReturnConvention : uint8_t {
  kTagged = 0,
  kUnboxedInt64 = 1,
};

struct Value {
  ValueKind kind = ValueKind::kNull;
  int64_t int_value = 0;
  double double_value = 0.0;
  bool bool_value = false;
  std::string string_value;
  std::vector<Value> list_value;
  // Flat key/value storage: even indexes are keys, odd indexes are values.
  std::vector<Value> map_entries;

  static Value Null();
  static Value Int(int64_t value);
  static Value Double(double value);
  static Value Bool(bool value);
  static Value String(std::string value);
  static Value List(std::vector<Value> value);
  static Value Map(std::vector<Value> entries);
};

struct InterpretResult {
  bool ok = false;
  Value value;
  std::string error;

  static InterpretResult Ok(Value value);
  static InterpretResult Error(std::string error);
};

struct BytecodeFunction {
  std::string function_id;
  ReturnConvention return_convention = ReturnConvention::kTagged;
  uint8_t parameter_count = 0;
  uint8_t register_count = 0;
  uint32_t bytecode_offset = 0;
  uint32_t bytecode_length = 0;
  std::vector<Value> constants;
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
  InterpretResult Interpret(const std::string& function_id,
                            const std::vector<Value>& arguments) const;
  bool DisablePatch(const std::string& function_id);
  void Clear();
  std::size_t patch_count() const { return table_.size(); }

 private:
  PatchTable table_;
};

bool ValidateModule(const BytecodeModule& module, std::string* error);
bool LoadBytecodeModuleFromFile(const std::string& path,
                                BytecodeModule* module,
                                std::string* error);

}  // namespace fcb
}  // namespace dart

#endif  // RUNTIME_VM_FCB_PATCH_RUNTIME_H_
