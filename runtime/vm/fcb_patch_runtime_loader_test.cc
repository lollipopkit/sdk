// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <cassert>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

void WriteU8(std::vector<uint8_t>* out, uint8_t value) {
  out->push_back(value);
}

void WriteU16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value));
}

void WriteU32(std::vector<uint8_t>* out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->push_back(static_cast<uint8_t>(value >> shift));
  }
}

void WriteString(std::vector<uint8_t>* out, const std::string& value) {
  WriteU16(out, static_cast<uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<uint8_t> BinaryFcbmV2ModuleWithDebugLocals() {
  std::vector<uint8_t> out = {'F', 'C', 'B', 'M'};
  WriteU32(&out, 2);  // version
  WriteU16(&out, 1);  // function count
  WriteString(&out, "package:app/main.dart::debugValue()");
  WriteU8(&out, 0);  // tagged return convention
  WriteU8(&out, 1);  // param_count
  WriteU8(&out, 1);  // local_count
  WriteU16(&out, 0);  // constants
  const std::vector<uint8_t> code = {
      0x02, 0x00,  // LoadArg 0.
      0xff,        // Return.
  };
  WriteU32(&out, static_cast<uint32_t>(code.size()));
  out.insert(out.end(), code.begin(), code.end());
  WriteU16(&out, 0);  // source map count
  WriteU16(&out, 1);  // debug locals count
  WriteU16(&out, 0);  // slot
  WriteString(&out, "input");
  return out;
}

void LoadsBinaryFcbmV2WithDebugLocals() {
  const char* path = "/tmp/fcb_sdk_patch_runtime_module_v2.fcbm";
  const std::vector<uint8_t> bytes = BinaryFcbmV2ModuleWithDebugLocals();
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  dart::fcb::BytecodeModule module;
  std::string error;
  assert(dart::fcb::LoadBytecodeModuleFromFile(path, &module, &error));
  assert(module.version == 2);
  assert(module.functions.size() == 1);
  assert(module.functions[0].debug_locals.size() == 1);
  assert(module.functions[0].debug_locals[0].slot == 0);
  assert(module.functions[0].debug_locals[0].name == "input");

  dart::fcb::PatchRuntime runtime;
  assert(runtime.LoadModuleFromFile(path, &error));
  const dart::fcb::InterpretResult result = runtime.Interpret(
      "package:app/main.dart::debugValue()", {dart::fcb::Value::Int(9)});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 9);
}

dart::fcb::BytecodeModule ModuleWithSingleFunction(
    const std::vector<uint8_t>& code,
    std::vector<dart::fcb::Value> constants) {
  dart::fcb::BytecodeModule module;
  module.version = 2;
  module.bytecode = code;
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/main.dart::bad()";
  function.return_convention = dart::fcb::ReturnConvention::kTagged;
  function.parameter_count = 0;
  function.register_count = 0;
  function.bytecode_offset = 0;
  function.bytecode_length = static_cast<uint32_t>(code.size());
  function.constants = std::move(constants);
  module.functions.push_back(std::move(function));
  return module;
}

void RejectsMissingStringOperandsDuringValidation() {
  std::string error;
  bool ok = dart::fcb::ValidateModule(
      ModuleWithSingleFunction({0x54, 0x00, 0x00, 0xff}, {}), &error);
  assert(!ok);
  assert(error.find("MakeClosure") != std::string::npos);
  assert(error.find("missing string constant") != std::string::npos);

  error.clear();
  ok = dart::fcb::ValidateModule(
      ModuleWithSingleFunction({0x55, 0x00, 0x00, 0x00, 0xff}, {}), &error);
  assert(!ok);
  assert(error.find("NewObject") != std::string::npos);
  assert(error.find("missing string constant") != std::string::npos);
}

void RejectsBadCallClosureMetadataDuringValidation() {
  std::string error;
  bool ok = dart::fcb::ValidateModule(
      ModuleWithSingleFunction({0x53, 0x00, 0x01, 0x00, 0xff}, {}), &error);
  assert(!ok);
  assert(error.find("CallClosure") != std::string::npos);
  assert(error.find("missing metadata constant") != std::string::npos);

  error.clear();
  ok = dart::fcb::ValidateModule(
      ModuleWithSingleFunction({0x53, 0x00, 0x01, 0x00, 0xff},
                               {dart::fcb::Value::String("path")}),
      &error);
  assert(!ok);
  assert(error.find("CallClosure metadata") != std::string::npos);
  assert(error.find(";named:") != std::string::npos);
}

void RejectsTooManyNamedArgumentsDuringValidation() {
  std::string error;
  bool ok = dart::fcb::ValidateModule(
      ModuleWithSingleFunction({0x55, 0x00, 0x00, 0x01, 0xff},
                               {dart::fcb::Value::String(
                                   "dart:core::class:Uri.;named:path,query")}),
      &error);
  assert(!ok);
  assert(error.find("NewObject") != std::string::npos);
  assert(error.find("too many named") != std::string::npos);

  error.clear();
  ok = dart::fcb::ValidateModule(
      ModuleWithSingleFunction({0x53, 0x00, 0x01, 0x01, 0xff},
                               {dart::fcb::Value::String(";named:path,query")}),
      &error);
  assert(!ok);
  assert(error.find("CallClosure") != std::string::npos);
  assert(error.find("too many named") != std::string::npos);
}

}  // namespace

void RunFcbPatchRuntimeLoaderStandaloneTests() {
  LoadsBinaryFcbmV2WithDebugLocals();
  RejectsMissingStringOperandsDuringValidation();
  RejectsBadCallClosureMetadataDuringValidation();
  RejectsTooManyNamedArgumentsDuringValidation();
}
