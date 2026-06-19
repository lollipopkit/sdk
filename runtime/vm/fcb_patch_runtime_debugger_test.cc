// Copyright (c) 2026, the FCB project authors.

#include "vm/unit_test.h"

#if !defined(PRODUCT)
#include <cstring>

#include "vm/dart_entry.h"
#include "vm/debugger.h"
#include "vm/fcb_patch_runtime_internal.h"
#include "vm/fcb_patch_runtime.h"
#include "vm/json_stream.h"
#include "vm/object.h"
#include "vm/thread.h"

namespace dart {
namespace {

bool saw_active_interpreter_frame = false;
bool saw_captured_closure_active_frame = false;
bool saw_materialized_closure_active_frame = false;
bool saw_bytecode_closure_variable_frame = false;
bool saw_active_handler_frame = false;

static void ObserveActivePatchFrame(
    const fcb::internal::PatchStackTraceFrameInfo& frame_info) {
  if (frame_info.bytecode_offset != 4) {
    return;
  }
  EXPECT_STREQ("package:app/live.dart::activeFrame()", frame_info.function_id);
  EXPECT_STREQ("package:app/live.dart:20:3", frame_info.source_location);
  EXPECT(frame_info.argument_count == 1);
  EXPECT(frame_info.local_count == 1);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 1);

  Zone* zone = Thread::Current()->zone();
  DebuggerStackTrace* stack_trace = DebuggerStackTrace::Collect();
  EXPECT(stack_trace->Length() > 0);
  ActivationFrame* frame = stack_trace->FrameAt(stack_trace->Length() - 1);
  EXPECT(frame->IsFcbPatchFrame());
  EXPECT_STREQ("package:app/live.dart::activeFrame()",
               String::Handle(zone, frame->QualifiedFunctionName())
                   .ToCString());
  EXPECT_STREQ("package:app/live.dart:20:3",
               frame->fcb_patch_source_location().ToCString());
  EXPECT(frame->fcb_patch_bytecode_offset() == 4);
  EXPECT(frame->fcb_patch_argument_count() == 1);
  EXPECT(frame->NumLocalVariables() == 2);

  String& name = String::Handle(zone);
  Object& value = Object::Handle(zone);
  TokenPosition ignored = TokenPosition::kNoSource;
  frame->VariableAt(0, &name, &ignored, &ignored, &ignored, &value);
  EXPECT_STREQ("input", name.ToCString());
  EXPECT_EQ(41, Integer::Cast(value).Value());
  frame->VariableAt(1, &name, &ignored, &ignored, &ignored, &value);
  EXPECT_STREQ("savedInput", name.ToCString());
  EXPECT_EQ(41, Integer::Cast(value).Value());

  const GrowableObjectArray& param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& param_values =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_bounds =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_defaults =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, frame->BuildParameters(
                                      param_names, param_values,
                                      type_param_names, type_param_bounds,
                                      type_param_defaults));
  EXPECT(type_arguments.IsNull());
  EXPECT(param_names.Length() == 2);
  EXPECT(param_values.Length() == 2);
  name ^= param_names.At(0);
  EXPECT_STREQ("input", name.ToCString());
  EXPECT_EQ(41, Integer::Cast(Object::Handle(zone, param_values.At(0))).Value());
  name ^= param_names.At(1);
  EXPECT_STREQ("savedInput", name.ToCString());
  EXPECT_EQ(41, Integer::Cast(Object::Handle(zone, param_values.At(1))).Value());

  JSONStream js;
  {
    JSONObject jsobj(&js);
    frame->PrintToJSONObject(&jsobj);
  }
  const char* json = js.ToCString();
  EXPECT_SUBSTRING("\"fcbPatchArgumentCount\":1", json);
  EXPECT_SUBSTRING("\"fcbPatchScope\"", json);
  EXPECT_SUBSTRING("\"type\":\"FcbPatchScope\"", json);
  EXPECT_SUBSTRING("\"argumentCount\":1", json);
  EXPECT_SUBSTRING("\"capturedSlotCount\":0", json);
  EXPECT_SUBSTRING("\"name\":\"arguments\"", json);
  EXPECT_SUBSTRING("\"start\":0", json);
  EXPECT_SUBSTRING("\"count\":1", json);
  EXPECT_SUBSTRING("\"name\":\"locals\"", json);
  EXPECT_SUBSTRING("\"variables\":[\"input\"]", json);
  EXPECT_SUBSTRING("\"variables\":[\"savedInput\"]", json);
  EXPECT_SUBSTRING("\"fcbPatchVars\"", json);
  EXPECT_SUBSTRING("\"type\":\"FcbPatchBoundVariable\"", json);
  EXPECT_SUBSTRING("\"name\":\"input\"", json);
  EXPECT_SUBSTRING("\"scope\":\"arguments\"", json);
  EXPECT_SUBSTRING("\"valueMaterialized\":true", json);
  EXPECT_SUBSTRING("\"valueKind\":\"int\"", json);
  EXPECT_SUBSTRING("\"valuePreview\":\"41\"", json);
  EXPECT_SUBSTRING("\"name\":\"savedInput\"", json);
  EXPECT_SUBSTRING("\"scope\":\"locals\"", json);
  saw_active_interpreter_frame = true;
}

