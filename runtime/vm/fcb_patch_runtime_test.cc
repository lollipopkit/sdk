// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
#include "lib/stacktrace.h"
#include "vm/class_finalizer.h"
#include "vm/heap/heap.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/symbols.h"
#include "vm/thread.h"
#endif
#include "vm/unit_test.h"
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#endif
void RunFcbPatchRuntimeLoaderStandaloneTests();
namespace {

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
dart::ClassPtr CreateTestClass(dart::Thread* thread,
                               const dart::Library& library,
                               const char* name) {
  dart::Zone* zone = thread->zone();
  const dart::String& class_name =
      dart::String::Handle(zone, dart::Symbols::New(thread, name));
  const dart::Script& script = dart::Script::Handle(zone);
  const dart::Class& cls = dart::Class::Handle(
      zone, dart::Class::New(library, class_name, script,
                             dart::TokenPosition::kNoSource));
  cls.set_is_synthesized_class_unsafe();
  cls.set_is_declaration_loaded_unsafe();
  library.AddClass(cls);
  dart::ClassFinalizer::FinalizeTypesInClass(cls);
  {
    dart::SafepointWriteRwLocker ml(
        thread, thread->isolate_group()->program_lock());
    cls.Finalize();
  }
  return cls.ptr();
}
#endif

dart::fcb::BytecodeModule TestModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {0x01, 0x00, 0x00, 0xff};
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/pricing.dart::price(int)";
  function.parameter_count = 1;
  function.register_count = 2;
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
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("price"));
  function.constants.push_back(dart::fcb::Value::Int(9));
  function.constants.push_back(dart::fcb::Value::String("active"));
  function.constants.push_back(dart::fcb::Value::Bool(true));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule GetFieldModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0.
      0x43, 0x00, 0x00,  // GetField "price".
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/field.dart::price(Map)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("price"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule SetFieldModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0.
      0x01, 0x00, 0x01,  // LoadConst 42.
      0x44, 0x00, 0x00,  // SetField "price".
      0x43, 0x00, 0x00,  // GetField "price".
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/field.dart::setPrice(Map)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("price"));
  function.constants.push_back(dart::fcb::Value::Int(42));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule BadGetFieldModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst 7.
      0x43, 0x00, 0x01,  // GetField "price".
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/field.dart::badGet()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(7));
  function.constants.push_back(dart::fcb::Value::String("price"));
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
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(7));
  function.constants.push_back(dart::fcb::Value::Int(2));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule StringConcatModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst "Hello, ".
      0x02, 0x00,        // LoadArg 0.
      0x01, 0x00, 0x01,  // LoadConst "!".
      0x42, 0x00, 0x03,  // StringConcat 3.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/message.dart::greet(String)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("Hello, "));
  function.constants.push_back(dart::fcb::Value::String("!"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule StringConcatCoercionModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst "count=".
      0x01, 0x00, 0x01,  // LoadConst 7.
      0x01, 0x00, 0x02,  // LoadConst ", active=".
      0x01, 0x00, 0x03,  // LoadConst true.
      0x01, 0x00, 0x04,  // LoadConst ", note=".
      0x01, 0x00, 0x05,  // LoadConst null.
      0x42, 0x00, 0x06,  // StringConcat 6.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/message.dart::summary()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("count="));
  function.constants.push_back(dart::fcb::Value::Int(7));
  function.constants.push_back(dart::fcb::Value::String(", active="));
  function.constants.push_back(dart::fcb::Value::Bool(true));
  function.constants.push_back(dart::fcb::Value::String(", note="));
  function.constants.push_back(dart::fcb::Value::Null());
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule BadStringConcatModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst "only one".
      0x42, 0x00, 0x02,  // StringConcat 2.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/message.dart::badConcat()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("only one"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule IsTypeModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0.
      0x45, 0x00, 0x00,  // IsType "num".
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/type.dart::isNum(Object)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("num"));
  module.functions.push_back(std::move(function));
  return module;
}

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
dart::fcb::BytecodeModule UserTypeModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0.
      0x45, 0x00, 0x00,  // IsType "<test-uri>::User".
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/type.dart::isUser(Object)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("fcb:test::User"));
  module.functions.push_back(std::move(function));
  return module;
}
#endif

