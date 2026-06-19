// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"
#include "vm/fcb_patch_runtime_internal.h"

#include <string>
#include <utility>

#include "vm/dart_entry.h"
#include "vm/object.h"
#include "vm/os.h"
#include "vm/resolver.h"
#include "vm/symbols.h"
#include "vm/thread.h"
#include "vm/unit_test.h"

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
namespace {

dart::fcb::BytecodeModule CallClosureNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,              // LoadArg 0 closure.
      0x01, 0x00, 0x01,        // LoadConst "new".
      0x53, 0x00, 0x01, 0x01,  // CallClosure ;named:path, argc 1.
      0x51, 0x00, 0x02, 0x00,  // CallDynamic toString().
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/closure.dart::callNamed(Function)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(";named:path"));
  function.constants.push_back(dart::fcb::Value::String("new"));
  function.constants.push_back(dart::fcb::Value::String("toString"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallClosureBadMetadataModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x53, 0x00, 0x01, 0x00,  // CallClosure with malformed metadata.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/closure.dart::badMetadata()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("named:path"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallClosureTooManyNamedModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,              // LoadArg 0 closure.
      0x01, 0x00, 0x01,        // LoadConst "new".
      0x53, 0x00, 0x01, 0x01,  // CallClosure ;named:path,extra, argc 1.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/closure.dart::tooManyNamed(Function)";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String(";named:path,extra"));
  function.constants.push_back(dart::fcb::Value::String("new"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule MakeClosureMissingTargetModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x54,
      0x00,
      0x00,  // MakeClosure dart:core::fcbMissingClosureTarget.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/closure.dart::missingTarget()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::fcbMissingClosureTarget"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule TryCatchesMakeClosureMissingTargetModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x0b, 0x00, 0x0f,  // TryBegin handler 11, end 15.
      0x54, 0x00, 0x00,              // MakeClosure missing target.
      0x30, 0x00, 0x0f,              // Jump 15.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x03, 0x00,                    // LoadLocal 0.
      0xff,                          // Return.
  };
  dart::fcb::BytecodeFunction function;
  function.function_id =
      "package:app/closure.dart::catchMissingClosureTarget()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::fcbMissingClosureTarget"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallClosureTryCatchesVmExceptionModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x12, 0x00, 0x16,  // TryBegin handler 18, end 22.
      0x54, 0x00, 0x00,              // MakeClosure int.parse.
      0x01, 0x00, 0x01,              // LoadConst "not-int".
      0x53, 0x00, 0x00, 0x01,        // CallClosure argc 1.
      0x30, 0x00, 0x16,              // Jump 22.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x03, 0x00,                    // LoadLocal 0.
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/closure.dart::catchClosureException()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:int.parse"));
  function.constants.push_back(dart::fcb::Value::String("not-int"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule CallOriginalWithBytecodeClosureArgumentModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  const uint32_t closure_offset = 0;
  module.bytecode.insert(module.bytecode.end(),
                         {
                             0x01,
                             0x00,
                             0x00,  // LoadConst "unused".
                             0xff,  // Return.
                         });
  const uint32_t call_offset = static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(),
                         {
                             0x01,
                             0x00,
                             0x00,  // LoadConst "captured".
                             0x54,
                             0x00,
                             0x02,  // MakeClosure bytecode target;captures:1.
                             0x01,
                             0x00,
                             0x01,  // LoadConst null.
                             0x52,
                             0x00,
                             0x03,
                             0x02,  // CallOriginal identical(closure, null).
                             0xff,
                         });

  dart::fcb::BytecodeFunction closure;
  closure.function_id = "package:app/closure.dart::internalClosure()";
  closure.parameter_count = 1;
  closure.register_count = 1;
  closure.bytecode_offset = closure_offset;
  closure.bytecode_length = 4;
  closure.constants.push_back(dart::fcb::Value::String("unused"));
  module.functions.push_back(std::move(closure));

  dart::fcb::BytecodeFunction function;
  function.function_id =
      "package:app/closure.dart::passBytecodeClosureToOriginal()";
  function.bytecode_offset = call_offset;
  function.bytecode_length = 14;
  function.constants.push_back(dart::fcb::Value::String(
      "captured"));
  function.constants.push_back(dart::fcb::Value::Null());
  function.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::internalClosure();captures:1"));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::identical"));
  module.functions.push_back(std::move(function));
  return module;
}

dart::fcb::BytecodeModule UriObjectModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x01, 0x00, 0x00,        // LoadConst "old".
      0x55, 0x00, 0x01, 0x01,  // NewObject Uri(path: "old").
      0xff,
  };
  dart::fcb::BytecodeFunction function;
  function.function_id = "package:app/closure.dart::oldUri()";
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(dart::fcb::Value::String("old"));
  function.constants.push_back(
      dart::fcb::Value::String("dart:core::class:Uri.;named:path"));
  module.functions.push_back(std::move(function));
  return module;
}

}  // namespace