static fcb::BytecodeModule ActiveFrameCallbackModule() {
  fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,  // LoadArg 0.
      0x04, 0x00,  // StoreLocal 0.
      0x03, 0x00,  // LoadLocal 0.
      0xff,
  };
  fcb::BytecodeFunction function;
  function.function_id = "package:app/live.dart::activeFrame()";
  function.parameter_count = 1;
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.source_map.push_back(
    fcb::SourceMapEntry{0, "package:app/live.dart:19:3"});
  function.source_map.push_back(
      fcb::SourceMapEntry{4, "package:app/live.dart:20:3"});
  function.debug_locals.push_back(fcb::DebugLocalEntry{0, "input"});
  function.debug_locals.push_back(fcb::DebugLocalEntry{1, "savedInput"});
  module.functions.push_back(std::move(function));
  return module;
}

static void ObserveActiveHandlerPatchFrame(
    const fcb::internal::PatchStackTraceFrameInfo& frame_info) {
  if (std::strcmp(frame_info.function_id,
                  "package:app/try.dart::debugHandlerFrame()") != 0 ||
      frame_info.bytecode_offset != 5) {
    return;
  }
  EXPECT(frame_info.active_handler_count == 1);
  EXPECT(frame_info.innermost_handler_offset == 11);
  EXPECT(frame_info.innermost_handler_end_offset == 16);

  Zone* zone = Thread::Current()->zone();
  DebuggerStackTrace* stack_trace = DebuggerStackTrace::Collect();
  EXPECT(stack_trace->Length() > 0);
  ActivationFrame* frame = stack_trace->FrameAt(stack_trace->Length() - 1);
  EXPECT(frame->IsFcbPatchFrame());
  EXPECT(frame->fcb_patch_active_handler_count() == 1);
  EXPECT(frame->fcb_patch_innermost_handler_offset() == 11);
  EXPECT(frame->fcb_patch_innermost_handler_end_offset() == 16);
  const String& boom = String::Handle(zone, String::New("boom"));
  const Instance& exception = Instance::Handle(zone, boom.ptr());
  EXPECT(frame->HandlesException(exception));
  EXPECT(stack_trace->GetHandlerFrame(exception) == frame);

  JSONStream js;
  {
    JSONObject jsobj(&js);
    frame->PrintToJSONObject(&jsobj);
  }
  const char* json = js.ToCString();
  EXPECT_SUBSTRING("\"fcbPatchActiveHandlerCount\":1", json);
  EXPECT_SUBSTRING("\"fcbPatchInnermostHandlerOffset\":11", json);
  EXPECT_SUBSTRING("\"fcbPatchInnermostHandlerEndOffset\":16", json);
  saw_active_handler_frame = true;
}

static fcb::BytecodeModule ActiveHandlerFrameModule() {
  fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x61, 0x00, 0x0b, 0x00, 0x10,  // TryBegin handler 11, end 16.
      0x01, 0x00, 0x00,              // LoadConst "ok".
      0x30, 0x00, 0x10,              // Jump 16.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x01, 0x00, 0x01,              // LoadConst "caught".
      0xff,                          // Return.
  };
  fcb::BytecodeFunction function;
  function.function_id = "package:app/try.dart::debugHandlerFrame()";
  function.register_count = 1;
  function.bytecode_length = static_cast<uint32_t>(module.bytecode.size());
  function.constants.push_back(fcb::Value::String("ok"));
  function.constants.push_back(fcb::Value::String("caught"));
  function.source_map.push_back(
      fcb::SourceMapEntry{5, "package:app/try.dart:12:5"});
  module.functions.push_back(std::move(function));
  return module;
}

