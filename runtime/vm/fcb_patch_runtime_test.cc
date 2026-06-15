// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace {

dart::fcb::BytecodeModule TestModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {0x01, 0x00, 0x00, 0xff};
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/pricing.dart::price(int)";
  function.parameter_count = 1;
  function.register_count = 2;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(42));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule AddModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {0x02, 0x00, 0x02, 0x01, 0x10, 0xff};
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/math.dart::add(int,int)";
  function.parameter_count = 2;
  function.register_count = 2;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule ThreeArgModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,  // LoadArg 0.
      0x02, 0x01,  // LoadArg 1.
      0x10,        // Add.
      0x02, 0x02,  // LoadArg 2.
      0x10,        // Add.
      0xff,        // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/math.dart::sum3(int,int,int)";
  function.parameter_count = 3;
  function.register_count = 3;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule ConditionalStringModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0.
      0x31, 0x00, 0x09,  // JumpIfFalse 9.
      0x01, 0x00, 0x00,  // LoadConst "patched".
      0xff,              // Return.
      0x01, 0x00, 0x01,  // LoadConst null.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/message.dart::message(bool)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("patched"));
  function.constants.push_back(dart::fcb::Value::Null());
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule ListModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst 7.
      0x01, 0x00, 0x01,  // LoadConst "days".
      0x40, 0x00, 0x02,  // MakeList 2.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/list.dart::items()";
  function.parameter_count = 0;
  function.register_count = 0;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(7));
  function.constants.push_back(dart::fcb::Value::String("days"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule MapModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst "price".
      0x01, 0x00, 0x01,  // LoadConst 9.
      0x01, 0x00, 0x02,  // LoadConst "active".
      0x01, 0x00, 0x03,  // LoadConst true.
      0x41, 0x00, 0x02,  // MakeMap 2.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/map.dart::attrs()";
  function.parameter_count = 0;
  function.register_count = 0;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("price"));
  function.constants.push_back(dart::fcb::Value::Int(9));
  function.constants.push_back(dart::fcb::Value::String("active"));
  function.constants.push_back(dart::fcb::Value::Bool(true));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule DoubleMathModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst 2.5.
      0x01, 0x00, 0x01,  // LoadConst 4.
      0x10,              // Add.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/math.dart::doubleAdd()";
  function.parameter_count = 0;
  function.register_count = 0;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Double(2.5));
  function.constants.push_back(dart::fcb::Value::Int(4));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule DivisionModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst 7.
      0x01, 0x00, 0x01,  // LoadConst 2.
      0x13,              // Div.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/math.dart::divide()";
  function.parameter_count = 0;
  function.register_count = 0;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(7));
  function.constants.push_back(dart::fcb::Value::Int(2));
  module.functions.push_back(std::move(function));
  return module;
}

void LoadsAndResolvesPatchedFunction() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(TestModule(), &error));
  assert(runtime.patch_count() == 1);

  const dart::fcb::DispatchDecision decision =
      runtime.Resolve("package:app/pricing.dart::price(int)");
  assert(decision.state == dart::fcb::PatchState::kPatchedInterpreted);
  assert(decision.function != nullptr);
  assert(decision.function->parameter_count == 1);

  const dart::fcb::DispatchDecision miss =
      runtime.Resolve("package:app/pricing.dart::unchanged()");
  assert(miss.state == dart::fcb::PatchState::kOriginalOnly);
  assert(miss.function == nullptr);
}

void DisabledPatchFallsBackWithoutFunction() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(TestModule(), &error));
  assert(runtime.DisablePatch("package:app/pricing.dart::price(int)"));

  const dart::fcb::DispatchDecision decision =
      runtime.Resolve("package:app/pricing.dart::price(int)");
  assert(decision.state == dart::fcb::PatchState::kDisabledBadPatch);
  assert(decision.function == nullptr);
}

void InterpretsPatchedFunction() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(TestModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/pricing.dart::price(int)",
                        {dart::fcb::Value::Int(7)});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 42);
}

void InterpretsArgumentsAndBinaryOps() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(AddModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/math.dart::add(int,int)",
                        {dart::fcb::Value::Int(3), dart::fcb::Value::Int(4)});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 7);
}

void InterpretsThreeArgumentFunction() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(ThreeArgModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/math.dart::sum3(int,int,int)",
                        {dart::fcb::Value::Int(2), dart::fcb::Value::Int(3),
                         dart::fcb::Value::Int(5)});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 10);
}

void InterpretsConditionalsStringsAndNulls() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(ConditionalStringModule(), &error));

  const dart::fcb::InterpretResult true_result =
      runtime.Interpret("package:app/message.dart::message(bool)",
                        {dart::fcb::Value::Bool(true)});
  assert(true_result.ok);
  assert(true_result.value.kind == dart::fcb::ValueKind::kString);
  assert(true_result.value.string_value == "patched");

  const dart::fcb::InterpretResult false_result =
      runtime.Interpret("package:app/message.dart::message(bool)",
                        {dart::fcb::Value::Bool(false)});
  assert(false_result.ok);
  assert(false_result.value.kind == dart::fcb::ValueKind::kNull);
}

void InterpretsListLiteral() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(ListModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/list.dart::items()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kList);
  assert(result.value.list_value.size() == 2);
  assert(result.value.list_value[0].kind == dart::fcb::ValueKind::kInt);
  assert(result.value.list_value[0].int_value == 7);
  assert(result.value.list_value[1].kind == dart::fcb::ValueKind::kString);
  assert(result.value.list_value[1].string_value == "days");
}

