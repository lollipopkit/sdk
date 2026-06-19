// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <string>
#include <utility>

#include "vm/heap/heap.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/thread.h"
#include "vm/unit_test.h"

namespace {

dart::fcb::BytecodeModule GcRootStressModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00, 0xff,  // labels(): LoadConst 0; Return.
      0x01, 0x00, 0x00, 0xff,  // attrs(): LoadConst 0; Return.
  };
  dart::fcb::BytecodeFunction labels;
  labels.function_id = "package:app/gc.dart::labels()";
  labels.bytecode_length = 4;
  labels.constants.push_back(dart::fcb::Value::List({
      dart::fcb::Value::String("base"),
      dart::fcb::Value::String("patched"),
  }));
  module.functions.push_back(std::move(labels));

  dart::fcb::BytecodeFunction attrs;
  attrs.function_id = "package:app/gc.dart::attrs()";
  attrs.bytecode_offset = 4;
  attrs.bytecode_length = 4;
  attrs.constants.push_back(dart::fcb::Value::Map({
      dart::fcb::Value::String("price"),
      dart::fcb::Value::Int(9),
      dart::fcb::Value::String("active"),
      dart::fcb::Value::Bool(true),
  }));
  module.functions.push_back(std::move(attrs));
  return module;
}

}  // namespace

namespace dart {

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeGcStress) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(thread != nullptr);
  ASSERT(isolate_group != nullptr);

  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  EXPECT(runtime->LoadModule(GcRootStressModule(), &error));

  for (intptr_t i = 0; i < 100; i++) {
    {
      TransitionNativeToVM transition(thread);
      GCTestHelper::CollectAllGarbage(/*compact=*/true);
    }

    fcb::InterpretResult result =
        runtime->Interpret("package:app/gc.dart::labels()", {});
    EXPECT(result.ok);
    EXPECT(result.value.kind == fcb::ValueKind::kList);
    const Array& labels =
        Array::Cast(Object::Handle(thread->zone(), result.value.ToDart()));
    EXPECT_EQ(2, labels.Length());
    const String& base =
        String::Cast(Object::Handle(thread->zone(), labels.At(0)));
    const String& patched =
        String::Cast(Object::Handle(thread->zone(), labels.At(1)));
    EXPECT_STREQ("base", base.ToCString());
    EXPECT_STREQ("patched", patched.ToCString());

    fcb::InterpretResult map_result =
        runtime->Interpret("package:app/gc.dart::attrs()", {});
    EXPECT(map_result.ok);
    EXPECT(map_result.value.kind == fcb::ValueKind::kMap);
    const Object& map_object =
        Object::Handle(thread->zone(), map_result.value.ToDart());
    EXPECT(map_object.IsMap());
    const Map& map = Map::Cast(map_object);
    EXPECT_EQ(2, map.Length());
    Map::Iterator iterator(map);
    EXPECT(iterator.MoveNext());
    EXPECT_STREQ(
        "price",
        String::Cast(Object::Handle(thread->zone(), iterator.CurrentKey()))
            .ToCString());
    EXPECT_EQ(9,
              Integer::Cast(
                  Object::Handle(thread->zone(), iterator.CurrentValue()))
                  .Value());
    EXPECT(iterator.MoveNext());
    EXPECT_STREQ(
        "active",
        String::Cast(Object::Handle(thread->zone(), iterator.CurrentKey()))
            .ToCString());
    EXPECT(Bool::Cast(Object::Handle(thread->zone(), iterator.CurrentValue()))
               .value());
    EXPECT(!iterator.MoveNext());
  }

  isolate_group->ClearFcbPatchRuntime();
}

}  // namespace dart