static void ObserveBytecodeClosureVariablePatchFrame(
    const fcb::internal::PatchStackTraceFrameInfo& frame_info) {
  if (std::strcmp(frame_info.function_id,
                  "package:app/closure.dart::storeClosure()") != 0 ||
      frame_info.bytecode_offset != 5) {
    return;
  }
  EXPECT(frame_info.argument_count == 0);
  EXPECT(frame_info.local_count == 1);

  Zone* zone = Thread::Current()->zone();
  DebuggerStackTrace* stack_trace = DebuggerStackTrace::Collect();
  EXPECT(stack_trace->Length() > 0);
  ActivationFrame* frame = stack_trace->FrameAt(stack_trace->Length() - 1);
  EXPECT(frame->IsFcbPatchFrame());
  EXPECT(frame->NumLocalVariables() == 1);

  String& name = String::Handle(zone);
  Object& value = Object::Handle(zone);
  TokenPosition ignored = TokenPosition::kNoSource;
  frame->VariableAt(0, &name, &ignored, &ignored, &ignored, &value);
  EXPECT_STREQ("callback", name.ToCString());
  EXPECT(value.IsNull());

  JSONStream js;
  {
    JSONObject jsobj(&js);
    frame->PrintToJSONObject(&jsobj);
  }
  const char* json = js.ToCString();
  EXPECT_SUBSTRING("\"fcbPatchVars\"", json);
  EXPECT_SUBSTRING("\"name\":\"callback\"", json);
  EXPECT_SUBSTRING("\"scope\":\"locals\"", json);
  EXPECT_SUBSTRING("\"valueMaterialized\":false", json);
  EXPECT_SUBSTRING("\"valueKind\":\"bytecode_closure\"", json);
  EXPECT_SUBSTRING(
      "\"valuePreview\":\"BytecodeClosure(function=package:app\\/closure.dart::"
      "storeClosure.<closure>(),captures=0)\"",
      json);

  const GrowableObjectArray& param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& param_values =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_bounds =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_defaults =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, frame->BuildParameters(
                                      param_names, param_values,
                                      type_param_names, type_param_bounds,
                                      type_param_defaults));
  EXPECT(type_arguments.IsNull());
  EXPECT(param_names.Length() == 0);
  EXPECT(param_values.Length() == 0);
  saw_bytecode_closure_variable_frame = true;
}

static fcb::BytecodeModule BytecodeClosureVariableFrameModule() {
  fcb::BytecodeModule module;
  module.version = 1;

  const uint32_t closure_offset = 0;
  module.bytecode.insert(module.bytecode.end(), {
      0x01, 0x00, 0x00,  // LoadConst "ok".
      0xff,              // Return.
  });

  const uint32_t store_offset = static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x54, 0x00, 0x00,  // MakeClosure closure;captures:0.
      0x04, 0x00,        // StoreLocal 0.
      0x03, 0x00,        // LoadLocal 0.
      0xff,              // Return.
  });

  fcb::BytecodeFunction closure;
  closure.function_id = "package:app/closure.dart::storeClosure.<closure>()";
  closure.bytecode_offset = closure_offset;
  closure.bytecode_length = store_offset - closure_offset;
  closure.constants.push_back(fcb::Value::String("ok"));
  closure.source_map.push_back(
      fcb::SourceMapEntry{0, "package:app/closure.dart:70:5"});
  module.functions.push_back(std::move(closure));

  fcb::BytecodeFunction store;
  store.function_id = "package:app/closure.dart::storeClosure()";
  store.register_count = 1;
  store.bytecode_offset = store_offset;
  store.bytecode_length =
      static_cast<uint32_t>(module.bytecode.size()) - store_offset;
  store.constants.push_back(fcb::Value::String(
      "package:app/closure.dart::storeClosure.<closure>();captures:0"));
  store.source_map.push_back(
      fcb::SourceMapEntry{0, "package:app/closure.dart:66:3"});
  store.source_map.push_back(
      fcb::SourceMapEntry{5, "package:app/closure.dart:67:3"});
  store.debug_locals.push_back(fcb::DebugLocalEntry{0, "callback"});
  module.functions.push_back(std::move(store));

  return module;
}

static void ExpectStringLocal(Zone* zone,
                              ActivationFrame* frame,
                              intptr_t index,
                              const char* expected_name,
                              const char* expected_value) {
  String& name = String::Handle(zone);
  Object& value = Object::Handle(zone);
  TokenPosition ignored = TokenPosition::kNoSource;
  frame->VariableAt(index, &name, &ignored, &ignored, &ignored, &value);
  EXPECT_STREQ(expected_name, name.ToCString());
  EXPECT(value.IsString());
  EXPECT_STREQ(expected_value, String::Cast(value).ToCString());
}