dart::fcb::BytecodeModule AsTypeModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0.
      0x46, 0x00, 0x00,  // AsType "String".
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/type.dart::asString(Object)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("String"));
  module.functions.push_back(std::move(function));
  return module;
}

void WriteU8(std::vector<uint8_t>* out, uint8_t value) {
  out->push_back(value);
}

void WriteU16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value));
}

void WriteU32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value >> 24));
  out->push_back(static_cast<uint8_t>(value >> 16));
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value));
}

void WriteI64(std::vector<uint8_t>* out, int64_t value) {
  const uint64_t raw = static_cast<uint64_t>(value);
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<uint8_t>(raw >> shift));
  }
}

void WriteString(std::vector<uint8_t>* out, const std::string& value) {
  WriteU16(out, static_cast<uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<uint8_t> BinaryFcbmModule(uint32_t source_map_offset) {
  std::vector<uint8_t> out = {'F', 'C', 'B', 'M'};
  WriteU32(&out, 1);  // version
  WriteU16(&out, 1);  // function count
  WriteString(&out, "package:app/main.dart::mainValue()");
  WriteU8(&out, 0);  // tagged return convention
  WriteU8(&out, 0);  // param_count
  WriteU8(&out, 1);  // local_count
  WriteU16(&out, 2);  // constants
  WriteU8(&out, 1);  // Int
  WriteI64(&out, 3);
  WriteU8(&out, 4);  // String
  WriteString(&out, "package:app/main.dart::helper()");
  const std::vector<uint8_t> code = {
      0x01, 0x00, 0x00,  // LoadConst 0.
      0xff,              // Return.
  };
  WriteU32(&out, static_cast<uint32_t>(code.size()));
  out.insert(out.end(), code.begin(), code.end());
  WriteU16(&out, 1);  // source map count
  WriteU32(&out, source_map_offset);
  WriteString(&out, "package:app/main.dart:7:10");
  return out;
}

dart::fcb::BytecodeModule CallStaticModule(dart::fcb::Value constant) {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x50, 0x00, 0x00, 0x00,  // CallStatic constant 0, argc 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/main.dart::mainValue()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(std::move(constant));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallStaticInterpretModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;

  const std::vector<uint8_t> helper_code = {
      0x01, 0x00, 0x00,  // LoadConst 2.
      0xff,              // Return.
  };
  dart::fcb::BytecodeFunction helper;
  helper.function_id = "package:app/main.dart::helper()";
  helper.parameter_count = 0;
  helper.register_count = 0;
  helper.bytecode_offset = 0;
  helper.bytecode_length = static_cast<uint32_t>(helper_code.size());
  helper.constants.push_back(dart::fcb::Value::Int(2));
  module.bytecode.insert(module.bytecode.end(), helper_code.begin(),
                         helper_code.end());

  const std::vector<uint8_t> main_code = {
      0x50, 0x00, 0x00, 0x00,  // CallStatic helper, argc 0.
      0x01, 0x00, 0x01,        // LoadConst 1.
      0x10,                    // Add.
      0xff,                    // Return.
  };
  dart::fcb::BytecodeFunction main;
  main.function_id = "package:app/main.dart::mainValue()";
  main.parameter_count = 0;
  main.register_count = 0;
  main.bytecode_offset = static_cast<uint32_t>(module.bytecode.size());
  main.bytecode_length = static_cast<uint32_t>(main_code.size());
  main.constants.push_back(
      dart::fcb::Value::String("package:app/main.dart::helper()"));
  main.constants.push_back(dart::fcb::Value::Int(1));
  module.bytecode.insert(module.bytecode.end(), main_code.begin(),
                         main_code.end());

  module.functions.push_back(std::move(helper));
  module.functions.push_back(std::move(main));
  return module;
}

dart::fcb::BytecodeModule CallStaticArgumentModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;

  const std::vector<uint8_t> helper_code = {
      0x02, 0x00,  // LoadArg 0.
      0xff,        // Return.
  };
  dart::fcb::BytecodeFunction helper;
  helper.function_id = "package:app/main.dart::identity(int)";
  helper.parameter_count = 1;
  helper.register_count = 1;
  helper.bytecode_offset = 0;
  helper.bytecode_length = static_cast<uint32_t>(helper_code.size());
  module.bytecode.insert(module.bytecode.end(), helper_code.begin(),
                         helper_code.end());

  const std::vector<uint8_t> main_code = {
      0x02, 0x00,              // LoadArg 0.
      0x50, 0x00, 0x00, 0x01,  // CallStatic identity, argc 1.
      0x01, 0x00, 0x01,        // LoadConst 1.
      0x10,                    // Add.
      0xff,                    // Return.
  };
  dart::fcb::BytecodeFunction main;
  main.function_id = "package:app/main.dart::plusOne(int)";
  main.parameter_count = 1;
  main.register_count = 1;
  main.bytecode_offset = static_cast<uint32_t>(module.bytecode.size());
  main.bytecode_length = static_cast<uint32_t>(main_code.size());
  main.constants.push_back(
      dart::fcb::Value::String("package:app/main.dart::identity(int)"));
  main.constants.push_back(dart::fcb::Value::Int(1));
  module.bytecode.insert(module.bytecode.end(), main_code.begin(),
                         main_code.end());

  module.functions.push_back(std::move(helper));
  module.functions.push_back(std::move(main));
  return module;
}

dart::fcb::BytecodeModule MissingCallStaticTargetModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x50, 0x00, 0x00, 0x00,  // CallStatic missing target, argc 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/main.dart::broken()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("package:app/main.dart::missing()"));
  module.functions.push_back(std::move(function));
  return module;
}

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
dart::fcb::BytecodeModule CallDynamicModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "dart-vm".
      0x01, 0x00, 0x02,        // LoadConst 5.
      0x51, 0x00, 0x00, 0x01,  // CallDynamic substring, argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/dynamic.dart::substring()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("substring"));
  function.constants.push_back(dart::fcb::Value::String("dart-vm"));
  function.constants.push_back(dart::fcb::Value::Int(5));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "same".
      0x01, 0x00, 0x01,        // LoadConst "same".
      0x52, 0x00, 0x00, 0x02,  // CallOriginal dart:core::identical, argc 2.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/original.dart::identicalConst()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("dart:core::identical"));
  function.constants.push_back(dart::fcb::Value::String("same"));
  module.functions.push_back(std::move(function));
  return module;
}
#endif

