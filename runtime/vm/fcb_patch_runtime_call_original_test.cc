// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"
#include "vm/fcb_patch_runtime_internal.h"

#include <string>
#include <utility>

#include "vm/object.h"
#include "vm/os.h"
#include "vm/thread.h"
#include "vm/unit_test.h"

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
namespace {

dart::fcb::BytecodeModule CallOriginalClassStaticModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "42".
      0x52, 0x00, 0x00, 0x01,  // CallOriginal int.parse, argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::parseInt()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:int.parse"));
  function.constants.push_back(dart::fcb::Value::String("42"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "ff".
      0x01, 0x00, 0x02,        // LoadConst 16.
      0x52, 0x00, 0x00, 0x02,  // CallOriginal int.parse(radix: 16).
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::parseHex()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(
      "dart:core::class:int.parse;named:radix"));
  function.constants.push_back(dart::fcb::Value::String("ff"));
  function.constants.push_back(dart::fcb::Value::Int(16));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalGenericModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "patched".
      0x52, 0x00, 0x00, 0x01,  // CallOriginal checkNotNull<String>, argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::checkString()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(
      "dart:core::class:ArgumentError.checkNotNull;types:String"));
  function.constants.push_back(dart::fcb::Value::String("patched"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalWrongTypeArgsModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "patched".
      0x52, 0x00, 0x00, 0x01,  // CallOriginal with too many type args.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::badTypeArgs()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(
      "dart:core::class:ArgumentError.checkNotNull;types:String,int"));
  function.constants.push_back(dart::fcb::Value::String("patched"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalTooManyNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "ff".
      0x52, 0x00, 0x00, 0x01,  // CallOriginal int.parse(radix, extra), argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::tooManyNamed()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(
      "dart:core::class:int.parse;named:radix,extra"));
  function.constants.push_back(dart::fcb::Value::String("ff"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalMalformedTypeArgsModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "patched".
      0x52, 0x00, 0x00, 0x01,  // CallOriginal checkNotNull;types:String,.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::badTypeMetadata()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(
      "dart:core::class:ArgumentError.checkNotNull;types:String,"));
  function.constants.push_back(dart::fcb::Value::String("patched"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalTryCatchesVmExceptionModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x0f, 0x00, 0x13,  // TryBegin handler 15, end 19.
      0x01, 0x00, 0x01,              // LoadConst null.
      0x52, 0x00, 0x00, 0x01,        // CallOriginal checkNotNull(null).
      0x30, 0x00, 0x13,              // Jump 19.
      0x04, 0x00,                    // StoreLocal 0 (exception text).
      0x03, 0x00,                    // LoadLocal 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::catchVmException()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(
      "dart:core::class:ArgumentError.checkNotNull;types:String"));
  function.constants.push_back(dart::fcb::Value::Null());
  module.functions.push_back(std::move(function));
  return module;
}

}  // namespace

namespace dart {

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallOriginalClassStatic) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalClassStaticModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/original.dart::parseInt()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeCallOriginalClassStatic failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kInt);
  EXPECT_EQ(42, result.value.int_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallOriginalNamed) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/original.dart::parseHex()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeCallOriginalNamed failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kInt);
  EXPECT_EQ(255, result.value.int_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallOriginalGeneric) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalGenericModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/original.dart::checkString()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeCallOriginalGeneric failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched", result.value.string_value.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallOriginalGenericRejectsWrongArgs) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalWrongTypeArgsModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/original.dart::badTypeArgs()", {});
  EXPECT(!result.ok);
  EXPECT_SUBSTRING("type arguments", result.error.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallOriginalRejectsTooManyNamed) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalTooManyNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/original.dart::tooManyNamed()", {});
  EXPECT(!result.ok);
  EXPECT_SUBSTRING("too many named function arguments", result.error.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallOriginalRejectsMalformedTypeArgs) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalMalformedTypeArgsModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/original.dart::badTypeMetadata()", {});
  EXPECT(!result.ok);
  EXPECT_SUBSTRING("empty type argument", result.error.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeTryCatchesCallOriginalException) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalTryCatchesVmExceptionModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/original.dart::catchVmException()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeTryCatchesCallOriginalException failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  const Object& exception =
      Object::Handle(thread->zone(), result.value.ToDart());
  EXPECT(exception.IsError());
  EXPECT_SUBSTRING("Invalid argument",
                   Error::Cast(exception).ToErrorCString());
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);
}

}  // namespace dart
#endif