namespace dart {

ClosurePtr MakeUriReplaceClosure(Thread* thread) {
  fcb::PatchRuntime uri_runtime;
  std::string error;
  EXPECT(uri_runtime.LoadModule(UriObjectModule(), &error));
  fcb::InterpretResult uri_result =
      uri_runtime.Interpret("package:app/closure.dart::oldUri()", {});
  EXPECT(uri_result.ok);

  Zone* zone = thread->zone();
  const Object& uri_object = Object::Handle(zone, uri_result.value.ToDart());
  EXPECT(uri_object.IsInstance());
  const Instance& uri_instance = Instance::Cast(uri_object);
  const Class& uri_class = Class::Handle(zone, uri_instance.clazz());
  const String& function_name =
      String::Handle(zone, Symbols::New(thread, "replace"));
  const Array& argument_names = Array::Handle(zone, Array::New(1));
  const String& path_name = String::Handle(zone, Symbols::New(thread, "path"));
  argument_names.SetAt(0, path_name, thread);
  const Array& descriptor = Array::Handle(
      zone, ArgumentsDescriptor::NewBoxed(
                /*type_args_len=*/0, /*num_arguments=*/2, argument_names));
  const ArgumentsDescriptor args_desc(descriptor);
  const Function& target =
      Function::Handle(zone, Resolver::ResolveDynamicForReceiverClass(
                                 uri_class, function_name, args_desc,
                                 /*allow_add=*/false));
  EXPECT(!target.IsNull());
  const Function& closure_function =
      Function::Handle(zone, target.ImplicitClosureFunction());
  const Closure& closure = Closure::Handle(
      zone, closure_function.ImplicitInstanceClosure(uri_instance));
  return closure.ptr();
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallClosureNamed) {
  Zone* zone = thread->zone();
  const Closure& closure = Closure::Handle(zone, MakeUriReplaceClosure(thread));
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallClosureNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::callNamed(Function)",
                        {fcb::Value::FromDart(closure.ptr())});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeCallClosureNamed failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("new", result.value.string_value.c_str());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallClosureRejectsBadMetadata) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallClosureBadMetadataModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::badMetadata()", {});
  EXPECT(!result.ok);
  EXPECT(result.error.find("CallClosure metadata must start with ;named:") !=
         std::string::npos);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeCallClosureRejectsTooManyNamed) {
  Zone* zone = thread->zone();
  const Closure& closure = Closure::Handle(zone, MakeUriReplaceClosure(thread));
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallClosureTooManyNamedModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::tooManyNamed(Function)",
                        {fcb::Value::FromDart(closure.ptr())});
  EXPECT(!result.ok);
  EXPECT(result.error.find("too many named closure arguments") !=
         std::string::npos);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeMakeClosureRejectsMissingTarget) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(MakeClosureMissingTargetModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::missingTarget()", {});
  EXPECT(!result.ok);
  EXPECT(result.error.find("MakeClosure failed: function not found") !=
         std::string::npos);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeTryCatchesMakeClosureMissingTarget) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(
      runtime.LoadModule(TryCatchesMakeClosureMissingTargetModule(), &error));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/closure.dart::catchMissingClosureTarget()", {});
  if (!result.ok) {
    OS::PrintErr(
        "FcbPatchRuntimeTryCatchesMakeClosureMissingTarget "
        "failed: %s\n",
        result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_SUBSTRING("MakeClosure failed: function not found",
                   result.value.string_value.c_str());
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchRuntimeTryCatchesCallClosureException) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CallClosureTryCatchesVmExceptionModule(), &error));
  fcb::InterpretResult result = runtime.Interpret(
      "package:app/closure.dart::catchClosureException()", {});
  if (!result.ok) {
    OS::PrintErr("FcbPatchRuntimeTryCatchesCallClosureException failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  const Object& exception =
      Object::Handle(thread->zone(), result.value.ToDart());
  EXPECT(exception.IsError());
  EXPECT_SUBSTRING("FormatException", Error::Cast(exception).ToErrorCString());
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(
    FcbPatchRuntimeMaterializesBytecodeClosureForOriginalCall) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(isolate_group != nullptr);
  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  EXPECT(runtime->LoadModule(CallOriginalWithBytecodeClosureArgumentModule(),
                             &error));
  fcb::InterpretResult result = runtime->Interpret(
      "package:app/closure.dart::passBytecodeClosureToOriginal()", {});

  if (!result.ok) {
    OS::PrintErr(
        "FcbPatchRuntimeMaterializesBytecodeClosureForOriginalCall failed: "
        "%s\n",
        result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBool);
  EXPECT(!result.value.bool_value);
  isolate_group->ClearFcbPatchRuntime();
}

ISOLATE_UNIT_TEST_CASE(
    FcbPatchRuntimeBytecodeClosureContainersMaterializeClosures) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(isolate_group != nullptr);
  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  EXPECT(runtime->LoadModule(CallOriginalWithBytecodeClosureArgumentModule(),
                             &error));

  fcb::Value closure = fcb::Value::BytecodeClosure(
      "package:app/closure.dart::internalClosure()",
      {fcb::Value::String("captured")});
  const Object& closure_object =
      Object::Handle(thread->zone(), closure.ToDart());
  EXPECT(closure_object.IsClosure());

  fcb::Value list = fcb::Value::List({fcb::Value::BytecodeClosure(
      "package:app/closure.dart::internalClosure()",
      {fcb::Value::String("captured")})});
  const Object& list_object = Object::Handle(thread->zone(), list.ToDart());
  EXPECT(list_object.IsArray());
  const Array& array = Array::Cast(list_object);
  const Object& list_closure = Object::Handle(thread->zone(), array.At(0));
  EXPECT(list_closure.IsClosure());

  fcb::Value map = fcb::Value::Map({
      fcb::Value::String("closure"),
      fcb::Value::BytecodeClosure(
          "package:app/closure.dart::internalClosure()",
          {fcb::Value::String("captured")}),
  });
  const Object& map_object = Object::Handle(thread->zone(), map.ToDart());
  EXPECT(map_object.IsMap());
  isolate_group->ClearFcbPatchRuntime();
}

}  // namespace dart
#endif