dart::fcb::BytecodeModule CallClosureModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,              // LoadArg 0 closure.
      0x02, 0x01,              // LoadArg 1.
      0x02, 0x02,              // LoadArg 2.
      0x53, 0x00, 0x00, 0x02,  // CallClosure argc 2.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id =
      "package:app/closure.dart::callPredicate(Function,Object,Object)";
  function.parameter_count = 3;
  function.register_count = 3;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule MakeClosureCallModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x54, 0x00, 0x00,        // MakeClosure dart:core::identical.
      0x02, 0x00,              // LoadArg 0.
      0x02, 0x00,              // LoadArg 0.
      0x53, 0x00, 0x00, 0x02,  // CallClosure argc 2.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/closure.dart::selfIdentical(Object)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::identical"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule NewObjectModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x55, 0x00, 0x00, 0x00,  // NewObject dart:core::Object., argc 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/object.dart::makeObject()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:Object."));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule NewObjectFactoryModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,        // LoadConst 0: code point 65.
      0x55, 0x00, 0x01, 0x01,  // NewObject String.fromCharCode, argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/object.dart::makeString()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(65));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:String.fromCharCode"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule NewObjectNamedFactoryModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,        // LoadConst 0: scheme.
      0x01, 0x00, 0x01,        // LoadConst 1: path.
      0x55, 0x00, 0x02, 0x02,  // NewObject Uri named args, argc 2.
      0x51, 0x00, 0x03, 0x00,  // CallDynamic toString, argc 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/object.dart::makeUriString()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("https"));
  function.constants.push_back(dart::fcb::Value::String("/patched"));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:Uri.;named:scheme,path"));
  function.constants.push_back(dart::fcb::Value::String("toString"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule NewObjectGenericModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,        // LoadConst 0: length.
      0x01, 0x00, 0x01,        // LoadConst 1: fill.
      0x55, 0x00, 0x02, 0x02,  // NewObject List<String>.filled, argc 2.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/object.dart::makeBox()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::Int(2));
  function.constants.push_back(dart::fcb::Value::String("patched-box"));
  function.constants.push_back(dart::fcb::Value::String(
      "dart:core::class:List.filled;types:String"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule SourceMappedErrorModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {0xff};  // Return with an empty stack.
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/bad.dart::broken()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.source_map.push_back(
      {0, "package:app/bad.dart:9:3"});
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
  (void)decision;
  assert(decision.state == dart::fcb::PatchState::kPatchedInterpreted);
  assert(decision.function != nullptr);
  assert(decision.function->parameter_count == 1);

  const dart::fcb::DispatchDecision miss =
      runtime.Resolve("package:app/pricing.dart::unchanged()");
  (void)miss;
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
  (void)decision;
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

void InterpretsGetFieldFromMap() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(GetFieldModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/field.dart::price(Map)",
                        {dart::fcb::Value::Map({
                            dart::fcb::Value::String("price"),
                            dart::fcb::Value::Int(9),
                        })});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 9);
}

void InterpretsSetFieldOnMap() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(SetFieldModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/field.dart::setPrice(Map)",
                        {dart::fcb::Value::Map({
                            dart::fcb::Value::String("price"),
                            dart::fcb::Value::Int(9),
                        })});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 42);
}

