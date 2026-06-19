// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"
#include "vm/fcb_patch_runtime_internal.h"

#include <string>
#include <utility>

#include "vm/object.h"
#include "vm/os.h"
#include "vm/unit_test.h"

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
namespace {

dart::fcb::BytecodeModule GenericListTypeModule(const char* function_id,
                                                const char* list_type,
                                                const char* check_type,
                                                bool as_type) {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,        // LoadConst 0: length.
      0x01, 0x00, 0x01,        // LoadConst 1: fill.
      0x55, 0x00, 0x02, 0x02,  // NewObject List<T>.filled, argc 2.
      static_cast<uint8_t>(as_type ? 0x46 : 0x45),
      0x00,
      0x03,  // IsType/AsType check type.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = function_id;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(1));
  function.constants.push_back(
      list_type == std::string("String") ? dart::fcb::Value::String("patched")
                                         : dart::fcb::Value::Int(7));
  function.constants.push_back(dart::fcb::Value::String(
      std::string("dart:core::class:List.filled;types:") + list_type));
  function.constants.push_back(dart::fcb::Value::String(check_type));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule AsTypeTryCatchesMismatchModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x15, 0x00, 0x19,  // TryBegin handler 21, end 25.
      0x01, 0x00, 0x00,              // LoadConst 1: length.
      0x01, 0x00, 0x01,              // LoadConst 7: fill.
      0x55, 0x00, 0x02, 0x02,        // NewObject List<int>.filled.
      0x46, 0x00, 0x03,              // AsType List<String>.
      0x30, 0x00, 0x19,              // Jump 25.
      0x04, 0x00,                    // StoreLocal 0 (exception text).
      0x03, 0x00,                    // LoadLocal 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/type.dart::catchAsTypeMismatch()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(1));
  function.constants.push_back(dart::fcb::Value::Int(7));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:List.filled;types:int"));
  function.constants.push_back(dart::fcb::Value::String("List<String>"));
  module.functions.push_back(std::move(function));
  return module;
}

}  // namespace

namespace dart {

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeGenericIsType) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(GenericListTypeModule(
             "package:app/type.dart::isStringList()", "String",
             "List<String>", false),
         &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/type.dart::isStringList()", {});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(result.value.bool_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeGenericIsTypeRejectsWrongArgs) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(GenericListTypeModule(
             "package:app/type.dart::isStringList()", "int",
             "List<String>", false),
         &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/type.dart::isStringList()", {});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(!result.value.bool_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeGenericAsType) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(GenericListTypeModule(
             "package:app/type.dart::asStringList()", "String",
             "List<String>", true),
         &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/type.dart::asStringList()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeGenericAsType failed: %s\n",
                 result.error.c_str());
    EXPECT(result.ok);
    return;
  }
  EXPECT(result.value.object_value != nullptr);
  EXPECT(Object::Handle(thread->zone(), result.value.ToDart()).IsArray());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeGenericAsTypeRejectsWrongArgs) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(GenericListTypeModule(
             "package:app/type.dart::asStringList()", "int",
             "List<String>", true),
         &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/type.dart::asStringList()", {});
  EXPECT(!result.ok);
  EXPECT_SUBSTRING("AsType failed", result.error.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeTryCatchesAsTypeMismatch) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(AsTypeTryCatchesMismatchModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/type.dart::catchAsTypeMismatch()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeTryCatchesAsTypeMismatch failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_SUBSTRING("AsType failed", result.value.string_value.c_str());
  EXPECT_SUBSTRING("List<String>", result.value.string_value.c_str());
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);
}

}  // namespace dart
#endif
