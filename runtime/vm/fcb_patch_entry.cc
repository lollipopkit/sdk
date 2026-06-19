// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_entry.h"

#include <cstring>
#include <utility>
#include <vector>

#include "vm/dart_entry.h"
#include "vm/flags.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/os.h"
#include "vm/fcb_patch_runtime_internal.h"
#include "vm/symbols.h"
#include "vm/thread.h"

extern "C" {
#if defined(__APPLE__)
int __attribute__((weak)) fcb_current_patch_number() {
  return -1;
}
int __attribute__((weak)) fcb_active_patch_number() {
  return fcb_current_patch_number();
}
int __attribute__((weak)) fcb_report_interpret_failure(int patch_number,
                                                       const char* function_id,
                                                       const char* error) {
  return -1;
}
void __attribute__((weak)) fcb_record_interpreter_call() {}
void __attribute__((weak)) fcb_record_aot_call() {}
#elif defined(__GNUC__)
int fcb_current_patch_number() __attribute__((weak));
int fcb_active_patch_number() __attribute__((weak));
int fcb_report_interpret_failure(int patch_number,
                                 const char* function_id,
                                 const char* error) __attribute__((weak));
void fcb_record_interpreter_call() __attribute__((weak));
void fcb_record_aot_call() __attribute__((weak));
#else
int fcb_current_patch_number();
int fcb_active_patch_number();
int fcb_report_interpret_failure(int patch_number,
                                 const char* function_id,
                                 const char* error);
void fcb_record_interpreter_call();
void fcb_record_aot_call();
#endif
}

