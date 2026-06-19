// Copyright (c) 2026, the FCB project authors.

#ifndef RUNTIME_VM_FCB_PATCH_RUNTIME_INTERNAL_H_
#define RUNTIME_VM_FCB_PATCH_RUNTIME_INTERNAL_H_

#include <cstddef>
#include <string>
#include <vector>

#include "vm/fcb_patch_runtime.h"

namespace dart {

class Function;

namespace fcb {
namespace internal {

ObjectPtr MaterializeDartObject(Value* value);
bool ContainsBytecodeClosure(const Value& value);
bool TryMaterializeDartObject(Value* value, ObjectPtr* out, std::string* error);
bool TryMaterializeBytecodeClosure(Value* value,
                                   ObjectPtr* out,
                                   std::string* error);
bool IsFcbBytecodeClosureTrampoline(const Function& function);
std::string FcbBytecodeClosureTargetId(const Function& function);
void PopulateScalarFromDartObject(Value* value);
std::size_t CountNamedArguments(const std::string& metadata);

bool DartInstanceGetField(const Value& receiver,
                          const std::string& field_name,
                          Value* out,
                          std::string* error);

bool DartInstanceSetField(Value* receiver,
                          const std::string& field_name,
                          Value value,
                          std::string* error);

bool DartInstanceCallDynamic(Value receiver,
                             const std::string& method_name,
                             std::vector<Value> arguments,
                             Value* out,
                             std::string* error,
                             Value* exception);

bool DartCallOriginal(const std::string& function_id,
                      std::vector<Value> arguments,
                      Value* out,
                      std::string* error,
                      Value* exception);

bool DartMakeClosure(const std::string& function_id,
                     Value* out,
                     std::string* error);

bool DartNewObject(const std::string& constructor_id,
                   std::vector<Value> arguments,
                   Value* out,
                   std::string* error,
                   Value* exception);

bool DartCallClosure(Value closure,
                     const std::string& call_metadata,
                     std::vector<Value> arguments,
                     Value* out,
                     std::string* error,
                     Value* exception);

bool DartIsType(Value value,
                const std::string& type_name,
                bool* out,
                std::string* error);

struct PatchStackTraceFrameInfo {
  const char* function_id;
  const char* source_location;
  uint32_t bytecode_offset;
  const BytecodeFunction* function;
  const Value* arguments;
  std::size_t argument_count;
  std::size_t captured_argument_count;
  const Value* locals;
  std::size_t local_count;
  std::size_t active_handler_count;
  uint32_t innermost_handler_offset;
  uint32_t innermost_handler_end_offset;
};
using ActivePatchFrameUpdateCallback =
    void (*)(const PatchStackTraceFrameInfo& frame_info);

void RecordPatchStackTraceLocation(const BytecodeFunction& function,
                                   uint32_t bytecode_offset);
const char* LastPatchStackTraceLocation();
std::size_t PatchStackTraceLocationCount();
const char* PatchStackTraceLocationAt(std::size_t index);
PatchStackTraceFrameInfo PatchStackTraceFrameInfoAt(std::size_t index);
void ClearPatchStackTraceLocation();

void PushActivePatchFrame(const BytecodeFunction& function,
                          uint32_t bytecode_offset,
                          std::size_t captured_argument_count,
                          const std::vector<Value>* arguments,
                          const std::vector<Value>* locals,
                          std::size_t active_handler_count = 0,
                          uint32_t innermost_handler_offset = 0,
                          uint32_t innermost_handler_end_offset = 0);
void UpdateActivePatchFrame(const BytecodeFunction& function,
                            uint32_t bytecode_offset,
                            std::size_t captured_argument_count,
                            const std::vector<Value>* arguments,
                            const std::vector<Value>* locals,
                            std::size_t active_handler_count = 0,
                            uint32_t innermost_handler_offset = 0,
                            uint32_t innermost_handler_end_offset = 0);
void PopActivePatchFrame();
std::size_t ActivePatchFrameCount();
PatchStackTraceFrameInfo ActivePatchFrameInfoAt(std::size_t index);
void SetActivePatchFrameUpdateCallback(
    ActivePatchFrameUpdateCallback callback);

class ScopedActivePatchFrame {
 public:
  ScopedActivePatchFrame(const BytecodeFunction& function,
                         const std::vector<Value>& arguments,
                         const std::vector<Value>& locals,
                         std::size_t captured_argument_count);
  ~ScopedActivePatchFrame();

 private:
  const BytecodeFunction& function_;
  const std::vector<Value>& arguments_;
  const std::vector<Value>& locals_;
  std::size_t captured_argument_count_;
};

}  // namespace internal
}  // namespace fcb
}  // namespace dart

#endif  // RUNTIME_VM_FCB_PATCH_RUNTIME_INTERNAL_H_
