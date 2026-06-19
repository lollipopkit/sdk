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

dart::fcb::BytecodeModule NewObjectTooManyNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "old".
      0x55, 0x00, 0x00, 0x01,  // NewObject Uri(path, extra), argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/new_object.dart::tooManyNamed()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:Uri.;named:path,extra"));
  function.constants.push_back(dart::fcb::Value::String("old"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule NewObjectMalformedTypeArgsModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst 2.
      0x01, 0x00, 0x02,        // LoadConst "x".
      0x55, 0x00, 0x00, 0x02,  // NewObject List.filled;types:String,.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/new_object.dart::badTypes()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:List.filled;types:String,"));
  function.constants.push_back(dart::fcb::Value::Int(2));
  function.constants.push_back(dart::fcb::Value::String("x"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule NewObjectTryCatchesVmExceptionModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x12, 0x00, 0x16,  // TryBegin handler 18, end 22.
      0x01, 0x00, 0x01,              // LoadConst 2.
      0x01, 0x00, 0x02,              // LoadConst 1.
      0x55, 0x00, 0x00, 0x02,        // NewObject List.filled<String>(2, 1).
      0x30, 0x00, 0x16,              // Jump 22.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x03, 0x00,                    // LoadLocal 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/new_object.dart::catchNewObjectException()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:List.filled;types:String"));
  function.constants.push_back(dart::fcb::Value::Int(2));
  function.constants.push_back(dart::fcb::Value::Int(1));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule NewObjectFutureValueModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "patched-async".
      0x55, 0x00, 0x00, 0x01,  // NewObject _Future.value<String>(value).
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/new_object.dart::asyncLabel()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(
      "dart:async::class:_Future.value;types:String"));
  function.constants.push_back(dart::fcb::Value::String("patched-async"));
  module.functions.push_back(std::move(function));
  return module;
}

}  // namespace

namespace dart {

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeNewObjectRejectsTooManyNamed) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectTooManyNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/new_object.dart::tooManyNamed()", {});
  EXPECT(!result.ok);
  EXPECT(result.error.find("too many named constructor arguments") !=
         std::string::npos);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeNewObjectRejectsMalformedTypeArgs) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectMalformedTypeArgsModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/new_object.dart::badTypes()", {});
  EXPECT(!result.ok);
  EXPECT(result.error.find("empty type argument") != std::string::npos);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeTryCatchesNewObjectException) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectTryCatchesVmExceptionModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/new_object.dart::catchNewObjectException()",
                        {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeTryCatchesNewObjectException failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  const Object& exception =
      Object::Handle(thread->zone(), result.value.ToDart());
  EXPECT(exception.IsError());
  EXPECT_SUBSTRING("type", Error::Cast(exception).ToErrorCString());
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeNewObjectFutureValue) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectFutureValueModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/new_object.dart::asyncLabel()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeNewObjectFutureValue failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  const Object& future = Object::Handle(thread->zone(), result.value.ToDart());
  EXPECT(future.IsInstance());
  const Class& cls = Class::Handle(thread->zone(), future.clazz());
  EXPECT_SUBSTRING("Future", cls.UserVisibleNameCString());
}

}  // namespace dart
#endif
