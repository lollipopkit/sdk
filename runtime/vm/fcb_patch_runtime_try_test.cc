// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <cassert>
#include <string>
#include <utility>

#include "vm/unit_test.h"

namespace {

dart::fcb::BytecodeModule TryCatchThrowModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x0c, 0x00, 0x11,  // TryBegin handler 12, end 17.
      0x01, 0x00, 0x00,              // LoadConst "boom".
      0x60,                          // Throw.
      0x30, 0x00, 0x11,              // Jump 17.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x01, 0x00, 0x01,              // LoadConst "caught".
      0xff,                          // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/try.dart::recover()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("boom"));
  function.constants.push_back(dart::fcb::Value::String("caught"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule TryCatchFallthroughModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x0b, 0x00, 0x10,  // TryBegin handler 11, end 16.
      0x01, 0x00, 0x00,              // LoadConst "ok".
      0x30, 0x00, 0x10,              // Jump 16.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x01, 0x00, 0x01,              // LoadConst "caught".
      0xff,                          // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/try.dart::ok()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("ok"));
  function.constants.push_back(dart::fcb::Value::String("caught"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule TryCatchExceptionValueModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x0c, 0x00, 0x10,  // TryBegin handler 12, end 16.
      0x01, 0x00, 0x00,              // LoadConst "boom".
      0x60,                          // Throw.
      0x30, 0x00, 0x10,              // Jump 16.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x03, 0x00,                    // LoadLocal 0.
      0xff,                          // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/try.dart::exceptionValue()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("boom"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule TryCatchesCallStaticThrowModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;

  const std::vector<uint8_t> helper_code = {
      0x01, 0x00, 0x00,  // LoadConst "static-boom".
      0x60,              // Throw.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction helper;
  helper.function_id = "package:app/try.dart::throwingHelper()";
  helper.bytecode_offset = 0;
  helper.bytecode_length = static_cast<uint32_t>(helper_code.size());
  helper.constants.push_back(dart::fcb::Value::String("static-boom"));
  module.bytecode.insert(module.bytecode.end(), helper_code.begin(),
                         helper_code.end());

  const std::vector<uint8_t> caller_code = {
      0x61, 0x00, 0x0c, 0x00, 0x11,  // TryBegin handler 12, end 17.
      0x50, 0x00, 0x00, 0x00,        // CallStatic throwingHelper().
      0x30, 0x00, 0x11,              // Jump 17.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x01, 0x00, 0x01,              // LoadConst "caught-static".
      0xff,                          // Return.
  };
  dart::fcb::BytecodeFunction caller;
  caller.function_id = "package:app/try.dart::callStaticRecover()";
  caller.register_count = 1;
  caller.bytecode_offset = static_cast<uint32_t>(module.bytecode.size());
  caller.bytecode_length = static_cast<uint32_t>(caller_code.size());
  caller.constants.push_back(
      dart::fcb::Value::String("package:app/try.dart::throwingHelper()"));
  caller.constants.push_back(dart::fcb::Value::String("caught-static"));
  module.bytecode.insert(module.bytecode.end(), caller_code.begin(),
                         caller_code.end());

  module.functions.push_back(std::move(helper));
  module.functions.push_back(std::move(caller));
  return module;
}

dart::fcb::BytecodeModule UncaughtThrowModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst "boom".
      0x60,              // Throw.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/try.dart::uncaught()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("boom"));
  module.functions.push_back(std::move(function));
  return module;
}

void InterpretsCaughtThrow() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(TryCatchThrowModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/try.dart::recover()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kString);
  assert(result.value.string_value == "caught");
}

void InterpretsTryFallthrough() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  const dart::fcb::BytecodeModule module = TryCatchFallthroughModule();
  const bool loaded = runtime.LoadModule(module, &error);
  assert(loaded);
  (void)loaded;

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/try.dart::ok()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kString);
  assert(result.value.string_value == "ok");
}

void PreservesCaughtExceptionValue() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  const dart::fcb::BytecodeModule module = TryCatchExceptionValueModule();
  const bool loaded = runtime.LoadModule(module, &error);
  assert(loaded);
  (void)loaded;

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/try.dart::exceptionValue()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kString);
  assert(result.value.string_value == "boom");
}

void CatchesCallStaticThrow() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  const dart::fcb::BytecodeModule module = TryCatchesCallStaticThrowModule();
  const bool loaded = runtime.LoadModule(module, &error);
  assert(loaded);
  (void)loaded;

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/try.dart::callStaticRecover()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kString);
  assert(result.value.string_value == "caught-static");
}

void RejectsUncaughtThrow() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  const dart::fcb::BytecodeModule module = UncaughtThrowModule();
  const bool loaded = runtime.LoadModule(module, &error);
  assert(loaded);
  (void)loaded;

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/try.dart::uncaught()", {});
  assert(!result.ok);
  assert(result.error.find("Unhandled throw: boom") != std::string::npos);
}

void RejectsInvalidTryBeginTarget() {
  dart::fcb::BytecodeModule module = TryCatchThrowModule();
  module.bytecode[2] = 0x06;  // Handler target points into LoadConst operand.
  std::string error;
  assert(!dart::fcb::ValidateModule(module, &error));
  assert(error.find("non-instruction offset") != std::string::npos);
}

}  // namespace

void RunFcbPatchRuntimeTryStandaloneTests() {
  InterpretsCaughtThrow();
  InterpretsTryFallthrough();
  PreservesCaughtExceptionValue();
  CatchesCallStaticThrow();
  RejectsUncaughtThrow();
  RejectsInvalidTryBeginTarget();
}

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
namespace dart {

UNIT_TEST_CASE(FcbPatchRuntimeTryCatchThrow) {
  RunFcbPatchRuntimeTryStandaloneTests();
}

}  // namespace dart
#endif
