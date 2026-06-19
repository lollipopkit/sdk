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

dart::fcb::BytecodeModule CallDynamicNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "old".
      0x55, 0x00, 0x02, 0x01,  // NewObject Uri(path: "old").
      0x01, 0x00, 0x03,        // LoadConst "new".
      0x51, 0x00, 0x00, 0x01,  // CallDynamic replace(path: "new").
      0x51, 0x00, 0x04, 0x00,  // CallDynamic toString().
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/dynamic.dart::replacePath()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("replace;named:path"));
  function.constants.push_back(dart::fcb::Value::String("old"));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:Uri.;named:path"));
  function.constants.push_back(dart::fcb::Value::String("new"));
  function.constants.push_back(dart::fcb::Value::String("toString"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallDynamicTooManyNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "old".
      0x55, 0x00, 0x02, 0x01,  // NewObject Uri(path: "old").
      0x01, 0x00, 0x03,        // LoadConst "new".
      0x51, 0x00, 0x00, 0x01,  // CallDynamic replace(path, extra), argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/dynamic.dart::tooManyNamed()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("replace;named:path,extra"));
  function.constants.push_back(dart::fcb::Value::String("old"));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:Uri.;named:path"));
  function.constants.push_back(dart::fcb::Value::String("new"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallDynamicUnknownNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "old".
      0x55, 0x00, 0x02, 0x01,  // NewObject Uri(path: "old").
      0x01, 0x00, 0x03,        // LoadConst "new".
      0x51, 0x00, 0x00, 0x01,  // CallDynamic replace(unknown: "new").
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/dynamic.dart::unknownNamed()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("replace;named:unknown"));
  function.constants.push_back(dart::fcb::Value::String("old"));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:Uri.;named:path"));
  function.constants.push_back(dart::fcb::Value::String("new"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallDynamicTryCatchesVmExceptionModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x12, 0x00, 0x16,  // TryBegin handler 18, end 22.
      0x01, 0x00, 0x01,              // LoadConst "abc".
      0x01, 0x00, 0x02,              // LoadConst 10.
      0x51, 0x00, 0x00, 0x01,        // CallDynamic substring(10).
      0x30, 0x00, 0x16,              // Jump 22.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x03, 0x00,                    // LoadLocal 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/dynamic.dart::catchDynamicException()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("substring"));
  function.constants.push_back(dart::fcb::Value::String("abc"));
  function.constants.push_back(dart::fcb::Value::Int(10));
  module.functions.push_back(std::move(function));
  return module;
}

}  // namespace

namespace dart {

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallDynamicNamed) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallDynamicNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/dynamic.dart::replacePath()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeCallDynamicNamed failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("new", result.value.string_value.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallDynamicRejectsTooManyNamed) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallDynamicTooManyNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/dynamic.dart::tooManyNamed()", {});
  EXPECT(!result.ok);
  EXPECT(result.error.find("too many named dynamic arguments") !=
         std::string::npos);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallDynamicRejectsUnknownNamed) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallDynamicUnknownNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/dynamic.dart::unknownNamed()", {});
  EXPECT(!result.ok);
  EXPECT(result.error.find("method not found") != std::string::npos);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeTryCatchesCallDynamicException) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallDynamicTryCatchesVmExceptionModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/dynamic.dart::catchDynamicException()",
                        {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeTryCatchesCallDynamicException failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  const Object& exception =
      Object::Handle(thread->zone(), result.value.ToDart());
  EXPECT(exception.IsError());
  EXPECT_SUBSTRING("RangeError", Error::Cast(exception).ToErrorCString());
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);
}

}  // namespace dart
#endif