namespace dart {

DEFINE_FLAG(bool,
            trace_fcb_dispatch,
            false,
            "Trace FCB AOT dispatch target matching and interpretation.");
DEFINE_FLAG(charp,
            fcb_aot_probe_allowlist,
            nullptr,
            "Optional comma-separated FCB AOT probe allowlist. Entries may be "
            "script URLs, qualified function names, script::function IDs, or "
            "prefix*. When unset, FCB probes non-SDK Dart package functions.");

namespace fcb {

thread_local intptr_t suppress_patch_invocation_depth = 0;

bool IsPatchInvocationSuppressed() {
  return suppress_patch_invocation_depth > 0;
}

ScopedSuppressPatchInvocation::ScopedSuppressPatchInvocation() {
  ++suppress_patch_invocation_depth;
}

ScopedSuppressPatchInvocation::~ScopedSuppressPatchInvocation() {
  ASSERT(suppress_patch_invocation_depth > 0);
  --suppress_patch_invocation_depth;
}

namespace {

constexpr intptr_t kMaxBridgeCollectionDepth = 16;

bool StringEquals(const char* lhs, intptr_t lhs_len, const char* rhs) {
  return rhs != nullptr && static_cast<intptr_t>(std::strlen(rhs)) == lhs_len &&
         std::strncmp(lhs, rhs, lhs_len) == 0;
}

bool StringStartsWith(const char* value, const char* prefix, intptr_t len) {
  return value != nullptr && std::strncmp(value, prefix, len) == 0;
}

std::string StringToStdString(StringPtr value) {
  if (value == String::null()) {
    return {};
  }
  return String::Handle(value).ToCString();
}

std::string QualifiedFunctionId(const Function& function) {
  return StringToStdString(function.QualifiedUserVisibleName());
}

std::string ScriptQualifiedFunctionId(const Function& function) {
  Zone* zone = Thread::Current()->zone();
  const Script& script = Script::Handle(zone, function.script());
  const std::string qualified = QualifiedFunctionId(function);
  if (script.IsNull()) {
    return qualified;
  }
  const String& url = String::Handle(zone, script.url());
  if (url.IsNull()) {
    return qualified;
  }
  return std::string(url.ToCString()) + "::" + qualified;
}

bool MatchesProbeAllowlistEntry(const char* start,
                                intptr_t length,
                                const char* script_url,
                                const std::string& qualified,
                                const std::string& script_qualified) {
  while (length > 0 && (*start == ' ' || *start == '\t' || *start == '\n')) {
    ++start;
    --length;
  }
  while (length > 0 &&
         (start[length - 1] == ' ' || start[length - 1] == '\t' ||
          start[length - 1] == '\n')) {
    --length;
  }
  if (length == 0) {
    return false;
  }
  if (length == 1 && start[0] == '*') {
    return true;
  }

  const bool is_prefix = start[length - 1] == '*';
  const intptr_t match_length = is_prefix ? length - 1 : length;
  if (match_length <= 0) {
    return false;
  }
  if (is_prefix) {
    return StringStartsWith(script_url, start, match_length) ||
           (static_cast<intptr_t>(qualified.length()) >= match_length &&
            std::strncmp(qualified.c_str(), start, match_length) == 0) ||
           (static_cast<intptr_t>(script_qualified.length()) >= match_length &&
            std::strncmp(script_qualified.c_str(), start, match_length) == 0);
  }
  return StringEquals(start, match_length, script_url) ||
         (static_cast<intptr_t>(qualified.length()) == match_length &&
          std::strncmp(qualified.c_str(), start, match_length) == 0) ||
         (static_cast<intptr_t>(script_qualified.length()) == match_length &&
          std::strncmp(script_qualified.c_str(), start, match_length) == 0);
}

bool MatchesProbeAllowlist(const char* script_url,
                           const std::string& qualified,
                           const std::string& script_qualified) {
  const char* allowlist = FLAG_fcb_aot_probe_allowlist;
  if (allowlist == nullptr || allowlist[0] == '\0') {
    return false;
  }

  const char* entry = allowlist;
  const char* cursor = allowlist;
  while (true) {
    if (*cursor == ',' || *cursor == ';' || *cursor == '\0') {
      if (MatchesProbeAllowlistEntry(entry, cursor - entry, script_url,
                                     qualified, script_qualified)) {
        return true;
      }
      if (*cursor == '\0') {
        return false;
      }
      entry = cursor + 1;
    }
    ++cursor;
  }
}

bool HasProbeAllowlist() {
  const char* allowlist = FLAG_fcb_aot_probe_allowlist;
  return allowlist != nullptr && allowlist[0] != '\0';
}

bool IsSdkOrFlutterScriptUrl(const char* url) {
  if (url == nullptr) {
    return true;
  }
  return std::strncmp(url, "dart:", 5) == 0 ||
         std::strncmp(url, "package:flutter/", 16) == 0 ||
         std::strncmp(url, "package:flutter_test/", 21) == 0 ||
         std::strncmp(url, "package:sky_engine/", 19) == 0;
}

bool IsStaticPatchableFunction(const Function& function) {
  if (function.IsNull()) {
    return false;
  }
  if (!function.IsStaticFunction() || function.IsLocalFunction() ||
      function.IsClosureFunction() || function.IsImplicitGetterOrSetter()) {
    return false;
  }
  Zone* zone = Thread::Current()->zone();
  const Script& script = Script::Handle(zone, function.script());
  if (script.IsNull()) {
    return false;
  }
  const String& url = String::Handle(zone, script.url());
  if (url.IsNull()) {
    return false;
  }
  const char* url_chars = url.ToCString();
  if (url_chars == nullptr) {
    return false;
  }
  return !IsSdkOrFlutterScriptUrl(url_chars);
}

bool IsPatchableFunctionCandidate(const Function& function) {
  if (!IsStaticPatchableFunction(function)) {
    return false;
  }
  Zone* zone = Thread::Current()->zone();
  const Script& script = Script::Handle(zone, function.script());
  const String& url = String::Handle(zone, script.url());
  const char* url_chars = url.ToCString();
  const std::string qualified = QualifiedFunctionId(function);
  if (!HasProbeAllowlist()) {
    return true;
  }
  return MatchesProbeAllowlist(url_chars, qualified,
                               std::string(url_chars) + "::" + qualified);
}

bool ObjectToValue(Thread* thread,
                   const Object& object,
                   Value* out_value,
                   std::string* error,
                   intptr_t depth) {
  if (out_value == nullptr) {
    return false;
  }
  if (depth > kMaxBridgeCollectionDepth) {
    if (error != nullptr) {
      *error = "collection nesting exceeds FCB bridge limit";
    }
    return false;
  }
  if (object.IsNull()) {
    *out_value = Value::Null();
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsInteger()) {
    *out_value = Value::Int(Integer::Cast(object).Value());
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsDouble()) {
    *out_value = Value::Double(Double::Cast(object).value());
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsBool()) {
    *out_value = Value::Bool(Bool::Cast(object).value());
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsString()) {
    *out_value = Value::String(String::Cast(object).ToCString());
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsArray()) {
    Zone* zone = thread == nullptr ? Thread::Current()->zone() : thread->zone();
    const Array& array = Array::Cast(object);
    std::vector<Value> values;
    values.reserve(array.Length());
    Object& element = Object::ZoneHandle(zone);
    for (intptr_t i = 0; i < array.Length(); ++i) {
      element = array.At(i);
      Value value;
      if (!ObjectToValue(thread, element, &value, error, depth + 1)) {
        return false;
      }
      values.push_back(std::move(value));
    }
    *out_value = Value::List(std::move(values));
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsGrowableObjectArray()) {
    Zone* zone = thread == nullptr ? Thread::Current()->zone() : thread->zone();
    const GrowableObjectArray& array = GrowableObjectArray::Cast(object);
    std::vector<Value> values;
    values.reserve(array.Length());
    Object& element = Object::ZoneHandle(zone);
    for (intptr_t i = 0; i < array.Length(); ++i) {
      element = array.At(i);
      Value value;
      if (!ObjectToValue(thread, element, &value, error, depth + 1)) {
        return false;
      }
      values.push_back(std::move(value));
    }
    *out_value = Value::List(std::move(values));
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsInstance()) {
    *out_value = Value::Null();
    out_value->object_value = object.ptr();
    return true;
  }
  if (object.IsMap()) {
    Zone* zone = thread == nullptr ? Thread::Current()->zone() : thread->zone();
    const Map& map = Map::Cast(object);
    std::vector<Value> entries;
    entries.reserve(static_cast<std::size_t>(map.Length()) * 2);
    Map::Iterator iterator(map);
    Object& member = Object::ZoneHandle(zone);
    while (iterator.MoveNext()) {
      member = iterator.CurrentKey();
      Value key;
      if (!ObjectToValue(thread, member, &key, error, depth + 1)) {
        return false;
      }
      member = iterator.CurrentValue();
      Value value;
      if (!ObjectToValue(thread, member, &value, error, depth + 1)) {
        return false;
      }
      entries.push_back(std::move(key));
      entries.push_back(std::move(value));
    }
    *out_value = Value::Map(std::move(entries));
    out_value->object_value = object.ptr();
    return true;
  }
  if (error != nullptr) {
    *error = "unsupported argument type";
  }
  return false;
}

bool ValueToObject(Thread* thread,
                   const Value& value,
                   ObjectPtr* out_object,
                   std::string* error);

bool MapValueToObject(Thread* thread,
                      const Value& value,
                      ObjectPtr* out_object,
                      std::string* error) {
  if ((value.map_entries.size() % 2) != 0) {
    if (error != nullptr) {
      *error = "map return contains an odd number of key/value entries";
    }
    return false;
  }

  Zone* zone = thread == nullptr ? Thread::Current()->zone() : thread->zone();
  const Array& key_value_pairs =
      Array::Handle(zone, Array::New(value.map_entries.size()));
  for (intptr_t i = 0; i < static_cast<intptr_t>(value.map_entries.size());
       ++i) {
    ObjectPtr element = Object::null();
    if (!ValueToObject(thread, value.map_entries[i], &element, error)) {
      return false;
    }
    key_value_pairs.SetAt(i, Object::Handle(zone, element));
  }

  const Library& compact_hash_lib =
      Library::Handle(zone, Library::CompactHashLibrary());
  if (compact_hash_lib.IsNull()) {
    if (error != nullptr) {
      *error = "dart:_compact_hash library is unavailable";
    }
    return false;
  }

  const String& function_name = String::Handle(
      zone, Symbols::New(thread, "createMapFromKeyValueListUnsafe"));
  const Function& function = Function::Handle(
      zone, compact_hash_lib.LookupFunctionAllowPrivate(function_name));
  if (function.IsNull()) {
    if (error != nullptr) {
      *error = "dart:_compact_hash createMapFromKeyValueListUnsafe not found";
    }
    return false;
  }

  const Array& arguments = Array::Handle(zone, Array::New(1));
  arguments.SetAt(0, key_value_pairs);
  *out_object = DartEntry::InvokeFunction(function, arguments);
  return true;
}

bool ValueToObject(Thread* thread,
                   const Value& value,
                   ObjectPtr* out_object,
                   std::string* error) {
  if (out_object == nullptr) {
    return false;
  }
  if (value.object_value != nullptr) {
    *out_object = value.object_value;
    return true;
  }
  switch (value.kind) {
    case ValueKind::kNull:
      *out_object = Object::null();
      return true;
    case ValueKind::kInt:
      *out_object = Integer::New(value.int_value);
      return true;
    case ValueKind::kDouble:
      *out_object = Double::New(value.double_value);
      return true;
    case ValueKind::kBool:
      *out_object = Bool::Get(value.bool_value).ptr();
      return true;
    case ValueKind::kString:
      *out_object = String::New(value.string_value.c_str());
      return true;
    case ValueKind::kList: {
      Zone* zone = thread == nullptr ? Thread::Current()->zone() : thread->zone();
      const Array& array =
          Array::Handle(zone, Array::New(value.list_value.size()));
      for (intptr_t i = 0; i < static_cast<intptr_t>(value.list_value.size());
           ++i) {
        ObjectPtr element = Object::null();
        if (!ValueToObject(thread, value.list_value[i], &element, error)) {
          return false;
        }
        array.SetAt(i, Object::Handle(zone, element));
      }
      *out_object = array.ptr();
      return true;
    }
    case ValueKind::kMap:
      return MapValueToObject(thread, value, out_object, error);
    case ValueKind::kBytecodeClosure: {
      Value mutable_value = value;
      return internal::TryMaterializeDartObject(&mutable_value, out_object,
                                                error);
    }
  }
  UNREACHABLE();
  return false;
}

ErrorPtr ApiErrorFromMessage(const std::string& message) {
  const String& error =
      String::Handle(String::New(("FCB patch interpreter failed: " + message)
                                     .c_str()));
  return ApiError::New(error);
}

void RecordInterpreterCall() {
#if defined(__GNUC__)
  if (fcb_record_interpreter_call == nullptr) {
    return;
  }
#endif
  fcb_record_interpreter_call();
}

void RecordAotCall() {
#if defined(__GNUC__)
  if (fcb_record_aot_call == nullptr) {
    return;
  }
#endif
  fcb_record_aot_call();
}

void ReportInterpretFailure(const std::string& function_id,
                            const std::string& error) {
#if defined(__GNUC__)
  if (fcb_report_interpret_failure == nullptr) {
    return;
  }
#endif
  int patch_number = -1;
#if defined(__GNUC__) && !defined(__APPLE__)
  if (fcb_active_patch_number != nullptr) {
    patch_number = fcb_active_patch_number();
  } else if (fcb_current_patch_number != nullptr) {
    patch_number = fcb_current_patch_number();
  }
#else
  patch_number = fcb_active_patch_number();
#endif
  if (patch_number < 0) {
    return;
  }
  fcb_report_interpret_failure(patch_number, function_id.c_str(),
                               error.c_str());
}

DispatchDecision ResolveFunctionPatch(PatchRuntime* runtime,
                                      const Function& function,
                                      std::string* out_function_id) {
  if (runtime == nullptr || function.IsNull()) {
    return {};
  }

  std::string function_id = FunctionIdFor(function);
  DispatchDecision decision = ResolveFunctionEntry(runtime, function_id);
  if (decision.state == PatchState::kPatchedInterpreted) {
    if (out_function_id != nullptr) {
      *out_function_id = std::move(function_id);
    }
    return decision;
  }

  const std::string qualified = QualifiedFunctionId(function);
  if (qualified == function_id) {
    return decision;
  }
  decision = ResolveFunctionEntry(runtime, qualified);
  if (decision.state == PatchState::kPatchedInterpreted &&
      out_function_id != nullptr) {
    *out_function_id = qualified;
  }
  return decision;
}

}  // namespace

DispatchDecision ResolveFunctionEntry(PatchRuntime* runtime,
                                      const std::string& function_id) {
  if (runtime == nullptr) {
    return {};
  }
  return runtime->Resolve(function_id);
}

DispatchDecision ResolveFunctionEntry(IsolateGroup* isolate_group,
                                      const std::string& function_id) {
  if (isolate_group == nullptr) {
    return {};
  }
  return ResolveFunctionEntry(isolate_group->fcb_patch_runtime(), function_id);
}

std::string FunctionIdFor(const Function& function) {
  return ScriptQualifiedFunctionId(function);
}

bool ShouldProbeFunction(Thread* thread, const Function& function) {
  const bool candidate = IsPatchableFunctionCandidate(function);
  const bool should_probe = candidate;
  if (FLAG_trace_fcb_dispatch && candidate &&
      std::strstr(function.ToFullyQualifiedCString(), "initialCounterValue") !=
          nullptr) {
    OS::PrintErr("FCB ShouldProbeFunction target=%s id=%s candidate=%s "
                 "probe=%s\n",
                 function.ToFullyQualifiedCString(),
                 FunctionIdFor(function).c_str(), candidate ? "true" : "false",
                 should_probe ? "true" : "false");
  }
  return should_probe;
}

bool IsFunctionPatched(Thread* thread, const Function& function) {
  if (!IsStaticPatchableFunction(function)) {
    return false;
  }
  IsolateGroup* isolate_group = thread->isolate_group();
  if (isolate_group == nullptr || isolate_group->fcb_patch_runtime() == nullptr) {
    return false;
  }
  const DispatchDecision decision =
      ResolveFunctionPatch(isolate_group->fcb_patch_runtime(), function,
                           /*out_function_id=*/nullptr);
  return decision.state == PatchState::kPatchedInterpreted &&
         decision.function != nullptr;
}

bool TryInvokePatchedFunctionImpl(Thread* thread,
                                  Zone* zone,
                                  const Function& function,
                                  const ObjectPtr* arguments,
                                  intptr_t argument_count,
                                  ObjectPtr* out_result,
                                  ReturnConvention* out_return_convention,
                                  bool require_probe_allowlist) {
  if (IsPatchInvocationSuppressed()) {
    return false;
  }
  if (out_result == nullptr) {
    return false;
  }
  if (require_probe_allowlist) {
    if (!ShouldProbeFunction(thread, function)) {
      return false;
    }
  } else if (!IsStaticPatchableFunction(function)) {
    return false;
  }
  IsolateGroup* isolate_group = thread->isolate_group();
  if (isolate_group == nullptr || isolate_group->fcb_patch_runtime() == nullptr) {
    return false;
  }

  PatchRuntime* runtime = isolate_group->fcb_patch_runtime();
  std::string function_id;
  DispatchDecision decision =
      ResolveFunctionPatch(runtime, function, &function_id);
  if (decision.state != PatchState::kPatchedInterpreted ||
      decision.function == nullptr) {
    if (FLAG_trace_fcb_dispatch) {
      OS::PrintErr("FCB TryInvoke miss target=%s id=%s state=%d\n",
                   function.ToFullyQualifiedCString(), function_id.c_str(),
                   static_cast<int>(decision.state));
    }
    RecordAotCall();
    return false;
  }

  if (decision.function->parameter_count != argument_count) {
    if (FLAG_trace_fcb_dispatch) {
      OS::PrintErr("FCB TryInvoke arg mismatch target=%s id=%s expected=%" Pu
                   " actual=%" Pu "\n",
                   function.ToFullyQualifiedCString(), function_id.c_str(),
                   static_cast<intptr_t>(decision.function->parameter_count),
                   argument_count);
    }
    RecordAotCall();
    return false;
  }
  if (out_return_convention != nullptr) {
    *out_return_convention = decision.function->return_convention;
  }
  Zone* handle_zone = thread != nullptr ? thread->zone() : zone;
  if (handle_zone == nullptr) {
    *out_result = ApiErrorFromMessage("FCB patch invocation has no handle zone");
    return true;
  }

  std::vector<Value> values(argument_count, Value::Null());
  for (intptr_t i = 0; i < argument_count; ++i) {
    if (!runtime->FunctionUsesArgument(*decision.function,
                                       static_cast<uint8_t>(i))) {
      continue;
    }
    Object& argument = Object::Handle(handle_zone);
    argument = arguments[i];
    Value value;
    std::string conversion_error;
    if (!ObjectToValue(thread, argument, &value, &conversion_error, 0)) {
      *out_result = ApiErrorFromMessage(conversion_error.empty()
                                            ? "unsupported argument type for " +
                                                  function_id
                                            : conversion_error + " for " +
                                                  function_id);
      return true;
    }
    values[i] = std::move(value);
  }

  InterpretResult result = runtime->Interpret(function_id, values);
  if (!result.ok) {
    if (FLAG_trace_fcb_dispatch) {
      OS::PrintErr("FCB TryInvoke interpreter error target=%s id=%s error=%s\n",
                   function.ToFullyQualifiedCString(), function_id.c_str(),
                   result.error.c_str());
    }
    runtime->DisablePatch(function_id);
    ReportInterpretFailure(function_id, result.error);
    RecordAotCall();
    return false;
  }
  RecordInterpreterCall();

  std::string conversion_error;
  if (!ValueToObject(thread, result.value, out_result, &conversion_error)) {
    const std::string error =
        conversion_error.empty() ? "unsupported return type for " + function_id
                                 : conversion_error + " for " + function_id;
    if (FLAG_trace_fcb_dispatch) {
      OS::PrintErr("FCB TryInvoke return conversion error target=%s id=%s "
                   "error=%s\n",
                   function.ToFullyQualifiedCString(), function_id.c_str(),
                   error.c_str());
    }
    runtime->DisablePatch(function_id);
    ReportInterpretFailure(function_id, error);
    RecordAotCall();
    return false;
  }
  if (FLAG_trace_fcb_dispatch) {
    OS::PrintErr("FCB TryInvoke handled target=%s id=%s\n",
                 function.ToFullyQualifiedCString(), function_id.c_str());
  }
  return true;
}

bool TryInvokePatchedFunction(Thread* thread,
                              const Function& function,
                              const std::vector<ObjectPtr>& arguments,
                              ObjectPtr* out_result,
                              ReturnConvention* out_return_convention) {
  return TryInvokePatchedFunctionImpl(thread, /*zone=*/nullptr, function,
                                      arguments.data(), arguments.size(),
                                      out_result,
                                      out_return_convention,
                                      /*require_probe_allowlist=*/true);
}

bool TryInvokePatchedAotFunction(Thread* thread,
                                 Zone* zone,
                                 const Function& function,
                                 const ObjectPtr* arguments,
                                 intptr_t argument_count,
                                 ObjectPtr* out_result,
                                 ReturnConvention* out_return_convention) {
  return TryInvokePatchedFunctionImpl(thread, zone, function, arguments,
                                      argument_count,
                                      out_result, out_return_convention,
                                      /*require_probe_allowlist=*/false);
}

bool TryInvokePatchedFunction(Thread* thread,
                              const Function& function,
                              const Array& arguments,
                              ObjectPtr* out_result) {
  std::vector<ObjectPtr> raw_arguments;
  raw_arguments.reserve(arguments.Length());
  for (intptr_t i = 0; i < arguments.Length(); ++i) {
    raw_arguments.push_back(arguments.At(i));
  }
  return TryInvokePatchedFunction(thread, function, raw_arguments, out_result);
}

bool TryInvokeBytecodeClosureTrampoline(Thread* thread,
                                        const Function& function,
                                        const Array& arguments,
                                        const Array& arguments_descriptor,
                                        ObjectPtr* out_result) {
  if (thread == nullptr || out_result == nullptr ||
      !internal::IsFcbBytecodeClosureTrampoline(function)) {
    return false;
  }
  IsolateGroup* isolate_group = thread->isolate_group();
  if (isolate_group == nullptr || isolate_group->fcb_patch_runtime() == nullptr) {
    *out_result =
        ApiErrorFromMessage("FCB bytecode closure runtime is unavailable");
    return true;
  }
  const ArgumentsDescriptor args_desc(arguments_descriptor);
  const intptr_t first_arg_index = args_desc.FirstArgIndex();
  if (arguments.Length() <= first_arg_index) {
    *out_result = ApiErrorFromMessage(
        "FCB bytecode closure invocation missing closure receiver");
    return true;
  }

  Zone* zone = thread->zone();
  const Object& raw_receiver =
      Object::Handle(zone, arguments.At(first_arg_index));
  if (!raw_receiver.IsClosure()) {
    *out_result =
        ApiErrorFromMessage("FCB bytecode closure receiver is not a Closure");
    return true;
  }
  const Closure& receiver = Closure::Cast(raw_receiver);
  const Object& raw_context = Object::Handle(zone, receiver.RawContext());
  if (!raw_context.IsContext()) {
    *out_result =
        ApiErrorFromMessage("FCB bytecode closure has no capture context");
    return true;
  }

  const std::string function_id =
      internal::FcbBytecodeClosureTargetId(function);
  PatchRuntime* runtime = isolate_group->fcb_patch_runtime();
  const DispatchDecision decision = runtime->Resolve(function_id);
  if (decision.state != PatchState::kPatchedInterpreted ||
      decision.function == nullptr) {
    *out_result =
        ApiErrorFromMessage("FCB bytecode closure target is not installed");
    return true;
  }
  const Context& context = Context::Cast(raw_context);
  const intptr_t capture_count = context.num_variables();
  if (decision.function->parameter_count < capture_count) {
    *out_result =
        ApiErrorFromMessage("FCB bytecode closure capture count exceeds target "
                            "parameter count");
    return true;
  }
  const intptr_t exposed_parameter_count =
      decision.function->parameter_count - capture_count;
  const intptr_t expected_type_args_len = function.NumTypeParameters();
  if (args_desc.TypeArgsLen() != expected_type_args_len) {
    *out_result = ApiErrorFromMessage(
        "FCB bytecode closure type argument count mismatch");
    return true;
  }
  const intptr_t actual_positional_count = args_desc.PositionalCount() - 1;
  const intptr_t named_count = function.NumOptionalNamedParameters();
  const intptr_t optional_positional_count =
      function.NumOptionalPositionalParameters();
  const intptr_t expected_positional_count =
      exposed_parameter_count - named_count;
  if (actual_positional_count < 0 ||
      expected_positional_count < optional_positional_count) {
    *out_result =
        ApiErrorFromMessage("FCB bytecode closure argument descriptor mismatch");
    return true;
  }
  std::vector<Value> values;
  values.reserve(decision.function->parameter_count);
  std::string conversion_error;
  for (intptr_t i = 0; i < capture_count; i++) {
    const Object& capture = Object::Handle(zone, context.At(i));
    Value value;
    if (!ObjectToValue(thread, capture, &value, &conversion_error, 0)) {
      *out_result = ApiErrorFromMessage(
          "capture " + std::to_string(i) + " for " + function_id + ": " +
          conversion_error);
      return true;
    }
    values.push_back(std::move(value));
  }
  for (intptr_t i = 0; i < expected_positional_count; i++) {
    if (i >= actual_positional_count) {
      values.push_back(Value::Null());
      continue;
    }
    const intptr_t argument_index = first_arg_index + 1 + i;
    const Object& argument = Object::Handle(zone, arguments.At(argument_index));
    Value value;
    if (!ObjectToValue(thread, argument, &value, &conversion_error, 0)) {
      *out_result = ApiErrorFromMessage(
          "argument " + std::to_string(i) + " for " + function_id + ": " +
          conversion_error);
      return true;
    }
    values.push_back(std::move(value));
  }
  if (named_count > 0) {
    String& expected_name = String::Handle(zone);
    String& actual_name = String::Handle(zone);
    for (intptr_t named_index = 0; named_index < named_count; named_index++) {
      const intptr_t parameter_index =
          function.num_fixed_parameters() + named_index;
      expected_name = function.ParameterNameAt(parameter_index);
      intptr_t argument_index = -1;
      for (intptr_t i = 0; i < args_desc.NamedCount(); i++) {
        actual_name = args_desc.NameAt(i);
        if (actual_name.ptr() == expected_name.ptr()) {
          argument_index = args_desc.PositionAt(i);
          break;
        }
      }
      if (argument_index < 0) {
        if (function.IsRequiredAt(parameter_index)) {
          *out_result = ApiErrorFromMessage(
              "missing required bytecode closure named argument " +
              StringToStdString(expected_name.ptr()));
          return true;
        }
        values.push_back(Value::Null());
        continue;
      }
      const Object& argument =
          Object::Handle(zone, arguments.At(argument_index));
      Value value;
      if (!ObjectToValue(thread, argument, &value, &conversion_error, 0)) {
        *out_result = ApiErrorFromMessage(
            "named argument " + StringToStdString(expected_name.ptr()) +
            " for " + function_id + ": " + conversion_error);
        return true;
      }
      values.push_back(std::move(value));
    }
  }

  InterpretResult result = runtime->Interpret(function_id, values,
                                             capture_count);
  if (!result.ok) {
    runtime->DisablePatch(function_id);
    ReportInterpretFailure(function_id, result.error);
    *out_result = ApiErrorFromMessage(result.error);
    RecordAotCall();
    return true;
  }
  RecordInterpreterCall();
  std::string return_error;
  if (!ValueToObject(thread, result.value, out_result, &return_error)) {
    runtime->DisablePatch(function_id);
    ReportInterpretFailure(function_id, return_error);
    *out_result = ApiErrorFromMessage(return_error);
    RecordAotCall();
    return true;
  }
  return true;
}

bool TryInvokeUniquePatchedFunctionByArity(Thread* thread,
                                           const std::vector<ObjectPtr>& arguments,
                                           ObjectPtr* out_result,
                                           ReturnConvention* out_return_convention) {
  if (thread == nullptr || out_result == nullptr) {
    return false;
  }
  IsolateGroup* isolate_group = thread->isolate_group();
  if (isolate_group == nullptr || isolate_group->fcb_patch_runtime() == nullptr) {
    return false;
  }

  PatchRuntime* runtime = isolate_group->fcb_patch_runtime();
  std::string function_id;
  DispatchDecision decision = runtime->ResolveUniqueByArity(
      arguments.size(), &function_id);
  if (decision.state != PatchState::kPatchedInterpreted ||
      decision.function == nullptr) {
    if (FLAG_trace_fcb_dispatch) {
      OS::PrintErr("FCB TryInvoke unique miss args=%" Pu "\n",
                   static_cast<intptr_t>(arguments.size()));
    }
    RecordAotCall();
    return false;
  }
  if (out_return_convention != nullptr) {
    *out_return_convention = decision.function->return_convention;
  }

  std::vector<Value> values;
  std::string error;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    Object& argument = Object::Handle(thread->zone());
    argument = arguments[i];
    Value value;
    if (!ObjectToValue(thread, argument, &value, &error, 0)) {
      *out_result = ApiErrorFromMessage("argument " + std::to_string(i) +
                                        " for " + function_id + ": " +
                                        error);
      return true;
    }
    values.push_back(std::move(value));
  }