static void ObserveCapturedClosureActivePatchFrame(
    const fcb::internal::PatchStackTraceFrameInfo& frame_info) {
  if (std::strcmp(frame_info.function_id,
                  "package:app/closure.dart::makeGreeting.<closure>()") != 0 ||
      frame_info.bytecode_offset != 6) {
    return;
  }
  EXPECT_STREQ("package:app/closure.dart::makeGreeting.<closure>()",
               frame_info.function_id);
  EXPECT_STREQ("package:app/closure.dart:44:5", frame_info.source_location);
  EXPECT(frame_info.argument_count == 3);
  EXPECT(frame_info.captured_argument_count == 2);
  EXPECT(frame_info.local_count == 3);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 2);

  Zone* zone = Thread::Current()->zone();
  DebuggerStackTrace* stack_trace = DebuggerStackTrace::Collect();
  EXPECT(stack_trace->Length() >= 2);
  ActivationFrame* closure_frame =
      stack_trace->FrameAt(stack_trace->Length() - 1);
  EXPECT(closure_frame->IsFcbPatchFrame());
  EXPECT_STREQ("package:app/closure.dart::makeGreeting.<closure>()",
               String::Handle(zone, closure_frame->QualifiedFunctionName())
                   .ToCString());
  EXPECT_STREQ("package:app/closure.dart:44:5",
               closure_frame->fcb_patch_source_location().ToCString());
  EXPECT(closure_frame->fcb_patch_bytecode_offset() == 6);
  EXPECT(closure_frame->fcb_patch_argument_count() == 3);
  EXPECT(closure_frame->fcb_patch_captured_slot_count() == 2);
  EXPECT(closure_frame->NumLocalVariables() == 6);
  ExpectStringLocal(zone, closure_frame, 0, "prefix", "patched ");
  ExpectStringLocal(zone, closure_frame, 1, "name", "Ada");
  ExpectStringLocal(zone, closure_frame, 2, "suffix", " friend");

  const GrowableObjectArray& param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& param_values =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_bounds =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_defaults =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, closure_frame->BuildParameters(
                                      param_names, param_values,
                                      type_param_names, type_param_bounds,
                                      type_param_defaults));
  EXPECT(type_arguments.IsNull());
  EXPECT(param_names.Length() == 6);
  EXPECT(param_values.Length() == 6);
  String& name = String::Handle(zone);
  name ^= param_names.At(0);
  EXPECT_STREQ("prefix", name.ToCString());
  EXPECT_STREQ("patched ",
               String::Cast(Object::Handle(zone, param_values.At(0)))
                   .ToCString());
  name ^= param_names.At(1);
  EXPECT_STREQ("name", name.ToCString());
  EXPECT_STREQ("Ada",
               String::Cast(Object::Handle(zone, param_values.At(1)))
                   .ToCString());
  name ^= param_names.At(2);
  EXPECT_STREQ("suffix", name.ToCString());
  EXPECT_STREQ(" friend",
               String::Cast(Object::Handle(zone, param_values.At(2)))
                   .ToCString());

  ActivationFrame* caller_frame =
      stack_trace->FrameAt(stack_trace->Length() - 2);
  EXPECT(caller_frame->IsFcbPatchFrame());
  EXPECT_STREQ("package:app/closure.dart::callGreeting()",
               String::Handle(zone, caller_frame->QualifiedFunctionName())
                   .ToCString());

  JSONStream js;
  {
    JSONObject jsobj(&js);
    closure_frame->PrintToJSONObject(&jsobj);
  }
  const char* json = js.ToCString();
  EXPECT_SUBSTRING("\"fcbPatchArgumentCount\":3", json);
  EXPECT_SUBSTRING("\"fcbPatchCapturedSlotCount\":2", json);
  EXPECT_SUBSTRING("\"fcbPatchScope\"", json);
  EXPECT_SUBSTRING("\"type\":\"FcbPatchScope\"", json);
  EXPECT_SUBSTRING("\"variableCount\":6", json);
  EXPECT_SUBSTRING("\"argumentCount\":3", json);
  EXPECT_SUBSTRING("\"capturedSlotCount\":2", json);
  EXPECT_SUBSTRING("\"name\":\"captured\"", json);
  EXPECT_SUBSTRING("\"start\":0", json);
  EXPECT_SUBSTRING("\"count\":2", json);
  EXPECT_SUBSTRING("\"variables\":[\"prefix\",\"name\"]", json);
  EXPECT_SUBSTRING("\"name\":\"arguments\"", json);
  EXPECT_SUBSTRING("\"start\":2", json);
  EXPECT_SUBSTRING("\"count\":1", json);
  EXPECT_SUBSTRING("\"variables\":[\"suffix\"]", json);
  EXPECT_SUBSTRING("\"name\":\"locals\"", json);
  EXPECT_SUBSTRING("\"start\":3", json);
  EXPECT_SUBSTRING("\"count\":3", json);
  EXPECT_SUBSTRING("\"fcbPatchVars\"", json);
  EXPECT_SUBSTRING("\"name\":\"prefix\"", json);
  EXPECT_SUBSTRING("\"scope\":\"captured\"", json);
  EXPECT_SUBSTRING("\"valueMaterialized\":true", json);
  EXPECT_SUBSTRING("\"valueKind\":\"string\"", json);
  EXPECT_SUBSTRING("\"valuePreview\":\"patched \"", json);
  EXPECT_SUBSTRING("\"name\":\"suffix\"", json);
  EXPECT_SUBSTRING("\"scope\":\"arguments\"", json);
  EXPECT_SUBSTRING("\"valuePreview\":\" friend\"", json);
  saw_captured_closure_active_frame = true;
}