void RejectsGetFieldOnNonMapReceiver() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(BadGetFieldModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/field.dart::badGet()", {});
  assert(!result.ok);
  assert(result.error.find("GetField failed") != std::string::npos);
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

void InterpretsStringConcat() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(StringConcatModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/message.dart::greet(String)",
                        {dart::fcb::Value::String("FCB")});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kString);
  assert(result.value.string_value == "Hello, FCB!");
}

void InterpretsStringConcatScalarCoercions() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(StringConcatCoercionModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/message.dart::summary()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kString);
  assert(result.value.string_value == "count=7, active=true, note=null");
}

void RejectsStringConcatStackUnderflow() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(BadStringConcatModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/message.dart::badConcat()", {});
  assert(!result.ok);
  assert(result.error.find("StringConcat stack underflow") !=
         std::string::npos);
}

void InterpretsIsType() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(IsTypeModule(), &error));

  dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/type.dart::isNum(Object)",
                        {dart::fcb::Value::Int(7)});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kBool);
  assert(result.value.bool_value);

  result = runtime.Interpret("package:app/type.dart::isNum(Object)",
                             {dart::fcb::Value::String("seven")});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kBool);
  assert(!result.value.bool_value);
}

void InterpretsAsType() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(AsTypeModule(), &error));

  dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/type.dart::asString(Object)",
                        {dart::fcb::Value::String("ok")});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kString);
  assert(result.value.string_value == "ok");

  result = runtime.Interpret("package:app/type.dart::asString(Object)",
                             {dart::fcb::Value::Int(7)});
  assert(!result.ok);
  assert(result.error.find("AsType failed") != std::string::npos);
}

