// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <string>
#include <utility>

#include "vm/object.h"
#include "vm/os.h"
#include "vm/unit_test.h"

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
namespace {

dart::fcb::BytecodeModule MapCrossesDynamicCallBoundaryModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x01,        // LoadConst "answer".
      0x01, 0x00, 0x02,        // LoadConst 42.
      0x41, 0x00, 0x01,        // MakeMap 1.
      0x01, 0x00, 0x01,        // LoadConst "answer".
      0x51, 0x00, 0x00, 0x01,  // CallDynamic containsKey("answer").
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/semantics.dart::mapContainsKey()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("containsKey"));
  function.constants.push_back(dart::fcb::Value::String("answer"));
  function.constants.push_back(dart::fcb::Value::Int(42));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule ListMaterializesAsDartArrayModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,  // LoadConst "first".
      0x01, 0x00, 0x01,  // LoadConst "second".
      0x40, 0x00, 0x02,  // MakeList 2.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/semantics.dart::makeList()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("first"));
  function.constants.push_back(dart::fcb::Value::String("second"));
  module.functions.push_back(std::move(function));
  return module;
}

}  // namespace

namespace dart {

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeMapCrossesDynamicCallBoundary) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(MapCrossesDynamicCallBoundaryModule(), &error));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/semantics.dart::mapContainsKey()", {});
  if (!result.ok) {
    OS::PrintErr(
        "FcbPatchRuntimeMapCrossesDynamicCallBoundary failed: %s\n",
        result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(result.value.bool_value);
  EXPECT(result.value.object_value != nullptr);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeListMaterializesAsDartArray) {
  Zone* zone = thread->zone();
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(ListMaterializesAsDartArrayModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/semantics.dart::makeList()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeListMaterializesAsDartArray failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kList);
  const Object& object = Object::Handle(zone, result.value.ToDart());
  EXPECT(object.IsArray());
  const Array& array = Array::Cast(object);
  EXPECT(array.Length() == 2);
  EXPECT_STREQ("first",
               String::Cast(Object::Handle(zone, array.At(0))).ToCString());
  EXPECT_STREQ("second",
               String::Cast(Object::Handle(zone, array.At(1))).ToCString());
}

}  // namespace dart
#endif  // !defined(FCB_PATCH_RUNTIME_STANDALONE)