static void ObserveMaterializedClosureActivePatchFrame(
    const fcb::internal::PatchStackTraceFrameInfo& frame_info) {
  if (std::strcmp(frame_info.function_id,
                  "package:app/closure.dart::makeGreeting.<closure>()") != 0 ||
      frame_info.bytecode_offset != 6) {
    return;
  }
  EXPECT_STREQ("package:app/closure.dart:44:5", frame_info.source_location);
  EXPECT(frame_info.argument_count == 3);
  EXPECT(frame_info.captured_argument_count == 2);
  EXPECT(frame_info.local_count == 3);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 1);

  Zone* zone = Thread::Current()->zone();
  DebuggerStackTrace* stack_trace = DebuggerStackTrace::Collect();
  EXPECT(stack_trace->Length() >= 1);
  ActivationFrame* closure_frame =
      stack_trace->FrameAt(stack_trace->Length() - 1);
  EXPECT(closure_frame->IsFcbPatchFrame());
  EXPECT_STREQ("package:app/closure.dart::makeGreeting.<closure>()",
               String::Handle(zone, closure_frame->QualifiedFunctionName())
                   .ToCString());
  EXPECT_STREQ("package:app/closure.dart:44:5",
               closure_frame->fcb_patch_source_location().ToCString());
  EXPECT(closure_frame->fcb_patch_argument_count() == 3);
  EXPECT(closure_frame->fcb_patch_captured_slot_count() == 2);
  EXPECT(closure_frame->NumLocalVariables() == 6);
  ExpectStringLocal(zone, closure_frame, 0, "prefix", "patched ");
  ExpectStringLocal(zone, closure_frame, 1, "name", "Ada");
  ExpectStringLocal(zone, closure_frame, 2, "suffix", " friend");

  const GrowableObjectArray& param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& param_values =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_bounds =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_defaults =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, closure_frame->BuildParameters(
                                      param_names, param_values,
                                      type_param_names, type_param_bounds,
                                      type_param_defaults));
  EXPECT(type_arguments.IsNull());
  EXPECT(param_names.Length() == 6);
  EXPECT(param_values.Length() == 6);
  String& name = String::Handle(zone);
  name ^= param_names.At(0);
  EXPECT_STREQ("prefix", name.ToCString());
  name ^= param_names.At(1);
  EXPECT_STREQ("name", name.ToCString());
  name ^= param_names.At(2);
  EXPECT_STREQ("suffix", name.ToCString());

  JSONStream js;
  {
    JSONObject jsobj(&js);
    closure_frame->PrintToJSONObject(&jsobj);
  }
  const char* json = js.ToCString();
  EXPECT_SUBSTRING("\"fcbPatchScope\"", json);
  EXPECT_SUBSTRING("\"variables\":[\"prefix\",\"name\"]", json);
  EXPECT_SUBSTRING("\"variables\":[\"suffix\"]", json);
  EXPECT_SUBSTRING("\"scope\":\"captured\"", json);
  EXPECT_SUBSTRING("\"scope\":\"arguments\"", json);
  saw_materialized_closure_active_frame = true;
}