void RejectsCallClosureWithoutVmRuntime() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(CallClosureModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::callPredicate(Function,Object,Object)",
                        {dart::fcb::Value::String("not a closure"),
                         dart::fcb::Value::String("same"),
                         dart::fcb::Value::String("same")});
  assert(!result.ok);
  assert(result.error.find("CallClosure failed") != std::string::npos);
}

void RejectsMakeClosureWithoutVmRuntime() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(MakeClosureCallModule(), &error));
  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::selfIdentical(Object)",
                        {dart::fcb::Value::String("same")});
  assert(!result.ok);
  assert(result.error.find("MakeClosure failed") != std::string::npos);
}

void RejectsNewObjectWithoutVmRuntime() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(NewObjectModule(), &error));
  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/object.dart::makeObject()", {});
  assert(!result.ok);
  assert(result.error.find("NewObject failed") != std::string::npos);
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
  (void)decision;
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

void LoadsBinaryFcbmWithSourceMap() {
  const char* path = "/tmp/fcb_sdk_patch_runtime_module.fcbm";
  const std::vector<uint8_t> bytes = BinaryFcbmModule(0);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  dart::fcb::BytecodeModule module;
  std::string error;
  assert(dart::fcb::LoadBytecodeModuleFromFile(path, &module, &error));
  assert(module.version == 1);
  assert(module.functions.size() == 1);
  assert(module.functions[0].function_id ==
         "package:app/main.dart::mainValue()");
  assert(module.functions[0].constants.size() == 2);
  assert(module.functions[0].constants[0].int_value == 3);
  assert(module.functions[0].constants[1].string_value ==
         "package:app/main.dart::helper()");
  assert(module.functions[0].source_map.size() == 1);
  assert(module.functions[0].source_map[0].bytecode_offset == 0);
  assert(module.functions[0].source_map[0].source_location ==
         "package:app/main.dart:7:10");
  dart::fcb::PatchRuntime runtime;
  assert(runtime.LoadModuleFromFile(path, &error));
  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/main.dart::mainValue()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 3);
}

void RejectsOutOfRangeSourceMapInBinaryFcbm() {
  const char* path = "/tmp/fcb_sdk_patch_runtime_bad_source_map.fcbm";
  const std::vector<uint8_t> bytes = BinaryFcbmModule(99);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  dart::fcb::BytecodeModule module;
  std::string error;
  assert(!dart::fcb::LoadBytecodeModuleFromFile(path, &module, &error));
  assert(error.find("source_map") != std::string::npos);
}

void ValidatesCallStaticFunctionConstant() {
  std::string error;
  assert(dart::fcb::ValidateModule(
      CallStaticModule(dart::fcb::Value::String(
          "package:app/main.dart::helper()")),
      &error));
  assert(!dart::fcb::ValidateModule(
      CallStaticModule(dart::fcb::Value::Int(7)), &error));
  assert(error.find("CallStatic") != std::string::npos);
}

void InterpretsCallStaticInPatchModule() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(CallStaticInterpretModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/main.dart::mainValue()", {});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 3);
}

void InterpretsCallStaticArguments() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(CallStaticArgumentModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/main.dart::plusOne(int)",
                        {dart::fcb::Value::Int(8)});
  assert(result.ok);
  assert(result.value.kind == dart::fcb::ValueKind::kInt);
  assert(result.value.int_value == 9);
}

void RejectsMissingCallStaticTarget() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(MissingCallStaticTargetModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/main.dart::broken()", {});
  assert(!result.ok);
  assert(result.error.find("CallStatic failed") != std::string::npos);
  assert(result.error.find("not active") != std::string::npos);
}