void InterpretsMapLiteral() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(MapModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/map.dart::attrs()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kMap);
  assert(result.value.map_entries.size() == 4);
  assert(result.value.map_entries[0].string_value == "price");
  assert(result.value.map_entries[1].int_value == 9);
  assert(result.value.map_entries[2].string_value == "active");
  assert(result.value.map_entries[3].bool_value);
}

void InterpretsMixedNumericOps() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(DoubleMathModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/math.dart::doubleAdd()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kDouble);
  assert(result.value.double_value == 6.5);
}

void InterpretsDivisionAsDouble() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(DivisionModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/math.dart::divide()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kDouble);
  assert(result.value.double_value == 3.5);
}

void RejectsArgumentCountMismatch() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(AddModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/math.dart::add(int,int)",
                        {dart::fcb::Value::Int(3)});
  assert(!result.ok);
  assert(result.error.find("argument count mismatch") != std::string::npos);
}

void DisabledPatchDoesNotInterpret() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(TestModule(), &error));
  assert(runtime.DisablePatch("package:app/pricing.dart::price(int)"));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/pricing.dart::price(int)",
                        {dart::fcb::Value::Int(7)});
  assert(!result.ok);
  assert(result.error.find("not active") != std::string::npos);
}

void RejectsInvalidModule() {
  dart::fcb::BytecodeModule module = TestModule();
  module.functions[0].bytecode_length = 99;
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(!runtime.LoadModule(module, &error));
  assert(error.find("range exceeds") != std::string::npos);
  assert(runtime.patch_count() == 0);
}

void RejectsJumpIntoOperand() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x30, 0x00, 0x02,  // Jump 2, into this jump's operand bytes.
      0x01, 0x00, 0x00,
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/bad.dart::jump()";
  function.parameter_count = 0;
  function.register_count = 0;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(1));
  module.functions.push_back(std::move(function));

  std::string error;
  assert(!dart::fcb::ValidateModule(module, &error));
  assert(error.find("non-instruction offset") != std::string::npos);
}

void RejectsMissingConstantReference() {
  dart::fcb::BytecodeModule module = TestModule();
  module.functions[0].constants.clear();

  std::string error;
  assert(!dart::fcb::ValidateModule(module, &error));
  assert(error.find("missing constant") != std::string::npos);
}

void LoadsModuleFromFcbJsonFile() {
  const char* path = "/tmp/fcb_sdk_patch_runtime_module.json";
  {
    std::ofstream file(path);
    file << R"fcb({
      "version": 1,
      "functions": [
        {
          "name": "package:app/pricing.dart::price(int)",
          "param_count": 1,
          "local_count": 2,
          "constants": [{"type": "Int", "value": 2}],
          "code": [1, 0, 0, 255]
        }
      ]
    })fcb";
  }

  dart::fcb::BytecodeModule module;
  std::string error;
  if (!dart::fcb::LoadBytecodeModuleFromFile(path, &module, &error)) {
    std::cerr << error << "\n";
    assert(false);
  }
  assert(module.functions.size() == 1);
  assert(module.bytecode.size() == 4);
  assert(module.functions[0].bytecode_offset == 0);
  assert(module.functions[0].bytecode_length == 4);
  assert(module.functions[0].constants.size() == 1);
  assert(module.functions[0].constants[0].kind == dart::fcb::ValueKind::kInt);
  assert(module.functions[0].constants[0].int_value == 2);

  dart::fcb::PatchRuntime runtime;
  assert(runtime.LoadModuleFromFile(path, &error));
  const dart::fcb::DispatchDecision decision =
      runtime.Resolve("package:app/pricing.dart::price(int)");
  assert(decision.state == dart::fcb::PatchState::kPatchedInterpreted);
  assert(decision.function != nullptr);
}

void LoadsSignedIntAndDoubleFromFcbJsonFile() {
  const char* path = "/tmp/fcb_sdk_patch_runtime_numbers.json";
  {
    std::ofstream file(path);
    file << R"fcb({
      "version": 1,
      "functions": [
        {
          "name": "package:app/nums.dart::neg()",
          "param_count": 0,
          "local_count": 0,
          "constants": [
            {"type": "Int", "value": -7},
            {"type": "Double", "value": -3.25}
          ],
          "code": [1, 0, 0, 255]
        }
      ]
    })fcb";
  }

  dart::fcb::BytecodeModule module;
  std::string error;
  assert(dart::fcb::LoadBytecodeModuleFromFile(path, &module, &error));
  assert(module.functions[0].constants.size() == 2);
  assert(module.functions[0].constants[0].kind == dart::fcb::ValueKind::kInt);
  assert(module.functions[0].constants[0].int_value == -7);
  assert(module.functions[0].constants[1].kind ==
         dart::fcb::ValueKind::kDouble);
  assert(module.functions[0].constants[1].double_value == -3.25);
}

}  // namespace

int main() {
  LoadsAndResolvesPatchedFunction();
  DisabledPatchFallsBackWithoutFunction();
  InterpretsPatchedFunction();
  InterpretsArgumentsAndBinaryOps();
  InterpretsThreeArgumentFunction();
  InterpretsConditionalsStringsAndNulls();
  InterpretsListLiteral();
  InterpretsMapLiteral();
  InterpretsMixedNumericOps();
  InterpretsDivisionAsDouble();
  RejectsArgumentCountMismatch();
  DisabledPatchDoesNotInterpret();
  RejectsInvalidModule();
  RejectsJumpIntoOperand();
  RejectsMissingConstantReference();
  LoadsModuleFromFcbJsonFile();
  LoadsSignedIntAndDoubleFromFcbJsonFile();
  return 0;
}