static fcb::BytecodeModule CapturedClosureActiveFrameModule() {
  fcb::BytecodeModule module;
  module.version = 1;

  const uint32_t closure_offset = 0;
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: captured name.
      0x02, 0x02,        // LoadArg 2: closure suffix argument.
      0x42, 0x00, 0x03,  // StringConcat 3.
      0xff,              // Return.
  });

  const uint32_t make_offset = static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x01, 0x00, 0x00,  // LoadConst "patched ".
      0x02, 0x00,        // LoadArg 0: name.
      0x54, 0x00, 0x01,  // MakeClosure closure;captures:2.
      0xff,              // Return.
  });

  const uint32_t call_offset = static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makeGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst " friend".
      0x53, 0x00, 0x00, 0x01,  // CallClosure argc 1.
      0xff,                    // Return.
  });

  fcb::BytecodeFunction closure;
  closure.function_id =
      "package:app/closure.dart::makeGreeting.<closure>()";
  closure.parameter_count = 3;
  closure.register_count = 3;
  closure.bytecode_offset = closure_offset;
  closure.bytecode_length = make_offset - closure_offset;
  closure.source_map.push_back(
      fcb::SourceMapEntry{0, "package:app/closure.dart:43:5"});
  closure.source_map.push_back(
      fcb::SourceMapEntry{6, "package:app/closure.dart:44:5"});
  closure.debug_locals.push_back(fcb::DebugLocalEntry{0, "prefix"});
  closure.debug_locals.push_back(fcb::DebugLocalEntry{1, "name"});
  closure.debug_locals.push_back(fcb::DebugLocalEntry{2, "suffix"});
  module.functions.push_back(std::move(closure));

  fcb::BytecodeFunction make;
  make.function_id = "package:app/closure.dart::makeGreeting()";
  make.parameter_count = 1;
  make.register_count = 1;
  make.bytecode_offset = make_offset;
  make.bytecode_length = call_offset - make_offset;
  make.constants.push_back(fcb::Value::String("patched "));
  make.constants.push_back(fcb::Value::String(
      "package:app/closure.dart::makeGreeting.<closure>();captures:2"));
  make.source_map.push_back(
      fcb::SourceMapEntry{0, "package:app/closure.dart:38:3"});
  make.debug_locals.push_back(fcb::DebugLocalEntry{0, "name"});
  module.functions.push_back(std::move(make));

  fcb::BytecodeFunction call;
  call.function_id = "package:app/closure.dart::callGreeting()";
  call.parameter_count = 1;
  call.register_count = 1;
  call.bytecode_offset = call_offset;
  call.bytecode_length =
      static_cast<uint32_t>(module.bytecode.size()) - call_offset;
  call.constants.push_back(
      fcb::Value::String("package:app/closure.dart::makeGreeting()"));
  call.constants.push_back(fcb::Value::String(" friend"));
  call.source_map.push_back(
      fcb::SourceMapEntry{0, "package:app/closure.dart:51:3"});
  call.debug_locals.push_back(fcb::DebugLocalEntry{0, "name"});
  module.functions.push_back(std::move(call));

  return module;
}

}  // namespace

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerStackTraceFromStringFrame) {
  Zone* zone = thread->zone();
  const Array& code_array = Array::Handle(zone, Array::New(1));
  const String& location =
      String::Handle(zone, String::New("package:app/bad.dart:9:3"));
  code_array.SetAt(0, location, thread);

  const TypedData& pc_offset_array =
      TypedData::Handle(zone, TypedData::New(kUintPtrCid, 1));
  pc_offset_array.SetUintPtr(0, 0);

  const StackTrace& stack_trace =
      StackTrace::Handle(zone, StackTrace::New(code_array, pc_offset_array));
  DebuggerStackTrace* debugger_stack_trace =
      DebuggerStackTrace::From(stack_trace);
  EXPECT(debugger_stack_trace->Length() == 1);

  ActivationFrame* frame = debugger_stack_trace->FrameAt(0);
  EXPECT(frame->IsFcbPatchFrame());
  EXPECT_STREQ("package:app/bad.dart:9:3",
               frame->fcb_patch_source_location().ToCString());
  EXPECT_STREQ("<fcb patch>",
               String::Handle(zone, frame->QualifiedFunctionName()).ToCString());
  EXPECT_STREQ("package:app/bad.dart",
               String::Handle(zone, frame->SourceUrl()).ToCString());
  EXPECT(frame->LineNumber() == 9);
  EXPECT(frame->ColumnNumber() == 3);
  EXPECT(frame->NumLocalVariables() == 0);
  EXPECT(frame->IsDebuggable());
  EXPECT(!frame->IsRewindable());
  EXPECT(Object::Handle(zone, frame->GetReceiver()).IsNull());
  const String& boom = String::Handle(zone, String::New("boom"));
  const Instance& exception = Instance::Handle(zone, boom.ptr());
  EXPECT(!frame->HandlesException(exception));
  EXPECT(debugger_stack_trace->GetHandlerFrame(exception) == nullptr);

  JSONStream js;
  {
    JSONObject jsobj(&js);
    frame->PrintToJSONObject(&jsobj);
  }
  const char* json = js.ToCString();
  EXPECT_SUBSTRING("\"kind\":\"FcbPatch\"", json);
  EXPECT_SUBSTRING("\"type\":\"FcbPatchSourceLocation\"", json);
  EXPECT_SUBSTRING("\"sourceUri\":\"package:app\\/bad.dart\"", json);
  EXPECT_SUBSTRING("\"line\":9", json);
  EXPECT_SUBSTRING("\"column\":3", json);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerCollectsLivePatchFrame) {
  Zone* zone = thread->zone();
  fcb::BytecodeFunction function;
  function.function_id = "package:app/live.dart::broken()";
  function.source_map.push_back(
      fcb::SourceMapEntry{0, "package:app/live.dart:12:5"});

  fcb::internal::ClearPatchStackTraceLocation();
  fcb::internal::RecordPatchStackTraceLocation(function, 7);
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 1);
  const fcb::internal::PatchStackTraceFrameInfo frame_info =
      fcb::internal::PatchStackTraceFrameInfoAt(0);
  EXPECT_STREQ("package:app/live.dart::broken()", frame_info.function_id);
  EXPECT_STREQ("package:app/live.dart:12:5", frame_info.source_location);
  EXPECT(frame_info.bytecode_offset == 7);

  DebuggerStackTrace* stack_trace = DebuggerStackTrace::Collect();
  EXPECT(stack_trace->Length() > 0);
  ActivationFrame* frame = stack_trace->FrameAt(stack_trace->Length() - 1);
  EXPECT(frame->IsFcbPatchFrame());
  EXPECT_STREQ("package:app/live.dart:12:5",
               frame->fcb_patch_source_location().ToCString());
  EXPECT_STREQ("package:app/live.dart::broken()",
               String::Handle(zone, frame->QualifiedFunctionName()).ToCString());
  EXPECT_STREQ("package:app/live.dart::broken()",
               frame->fcb_patch_function_id().ToCString());
  EXPECT(frame->fcb_patch_bytecode_offset() == 7);
  EXPECT_STREQ("package:app/live.dart",
               String::Handle(zone, frame->SourceUrl()).ToCString());
  EXPECT(frame->LineNumber() == 12);
  EXPECT(frame->ColumnNumber() == 5);

  JSONStream js;
  {
    JSONObject jsobj(&js);
    frame->PrintToJSONObject(&jsobj);
  }
  const char* json = js.ToCString();
  EXPECT_SUBSTRING("\"function\":\"package:app\\/live.dart::broken()\"", json);
  EXPECT_SUBSTRING(
      "\"fcbPatchFunctionId\":\"package:app\\/live.dart::broken()\"", json);
  EXPECT_SUBSTRING("\"fcbPatchBytecodeOffset\":7", json);

  fcb::internal::ClearPatchStackTraceLocation();
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerFrameEvaluationUsesSourceLibrary) {
  Zone* zone = thread->zone();
  const String& location =
      String::Handle(zone, String::New("dart:core:21:7"));
  ActivationFrame frame(location);

  const GrowableObjectArray& param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& param_values =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_names =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_bounds =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const GrowableObjectArray& type_param_defaults =
      GrowableObjectArray::Handle(zone, GrowableObjectArray::New());
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, frame.BuildParameters(
                                      param_names, param_values,
                                      type_param_names, type_param_bounds,
                                      type_param_defaults));
  EXPECT(type_arguments.IsNull());
  EXPECT(param_names.Length() == 0);
  EXPECT(param_values.Length() == 0);
  EXPECT(type_param_names.Length() == 0);
  EXPECT(type_param_bounds.Length() == 0);
  EXPECT(type_param_defaults.Length() == 0);

  uint8_t malformed_kernel[] = {0};
  const ExternalTypedData& kernel_data = ExternalTypedData::Handle(
      zone, ExternalTypedData::New(kExternalTypedDataUint8ArrayCid,
                                   malformed_kernel,
                                   sizeof(malformed_kernel)));
  const Object& result =
      Object::Handle(zone, frame.EvaluateCompiledExpression(
                               kernel_data, Array::empty_array(),
                               Array::empty_array(),
                               TypeArguments::Handle(TypeArguments::null())));
  EXPECT(result.IsApiError());
  EXPECT_SUBSTRING("Kernel isolate returned ill-formed kernel",
                   ApiError::Cast(result).ToErrorCString());

  const String& missing_location =
      String::Handle(zone, String::New("package:app/eval.dart:21:7"));
  ActivationFrame missing_frame(missing_location);
  const Object& missing_result =
      Object::Handle(zone, missing_frame.EvaluateCompiledExpression(
                               kernel_data, Array::empty_array(),
                               Array::empty_array(),
                               TypeArguments::Handle(TypeArguments::null())));
  EXPECT(missing_result.IsApiError());
  EXPECT_SUBSTRING(
      "Expression evaluation is not available for FCB patch frame source "
      "'package:app/eval.dart': library not found",
      ApiError::Cast(missing_result).ToErrorCString());
}

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerCollectsActiveInterpreterFrame) {
  saw_active_interpreter_frame = false;
  fcb::internal::SetActivePatchFrameUpdateCallback(ObserveActivePatchFrame);
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(ActiveFrameCallbackModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/live.dart::activeFrame()",
                        {fcb::Value::Int(41)});
  fcb::internal::SetActivePatchFrameUpdateCallback(nullptr);
  if (!result.ok) {
    OS::PrintErr("FcbPatchDebuggerCollectsActiveInterpreterFrame failed: %s\n",
                 result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kInt);
  EXPECT_EQ(41, result.value.int_value);
  EXPECT(saw_active_interpreter_frame);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerExposesActiveHandlerMetadata) {
  saw_active_handler_frame = false;
  fcb::internal::SetActivePatchFrameUpdateCallback(
      ObserveActiveHandlerPatchFrame);
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(ActiveHandlerFrameModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/try.dart::debugHandlerFrame()", {});
  fcb::internal::SetActivePatchFrameUpdateCallback(nullptr);
  if (!result.ok) {
    OS::PrintErr(
        "FcbPatchDebuggerExposesActiveHandlerMetadata failed: %s\n",
        result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("ok", result.value.string_value.c_str());
  EXPECT(saw_active_handler_frame);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerDescribesUnmaterializedBytecodeClosure) {
  saw_bytecode_closure_variable_frame = false;
  fcb::internal::SetActivePatchFrameUpdateCallback(
      ObserveBytecodeClosureVariablePatchFrame);
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(BytecodeClosureVariableFrameModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::storeClosure()", {});
  fcb::internal::SetActivePatchFrameUpdateCallback(nullptr);
  if (!result.ok) {
    OS::PrintErr(
        "FcbPatchDebuggerDescribesUnmaterializedBytecodeClosure failed: %s\n",
        result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kBytecodeClosure);
  EXPECT(saw_bytecode_closure_variable_frame);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerCollectsCapturedClosureActiveFrame) {
  saw_captured_closure_active_frame = false;
  fcb::internal::SetActivePatchFrameUpdateCallback(
      ObserveCapturedClosureActivePatchFrame);
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(CapturedClosureActiveFrameModule(), &error));
  fcb::InterpretResult result =
      runtime.Interpret("package:app/closure.dart::callGreeting()",
                        {fcb::Value::String("Ada")});
  fcb::internal::SetActivePatchFrameUpdateCallback(nullptr);
  if (!result.ok) {
    OS::PrintErr(
        "FcbPatchDebuggerCollectsCapturedClosureActiveFrame failed: %s\n",
        result.error.c_str());
  }
  EXPECT(result.ok);
  EXPECT(result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched Ada friend", result.value.string_value.c_str());
  EXPECT(saw_captured_closure_active_frame);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 0);
}

ISOLATE_UNIT_TEST_CASE(FcbPatchDebuggerCollectsMaterializedClosureActiveFrame) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(thread != nullptr);
  ASSERT(isolate_group != nullptr);
  saw_materialized_closure_active_frame = false;
  fcb::internal::SetActivePatchFrameUpdateCallback(
      ObserveMaterializedClosureActivePatchFrame);

  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  EXPECT(runtime->LoadModule(CapturedClosureActiveFrameModule(), &error));

  fcb::InterpretResult closure_result = runtime->Interpret(
      "package:app/closure.dart::makeGreeting()",
      {fcb::Value::String("Ada")});
  if (!closure_result.ok) {
    OS::PrintErr(
        "FcbPatchDebuggerCollectsMaterializedClosureActiveFrame failed: %s\n",
        closure_result.error.c_str());
  }
  EXPECT(closure_result.ok);
  EXPECT(closure_result.value.kind == fcb::ValueKind::kBytecodeClosure);

  Zone* zone = thread->zone();
  const Object& closure_object =
      Object::Handle(zone, closure_result.value.ToDart());
  EXPECT(closure_object.IsClosure());

  const Array& closure_arguments = Array::Handle(zone, Array::New(2));
  closure_arguments.SetAt(0, closure_object, thread);
  closure_arguments.SetAt(1, String::Handle(zone, String::New(" friend")),
                          thread);
  const Object& call_result =
      Object::Handle(zone, DartEntry::InvokeClosure(thread, closure_arguments));
  fcb::internal::SetActivePatchFrameUpdateCallback(nullptr);
  EXPECT(call_result.IsString());
  EXPECT_STREQ("patched Ada friend", String::Cast(call_result).ToCString());
  EXPECT(saw_materialized_closure_active_frame);
  EXPECT(fcb::internal::ActivePatchFrameCount() == 0);
  isolate_group->ClearFcbPatchRuntime();
}

}  // namespace dart
#endif  // !defined(PRODUCT)