void InterpreterErrorsIncludePatchSourceLocation() {
  dart::fcb::PatchRuntime runtime;
  std::string error;
  assert(runtime.LoadModule(SourceMappedErrorModule(), &error));

  const dart::fcb::InterpretResult result =
      runtime.Interpret("package:app/bad.dart::broken()", {});
  assert(!result.ok);
  assert(result.error.find("Return stack underflow") != std::string::npos);
  assert(result.error.find("package:app/bad.dart:9:3") != std::string::npos);
  assert(result.error.find("FCB patch") != std::string::npos);
}

void RunFcbPatchRuntimeTest() {
  LoadsAndResolvesPatchedFunction();
  DisabledPatchFallsBackWithoutFunction();
  InterpretsPatchedFunction();
  InterpretsArgumentsAndBinaryOps();
  InterpretsThreeArgumentFunction();
  InterpretsConditionalsStringsAndNulls();
  InterpretsListLiteral();
  InterpretsMapLiteral();
  InterpretsGetFieldFromMap();
  InterpretsSetFieldOnMap();
  RejectsGetFieldOnNonMapReceiver();
  InterpretsMixedNumericOps();
  InterpretsDivisionAsDouble();
  InterpretsStringConcat();
  InterpretsStringConcatScalarCoercions();
  RejectsStringConcatStackUnderflow();
  InterpretsIsType();
  InterpretsAsType();
#if defined(FCB_PATCH_RUNTIME_STANDALONE)
  RejectsCallClosureWithoutVmRuntime();
  RejectsMakeClosureWithoutVmRuntime();
  RejectsNewObjectWithoutVmRuntime();
#endif
  RejectsArgumentCountMismatch();
  DisabledPatchDoesNotInterpret();
  RejectsInvalidModule();
  RejectsJumpIntoOperand();
  RejectsMissingConstantReference();
  LoadsModuleFromFcbJsonFile();
  LoadsSignedIntAndDoubleFromFcbJsonFile();
  LoadsBinaryFcbmWithSourceMap();
  RunFcbPatchRuntimeLoaderStandaloneTests();
  RejectsOutOfRangeSourceMapInBinaryFcbm();
  ValidatesCallStaticFunctionConstant();
  InterpretsCallStaticInPatchModule();
  InterpretsCallStaticArguments();
  RejectsMissingCallStaticTarget();
  InterpreterErrorsIncludePatchSourceLocation();
}

}  // namespace

void RunFcbPatchRuntimeTryStandaloneTests();