  InterpretResult result = runtime->Interpret(function_id, values);
  if (!result.ok) {
    if (FLAG_trace_fcb_dispatch) {
      OS::PrintErr("FCB TryInvoke unique interpreter error id=%s error=%s\n",
                   function_id.c_str(), result.error.c_str());
    }
    runtime->DisablePatch(function_id);
    ReportInterpretFailure(function_id, result.error);
    RecordAotCall();
    return false;
  }
  RecordInterpreterCall();
  std::string conversion_error;
  if (!ValueToObject(thread, result.value, out_result, &conversion_error)) {
    const std::string error = "return value for " + function_id + ": " +
                              (conversion_error.empty()
                                   ? "unsupported return type"
                                   : conversion_error);
    if (FLAG_trace_fcb_dispatch) {
      OS::PrintErr("FCB TryInvoke unique return conversion error id=%s "
                   "error=%s\n",
                   function_id.c_str(), error.c_str());
    }
    runtime->DisablePatch(function_id);
    ReportInterpretFailure(function_id, error);
    RecordAotCall();
    return false;
  }
  if (FLAG_trace_fcb_dispatch) {
    OS::PrintErr("FCB TryInvoke unique handled id=%s\n", function_id.c_str());
  }
  return true;
}

bool TryInvokeUniquePatchedFunctionByArity(Thread* thread,
                                           const Array& arguments,
                                           ObjectPtr* out_result) {
  std::vector<ObjectPtr> raw_arguments;
  raw_arguments.reserve(arguments.Length());
  for (intptr_t i = 0; i < arguments.Length(); ++i) {
    raw_arguments.push_back(arguments.At(i));
  }
  return TryInvokeUniquePatchedFunctionByArity(thread, raw_arguments,
                                               out_result,
                                               /*out_return_convention=*/nullptr);
}

}  // namespace fcb
}  // namespace dart