#if defined(FCB_PATCH_RUNTIME_STANDALONE)
int main() {
  RunFcbPatchRuntimeTest();
  RunFcbPatchRuntimeTryStandaloneTests();
  return 0;
}
#else
namespace dart {

UNIT_TEST_CASE(FcbPatchRuntime) {
  RunFcbPatchRuntimeTest();
  RunFcbPatchRuntimeTryStandaloneTests();
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallDynamic) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallDynamicModule(), &error));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/dynamic.dart::substring()", {});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("vm", result.value.string_value.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallOriginal) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallOriginalModule(), &error));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/original.dart::identicalConst()", {});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(result.value.bool_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeIsType) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(IsTypeModule(), &error));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/type.dart::isNum(Object)", {fcb::Value::Int(7)});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(result.value.bool_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeUserDefinedIsType) {
  Zone* zone = thread->zone();
  const String& url = String::Handle(zone, Symbols::New(thread, "fcb:test"));
  const Library& library = Library::Handle(zone, Library::New(url));
  library.Register(thread);
  const Class& user_class =
      Class::Handle(zone, CreateTestClass(thread, library, "User"));
  const Instance& user = Instance::Handle(zone, Instance::New(user_class));

  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(UserTypeModule(), &error));

  fcb::InterpretResult result = runtime.Interpret(
      "package:app/type.dart::isUser(Object)",
      {fcb::Value::FromDart(user.ptr())});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(result.value.bool_value);

  result = runtime.Interpret("package:app/type.dart::isUser(Object)",
                             {fcb::Value::String("not user")});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(!result.value.bool_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallClosure) {
  Zone* zone = thread->zone();
  const String& core_url = String::Handle(zone, String::New("dart:core"));
  const Library& core =
      Library::Handle(zone, Library::LookupLibrary(thread, core_url));
  EXPECT(!core.IsNull());
  const String& name = String::Handle(zone, String::New("identical"));
  const Function& target =
      Function::Handle(zone, core.LookupFunctionAllowPrivate(name));
  EXPECT(!target.IsNull());
  const Function& closure_function =
      Function::Handle(zone, target.ImplicitClosureFunction());
  const Closure& closure =
      Closure::Handle(zone, closure_function.ImplicitStaticClosure());

  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallClosureModule(), &error));
  const String& same = String::Handle(zone, String::New("same"));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/closure.dart::callPredicate(Function,Object,Object)",
      {fcb::Value::FromDart(closure.ptr()), fcb::Value::FromDart(same.ptr()),
       fcb::Value::FromDart(same.ptr())});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(result.value.bool_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeMakeClosure) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(MakeClosureCallModule(), &error));
  const String& same = String::Handle(thread->zone(), String::New("same"));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/closure.dart::selfIdentical(Object)",
      {fcb::Value::FromDart(same.ptr())});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(result.value.bool_value);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeNewObject) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/object.dart::makeObject()", {});
  EXPECT(result.ok);
  EXPECT(result.value.object_value != nullptr);
  const Object& object = Object::Handle(thread->zone(), result.value.ToDart());
  EXPECT(object.IsInstance());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeNewObjectFactory) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectFactoryModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/object.dart::makeString()", {});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  const String& string =
      String::Cast(Object::Handle(thread->zone(), result.value.ToDart()));
  EXPECT_STREQ("A", string.ToCString());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeNewObjectNamedFactory) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectNamedFactoryModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/object.dart::makeUriString()", {});
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  const String& string =
      String::Cast(Object::Handle(thread->zone(), result.value.ToDart()));
  EXPECT_SUBSTRING("https:", string.ToCString());
  EXPECT_SUBSTRING("patched", string.ToCString());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeNewObjectGeneric) {
  Zone* zone = thread->zone();
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(NewObjectGenericModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/object.dart::makeBox()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeNewObjectGeneric failed: %s\n",
                 result.error.c_str());
    EXPECT(result.ok);
    return;
  }
  EXPECT(result.ok);
  EXPECT(result.value.object_value != nullptr);
  if (result.value.object_value == nullptr) {
    return;
  }

  const Instance& instance =
      Instance::Cast(Object::Handle(zone, result.value.ToDart()));
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, instance.GetTypeArguments());
  EXPECT(!type_arguments.IsNull());
  EXPECT(type_arguments.Length() == 1);
  const AbstractType& type =
      AbstractType::Handle(zone, type_arguments.TypeAt(0));
  EXPECT_STREQ("String", String::Handle(zone, type.Name()).ToCString());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeStackTraceSourceLocation) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(SourceMappedErrorModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/bad.dart::broken()", {});
  EXPECT(!result.ok);

  const StackTrace& stack_trace =
      StackTrace::Handle(thread->zone(), GetCurrentStackTrace(0).ptr());
  EXPECT(stack_trace.Length() > 0);
  const Object& fcb_frame = Object::Handle(
      thread->zone(), stack_trace.CodeAtFrame(stack_trace.Length() - 1));
  EXPECT(fcb_frame.IsString());
  EXPECT_STREQ("package:app/bad.dart:9:3",
               String::Cast(fcb_frame).ToCString());

  const char* stack_trace_text = stack_trace.ToCString();
  EXPECT_SUBSTRING("package:app/bad.dart:9:3", stack_trace_text);
  EXPECT_SUBSTRING("FCB patch", stack_trace_text);
}

}  // namespace dart
#endif
