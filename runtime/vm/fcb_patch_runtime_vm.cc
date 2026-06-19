// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime_internal.h"

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
#include "vm/class_finalizer.h"
#include "vm/dart_entry.h"
#include "vm/fcb_patch_entry.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/resolver.h"
#include "vm/symbols.h"
#include "vm/thread.h"
#endif

namespace dart {
namespace fcb {
namespace internal {
namespace {

bool SetErrorMessage(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
struct PatchStackTraceFrame {
  std::string function_id;
  std::string source_location;
  uint32_t bytecode_offset = 0;
  const BytecodeFunction* function = nullptr;
  std::size_t captured_argument_count = 0;
  const std::vector<Value>* arguments = nullptr;
  const std::vector<Value>* locals = nullptr;
  std::size_t active_handler_count = 0;
  uint32_t innermost_handler_offset = 0;
  uint32_t innermost_handler_end_offset = 0;
};

thread_local std::vector<PatchStackTraceFrame> patch_stack_trace_frames;
thread_local std::vector<PatchStackTraceFrame> active_patch_frames;
thread_local ActivePatchFrameUpdateCallback active_frame_update_callback =
    nullptr;

PatchStackTraceFrame MakePatchStackTraceFrame(const BytecodeFunction& function,
                                              uint32_t bytecode_offset) {
  PatchStackTraceFrame frame;
  frame.function_id = function.function_id;
  frame.bytecode_offset = bytecode_offset;
  frame.function = &function;
  const SourceMapEntry* best = nullptr;
  for (const SourceMapEntry& entry : function.source_map) {
    if (entry.bytecode_offset > bytecode_offset) {
      continue;
    }
    if (best == nullptr || entry.bytecode_offset >= best->bytecode_offset) {
      best = &entry;
    }
  }
  if (best != nullptr) {
    frame.source_location = best->source_location;
  }
  return frame;
}

std::string Trim(const std::string& value) {
  const std::size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::vector<std::string> SplitNamedArgumentNames(const std::string& names) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= names.size()) {
    const std::size_t comma = names.find(',', start);
    const std::size_t end = comma == std::string::npos ? names.size() : comma;
    if (end > start) {
      result.push_back(names.substr(start, end - start));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return result;
}

bool SplitTopLevelTypeNames(const std::string& names,
                            std::vector<std::string>* out,
                            std::string* error) {
  intptr_t depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= names.size(); i++) {
    const char ch = i < names.size() ? names[i] : ',';
    if (ch == '<') {
      depth++;
    } else if (ch == '>') {
      depth--;
      if (depth < 0) {
        return SetErrorMessage(error, "malformed type argument list");
      }
    }
    if (ch == ',' && depth == 0) {
      const std::string part = Trim(names.substr(start, i - start));
      if (part.empty()) {
        return SetErrorMessage(error, "empty type argument");
      }
      out->push_back(part);
      start = i + 1;
    }
  }
  if (depth != 0) {
    return SetErrorMessage(error, "unterminated type argument list");
  }
  return true;
}

bool ParseTypeName(const std::string& raw,
                   std::string* base_name,
                   std::vector<std::string>* type_argument_names,
                   std::string* error) {
  const std::string type_name = Trim(raw);
  const std::size_t args_start = type_name.find('<');
  if (args_start == std::string::npos) {
    *base_name = type_name;
    return true;
  }
  if (type_name.empty() || type_name.back() != '>') {
    return SetErrorMessage(error, "malformed generic type " + raw);
  }
  *base_name = Trim(type_name.substr(0, args_start));
  if (base_name->empty()) {
    return SetErrorMessage(error, "generic type has an empty base name");
  }
  return SplitTopLevelTypeNames(
      type_name.substr(args_start + 1, type_name.size() - args_start - 2),
      type_argument_names, error);
}

std::string StripNamedArguments(
    const std::string& target_id,
    std::vector<std::string>* named_argument_names) {
  const std::string marker = ";named:";
  const std::size_t marker_index = target_id.find(marker);
  if (marker_index == std::string::npos) {
    return target_id;
  }
  if (named_argument_names != nullptr) {
    *named_argument_names =
        SplitNamedArgumentNames(target_id.substr(marker_index + marker.size()));
  }
  return target_id.substr(0, marker_index);
}

bool StripConstructorTypeArguments(
    const std::string& constructor_id,
    std::vector<std::string>* type_argument_names,
    std::string* out,
    std::string* error) {
  const std::string marker = ";types:";
  const std::size_t marker_index = constructor_id.find(marker);
  if (marker_index == std::string::npos) {
    *out = constructor_id;
    return true;
  }
  const std::size_t value_start = marker_index + marker.size();
  const std::size_t value_end = constructor_id.find(';', value_start);
  if (type_argument_names != nullptr) {
    if (!SplitTopLevelTypeNames(
            constructor_id.substr(value_start, value_end - value_start),
            type_argument_names, error)) {
      return false;
    }
  }
  if (value_end == std::string::npos) {
    *out = constructor_id.substr(0, marker_index);
    return true;
  }
  *out =
      constructor_id.substr(0, marker_index) + constructor_id.substr(value_end);
  return true;
}

bool HasCurrentDartThread() {
  return Thread::Current() != nullptr;
}

ObjectPtr MaterializeDartMap(Value* value) {
  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  const intptr_t pair_count =
      static_cast<intptr_t>(value->map_entries.size() / 2);

  const Array& keys = Array::Handle(zone, Array::New(pair_count));
  const Array& values = Array::Handle(zone, Array::New(pair_count));
  for (intptr_t i = 0; i < pair_count; i++) {
    const Object& key =
        Object::Handle(zone, MaterializeDartObject(&value->map_entries[2 * i]));
    const Object& map_value = Object::Handle(
        zone, MaterializeDartObject(&value->map_entries[(2 * i) + 1]));
    keys.SetAt(i, key, thread);
    values.SetAt(i, map_value, thread);
  }

  const Type& dynamic_type = Type::Handle(zone, Type::DynamicType());
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, TypeArguments::New(2, Heap::kOld));
  type_arguments.SetTypeAt(0, dynamic_type);
  type_arguments.SetTypeAt(1, dynamic_type);
  const TypeArguments& canonical_type_arguments =
      TypeArguments::Handle(zone, type_arguments.Canonicalize(thread));

  const Class& map_class =
      Class::Handle(zone, thread->isolate_group()->object_store()->map_class());
  if (map_class.EnsureIsFinalized(thread) != Error::null()) {
    return Object::null();
  }
  const Function& factory = Function::Handle(
      zone, map_class.LookupFactoryAllowPrivate(
                Library::PrivateCoreLibName(Symbols::MapKeyValuesFactory())));
  if (factory.IsNull()) {
    return Object::null();
  }

  const Array& args = Array::Handle(zone, Array::New(3));
  args.SetAt(0, canonical_type_arguments, thread);
  args.SetAt(1, keys, thread);
  args.SetAt(2, values, thread);
  const Object& result =
      Object::Handle(zone, DartEntry::InvokeFunction(factory, args));
  if (result.IsError()) {
    return Object::null();
  }
  return result.ptr();
}
#endif

}  // namespace

std::size_t CountNamedArguments(const std::string& metadata) {
  constexpr char kMarker[] = ";named:";
  if (metadata.rfind(kMarker, 0) != 0) return 0;
  const std::size_t names_start = sizeof(kMarker) - 1;
  std::size_t count = 0;
  std::size_t start = names_start;
  while (start <= metadata.size()) {
    const std::size_t comma = metadata.find(',', start);
    const std::size_t end =
        comma == std::string::npos ? metadata.size() : comma;
    if (end > start) count++;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return count;
}

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
void RecordPatchStackTraceLocation(const BytecodeFunction& function,
                                   uint32_t bytecode_offset) {
  PatchStackTraceFrame frame =
      MakePatchStackTraceFrame(function, bytecode_offset);
  if (!frame.source_location.empty()) {
    if (patch_stack_trace_frames.empty() ||
        patch_stack_trace_frames.back().source_location !=
            frame.source_location) {
      patch_stack_trace_frames.push_back(std::move(frame));
    }
  }
}

const char* LastPatchStackTraceLocation() {
  return patch_stack_trace_frames.empty()
             ? nullptr
             : patch_stack_trace_frames.back().source_location.c_str();
}

std::size_t PatchStackTraceLocationCount() {
  return patch_stack_trace_frames.size();
}

const char* PatchStackTraceLocationAt(std::size_t index) {
  if (index >= patch_stack_trace_frames.size()) {
    return nullptr;
  }
  return patch_stack_trace_frames[index].source_location.c_str();
}

PatchStackTraceFrameInfo PatchStackTraceFrameInfoAt(std::size_t index) {
  if (index >= patch_stack_trace_frames.size()) {
    return PatchStackTraceFrameInfo{
        nullptr, nullptr, 0, nullptr, nullptr, 0, 0, nullptr, 0, 0, 0, 0};
  }
  const PatchStackTraceFrame& frame = patch_stack_trace_frames[index];
  return PatchStackTraceFrameInfo{frame.function_id.c_str(),
                                  frame.source_location.c_str(),
                                  frame.bytecode_offset,
                                  frame.function,
                                  nullptr,
                                  0,
                                  frame.captured_argument_count,
                                  nullptr,
                                  0,
                                  frame.active_handler_count,
                                  frame.innermost_handler_offset,
                                  frame.innermost_handler_end_offset};
}

void ClearPatchStackTraceLocation() {
  patch_stack_trace_frames.clear();
}

void PushActivePatchFrame(const BytecodeFunction& function,
                          uint32_t bytecode_offset,
                          std::size_t captured_argument_count,
                          const std::vector<Value>* arguments,
                          const std::vector<Value>* locals,
                          std::size_t active_handler_count,
                          uint32_t innermost_handler_offset,
                          uint32_t innermost_handler_end_offset) {
  PatchStackTraceFrame frame =
      MakePatchStackTraceFrame(function, bytecode_offset);
  frame.captured_argument_count = captured_argument_count;
  frame.arguments = arguments;
  frame.locals = locals;
  frame.active_handler_count = active_handler_count;
  frame.innermost_handler_offset = innermost_handler_offset;
  frame.innermost_handler_end_offset = innermost_handler_end_offset;
  active_patch_frames.push_back(std::move(frame));
}

void UpdateActivePatchFrame(const BytecodeFunction& function,
                            uint32_t bytecode_offset,
                            std::size_t captured_argument_count,
                            const std::vector<Value>* arguments,
                            const std::vector<Value>* locals,
                            std::size_t active_handler_count,
                            uint32_t innermost_handler_offset,
                            uint32_t innermost_handler_end_offset) {
  if (active_patch_frames.empty()) {
    return;
  }
  active_patch_frames.back() = MakePatchStackTraceFrame(function, bytecode_offset);
  active_patch_frames.back().captured_argument_count = captured_argument_count;
  active_patch_frames.back().arguments = arguments;
  active_patch_frames.back().locals = locals;
  active_patch_frames.back().active_handler_count = active_handler_count;
  active_patch_frames.back().innermost_handler_offset =
      innermost_handler_offset;
  active_patch_frames.back().innermost_handler_end_offset =
      innermost_handler_end_offset;
  if (active_frame_update_callback != nullptr) {
    active_frame_update_callback(
        ActivePatchFrameInfoAt(active_patch_frames.size() - 1));
  }
}

void PopActivePatchFrame() {
  if (!active_patch_frames.empty()) {
    active_patch_frames.pop_back();
  }
}

std::size_t ActivePatchFrameCount() {
  return active_patch_frames.size();
}

PatchStackTraceFrameInfo ActivePatchFrameInfoAt(std::size_t index) {
  if (index >= active_patch_frames.size()) {
    return PatchStackTraceFrameInfo{
        nullptr, nullptr, 0, nullptr, nullptr, 0, 0, nullptr, 0, 0, 0, 0};
  }
  const PatchStackTraceFrame& frame = active_patch_frames[index];
  return PatchStackTraceFrameInfo{frame.function_id.c_str(),
                                  frame.source_location.c_str(),
                                  frame.bytecode_offset,
                                  frame.function,
                                  frame.arguments == nullptr
                                      ? nullptr
                                      : frame.arguments->data(),
                                  frame.arguments == nullptr
                                      ? 0
                                      : frame.arguments->size(),
                                  frame.captured_argument_count,
                                  frame.locals == nullptr
                                      ? nullptr
                                      : frame.locals->data(),
                                  frame.locals == nullptr
                                      ? 0
                                      : frame.locals->size(),
                                  frame.active_handler_count,
                                  frame.innermost_handler_offset,
                                  frame.innermost_handler_end_offset};
}

void SetActivePatchFrameUpdateCallback(
    ActivePatchFrameUpdateCallback callback) {
  active_frame_update_callback = callback;
}

ScopedActivePatchFrame::ScopedActivePatchFrame(
    const BytecodeFunction& function,
    const std::vector<Value>& arguments,
    const std::vector<Value>& locals,
    std::size_t captured_argument_count)
    : function_(function),
      arguments_(arguments),
      locals_(locals),
      captured_argument_count_(captured_argument_count) {
  PushActivePatchFrame(function_, 0, captured_argument_count_, &arguments_,
                       &locals_);
}

ScopedActivePatchFrame::~ScopedActivePatchFrame() {
  PopActivePatchFrame();
}

bool ContainsBytecodeClosure(const Value& value) {
  if (value.kind == ValueKind::kBytecodeClosure) {
    return true;
  }
  for (const Value& element : value.list_value) {
    if (ContainsBytecodeClosure(element)) {
      return true;
    }
  }
  for (const Value& entry : value.map_entries) {
    if (ContainsBytecodeClosure(entry)) {
      return true;
    }
  }
  return false;
}

void PopulateScalarFromDartObject(Value* value) {
  if (value->object_value == nullptr || !HasCurrentDartThread()) {
    return;
  }
  const Object& object = Object::Handle(value->object_value);
  if (object.IsNull()) {
    value->kind = ValueKind::kNull;
  } else if (object.IsInteger()) {
    value->kind = ValueKind::kInt;
    value->int_value = Integer::Cast(object).Value();
  } else if (object.IsDouble()) {
    value->kind = ValueKind::kDouble;
    value->double_value = Double::Cast(object).value();
  } else if (object.IsBool()) {
    value->kind = ValueKind::kBool;
    value->bool_value = Bool::Cast(object).value();
  } else if (object.IsString()) {
    value->kind = ValueKind::kString;
    value->string_value = String::Cast(object).ToCString();
  }
}

ObjectPtr MaterializeDartObject(Value* value) {
  if (value->object_value != nullptr) {
    return value->object_value;
  }
  if (!HasCurrentDartThread()) {
    return nullptr;
  }

  switch (value->kind) {
    case ValueKind::kNull:
      value->object_value = Object::null();
      break;
    case ValueKind::kInt:
      value->object_value = Integer::New(value->int_value);
      break;
    case ValueKind::kDouble:
      value->object_value = Double::New(value->double_value);
      break;
    case ValueKind::kBool:
      value->object_value =
          value->bool_value ? Bool::True().ptr() : Bool::False().ptr();
      break;
    case ValueKind::kString:
      value->object_value = String::New(value->string_value.c_str());
      break;
    case ValueKind::kList: {
      const Array& array = Array::Handle(Array::New(value->list_value.size()));
      for (intptr_t i = 0; i < static_cast<intptr_t>(value->list_value.size());
           i++) {
        const Object& element =
            Object::Handle(MaterializeDartObject(&value->list_value[i]));
        array.SetAt(i, element, Thread::Current());
      }
      value->object_value = array.ptr();
      break;
    }
    case ValueKind::kMap: {
      value->object_value = MaterializeDartMap(value);
      break;
    }
    case ValueKind::kBytecodeClosure: {
      ObjectPtr raw_closure = nullptr;
      std::string error;
      if (TryMaterializeBytecodeClosure(value, &raw_closure, &error)) {
        value->object_value = raw_closure;
      } else {
        return nullptr;
      }
      break;
    }
  }
  return value->object_value;
}

bool TryMaterializeDartObject(Value* value,
                              ObjectPtr* out,
                              std::string* error) {
  if (out == nullptr) {
    return false;
  }
  if (value->kind == ValueKind::kBytecodeClosure) {
    return TryMaterializeBytecodeClosure(value, out, error);
  }
  *out = MaterializeDartObject(value);
  if (*out == nullptr) {
    return SetErrorMessage(error, "value is not a materialized Dart object");
  }
  return true;
}

bool DartInstanceGetField(const Value& receiver,
                          const std::string& field_name,
                          Value* out,
                          std::string* error) {
  if (receiver.object_value == nullptr || !HasCurrentDartThread()) {
    return false;
  }
  const Object& receiver_object = Object::Handle(receiver.object_value);
  if (!receiver_object.IsInstance()) {
    SetErrorMessage(error, "receiver is not a Dart instance");
    return false;
  }
  const Instance& receiver_instance = Instance::Cast(receiver_object);
  const Class& receiver_class = Class::Handle(receiver_instance.clazz());
  Thread* thread = Thread::Current();
  const String& name =
      String::Handle(thread->zone(), Symbols::New(thread, field_name.c_str()));
  const Field& field =
      Field::Handle(receiver_class.LookupFieldAllowPrivate(name, true));
  if (field.IsNull()) {
    SetErrorMessage(error, "missing Dart instance field");
    return false;
  }
  *out = Value::FromDart(receiver_instance.GetField(field));
  return true;
}

bool DartInstanceSetField(Value* receiver,
                          const std::string& field_name,
                          Value value,
                          std::string* error) {
  if (receiver->object_value == nullptr || !HasCurrentDartThread()) {
    return false;
  }
  const Object& receiver_object = Object::Handle(receiver->object_value);
  if (!receiver_object.IsInstance()) {
    SetErrorMessage(error, "receiver is not a Dart instance");
    return false;
  }
  const Instance& receiver_instance = Instance::Cast(receiver_object);
  const Class& receiver_class = Class::Handle(receiver_instance.clazz());
  Thread* thread = Thread::Current();
  const String& name =
      String::Handle(thread->zone(), Symbols::New(thread, field_name.c_str()));
  const Field& field =
      Field::Handle(receiver_class.LookupFieldAllowPrivate(name, true));
  if (field.IsNull()) {
    SetErrorMessage(error, "missing Dart instance field");
    return false;
  }
  ObjectPtr raw_value = nullptr;
  if (!TryMaterializeDartObject(&value, &raw_value, error)) {
    return false;
  }
  const Object& object_value = Object::Handle(raw_value);
  receiver_instance.SetField(field, object_value);
  return true;
}

bool DartInstanceCallDynamic(Value receiver,
                             const std::string& method_name,
                             std::vector<Value> arguments,
                             Value* out,
                             std::string* error,
                             Value* exception) {
  if (receiver.object_value == nullptr || !HasCurrentDartThread()) {
    return SetErrorMessage(error, "receiver is not a materialized Dart object");
  }
  const Object& receiver_object = Object::Handle(receiver.object_value);
  if (!receiver_object.IsInstance()) {
    return SetErrorMessage(error, "receiver is not a Dart instance");
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  const Instance& receiver_instance = Instance::Cast(receiver_object);
  const Class& receiver_class = Class::Handle(zone, receiver_instance.clazz());
  const Error& finalize_error =
      Error::Handle(zone, receiver_class.EnsureIsFinalized(thread));
  if (!finalize_error.IsNull()) {
    return SetErrorMessage(error, finalize_error.ToErrorCString());
  }

  std::vector<std::string> named_argument_names;
  const std::string resolved_method_name =
      StripNamedArguments(method_name, &named_argument_names);
  if (named_argument_names.size() > arguments.size()) {
    return SetErrorMessage(error, "too many named dynamic arguments");
  }
  const String& name =
      String::Handle(zone, Symbols::New(thread, resolved_method_name.c_str()));
  const intptr_t argument_count = static_cast<intptr_t>(arguments.size()) + 1;
  const intptr_t named_argument_count =
      static_cast<intptr_t>(named_argument_names.size());
  Array& argument_names = Array::Handle(zone);
  if (named_argument_count > 0) {
    argument_names = Array::New(named_argument_count);
    for (intptr_t i = 0; i < named_argument_count; i++) {
      const String& argument_name = String::Handle(
          zone, Symbols::New(thread, named_argument_names[i].c_str()));
      argument_names.SetAt(i, argument_name, thread);
    }
  }
  const Array& descriptor = Array::Handle(
      zone,
      named_argument_count == 0
          ? ArgumentsDescriptor::NewBoxed(/*type_args_len=*/0, argument_count)
          : ArgumentsDescriptor::NewBoxed(
                /*type_args_len=*/0, argument_count, argument_names));
  const ArgumentsDescriptor args_desc(descriptor);
  const Function& target = Function::Handle(
      zone, Resolver::ResolveDynamicForReceiverClass(
                receiver_class, name, args_desc, /*allow_add=*/false));
  if (target.IsNull()) {
    return SetErrorMessage(error, "method not found");
  }

  const Array& dart_arguments = Array::Handle(zone, Array::New(argument_count));
  dart_arguments.SetAt(0, receiver_object, thread);
  for (intptr_t i = 0; i < static_cast<intptr_t>(arguments.size()); i++) {
    ObjectPtr raw_argument = nullptr;
    if (!TryMaterializeDartObject(&arguments[i], &raw_argument, error)) {
      return false;
    }
    const Object& argument = Object::Handle(zone, raw_argument);
    dart_arguments.SetAt(i + 1, argument, thread);
  }

  ScopedSuppressPatchInvocation suppress_patch_invocation;
  const Object& result = Object::Handle(
      zone, DartEntry::InvokeFunction(target, dart_arguments, descriptor));
  if (result.IsError()) {
    if (exception != nullptr) {
      *exception = Value::FromDart(result.ptr());
    }
    return SetErrorMessage(error, Error::Cast(result).ToErrorCString());
  }
  *out = Value::FromDart(result.ptr());
  return true;
}

bool ResolveOriginalFunction(const std::string& function_id,
                             Function* out,
                             std::string* error) {
  const std::size_t separator = function_id.rfind("::");
  if (separator == std::string::npos || separator == 0 ||
      separator + 2 >= function_id.size()) {
    return SetErrorMessage(error,
                           "function id must be libraryUri::functionName");
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  const std::string library_uri = function_id.substr(0, separator);
  std::string function_name = function_id.substr(separator + 2);
  const std::size_t signature_start = function_name.find('(');
  if (signature_start != std::string::npos) {
    function_name = function_name.substr(0, signature_start);
  }
  if (function_name.empty()) {
    return SetErrorMessage(error, "function id has an empty function name");
  }

  const String& url =
      String::Handle(zone, Symbols::New(thread, library_uri.c_str()));
  const Library& library =
      Library::Handle(zone, Library::LookupLibrary(thread, url));
  if (library.IsNull()) {
    return SetErrorMessage(error, "library not found");
  }

  if (function_name.rfind("class:", 0) == 0) {
    std::string class_and_function = function_name.substr(6);
    const std::size_t dot = class_and_function.find('.');
    if (dot == std::string::npos || dot == 0) {
      return SetErrorMessage(error,
                             "class function id must include ClassName.name");
    }
    const std::string class_name = class_and_function.substr(0, dot);
    const std::string function_suffix = class_and_function.substr(dot);
    const String& class_symbol =
        String::Handle(zone, Symbols::New(thread, class_name.c_str()));
    const Class& cls =
        Class::Handle(zone, library.LookupClassAllowPrivate(class_symbol));
    if (cls.IsNull()) {
      return SetErrorMessage(error, "function class not found");
    }
    const Error& finalize_error =
        Error::Handle(zone, cls.EnsureIsFinalized(thread));
    if (!finalize_error.IsNull()) {
      return SetErrorMessage(error, finalize_error.ToErrorCString());
    }
    const std::string method_name = class_and_function.substr(dot + 1);
    const String& method_symbol =
        String::Handle(zone, Symbols::New(thread, method_name.c_str()));
    *out = cls.LookupStaticFunctionAllowPrivate(method_symbol);
    if (out->IsNull()) {
      const std::string full_function_name = class_name + function_suffix;
      const String& full_function_symbol = String::Handle(
          zone, Symbols::New(thread, full_function_name.c_str()));
      *out = cls.LookupStaticFunctionAllowPrivate(full_function_symbol);
      if (out->IsNull()) {
        return SetErrorMessage(error, "class function not found");
      }
    }
    return true;
  }

  const String& name =
      String::Handle(zone, Symbols::New(thread, function_name.c_str()));
  *out = library.LookupFunctionAllowPrivate(name);
  if (out->IsNull()) {
    return SetErrorMessage(error, "function not found");
  }
  return true;
}

bool ResolveConstructor(const std::string& constructor_id,
                        Class* out_class,
                        Function* out_constructor,
                        std::string* error) {
  const std::size_t separator = constructor_id.rfind("::");
  if (separator == std::string::npos || separator == 0 ||
      separator + 2 >= constructor_id.size()) {
    return SetErrorMessage(
        error, "constructor id must be libraryUri::class:ClassName.name");
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  const std::string library_uri = constructor_id.substr(0, separator);
  std::string class_and_constructor = constructor_id.substr(separator + 2);
  if (class_and_constructor.rfind("class:", 0) == 0) {
    class_and_constructor = class_and_constructor.substr(6);
  }
  const std::size_t dot = class_and_constructor.find('.');
  if (dot == std::string::npos || dot == 0) {
    return SetErrorMessage(error, "constructor id must include ClassName.");
  }
  const std::string class_name = class_and_constructor.substr(0, dot);
  const std::string constructor_suffix = class_and_constructor.substr(dot);

  const String& url =
      String::Handle(zone, Symbols::New(thread, library_uri.c_str()));
  const Library& library =
      Library::Handle(zone, Library::LookupLibrary(thread, url));
  if (library.IsNull()) {
    return SetErrorMessage(error, "constructor library not found");
  }
  const String& class_symbol =
      String::Handle(zone, Symbols::New(thread, class_name.c_str()));
  *out_class = library.LookupClassAllowPrivate(class_symbol);
  if (out_class->IsNull()) {
    return SetErrorMessage(error, "constructor class not found");
  }
  const Error& finalize_error =
      Error::Handle(zone, out_class->EnsureIsFinalized(thread));
  if (!finalize_error.IsNull()) {
    return SetErrorMessage(error, finalize_error.ToErrorCString());
  }

  const std::string full_constructor_name = class_name + constructor_suffix;
  const String& constructor_symbol =
      String::Handle(zone, Symbols::New(thread, full_constructor_name.c_str()));
  *out_constructor = out_class->LookupFunctionAllowPrivate(constructor_symbol);
  if (out_constructor->IsNull()) {
    return SetErrorMessage(error, "constructor not found");
  }
  if (!out_constructor->IsGenerativeConstructor() &&
      !out_constructor->IsFactory()) {
    return SetErrorMessage(error, "constructor target is not a constructor");
  }
  return true;
}

bool ResolveTypeClass(const std::string& type_name,
                      Class* out_class,
                      std::string* error) {
  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  std::string library_uri = "dart:core";
  std::string class_name = type_name;
  const std::size_t separator = type_name.find("::");
  if (separator != std::string::npos) {
    if (separator == 0 || separator + 2 >= type_name.size()) {
      return SetErrorMessage(error, "unsupported type " + type_name);
    }
    library_uri = type_name.substr(0, separator);
    class_name = type_name.substr(separator + 2);
    if (class_name.rfind("class:", 0) == 0) {
      class_name = class_name.substr(6);
    }
  }
  if (class_name.empty()) {
    return SetErrorMessage(error, "type id has an empty class name");
  }

  const String& url =
      String::Handle(zone, Symbols::New(thread, library_uri.c_str()));
  const Library& library =
      Library::Handle(zone, Library::LookupLibrary(thread, url));
  if (library.IsNull()) {
    return SetErrorMessage(error, "type library not found");
  }
  const String& name =
      String::Handle(zone, Symbols::New(thread, class_name.c_str()));
  *out_class = library.LookupClassAllowPrivate(name);
  if (out_class->IsNull()) {
    return SetErrorMessage(error, "type class not found");
  }
  const Error& finalize_error =
      Error::Handle(zone, out_class->EnsureIsFinalized(thread));
  if (!finalize_error.IsNull()) {
    return SetErrorMessage(error, finalize_error.ToErrorCString());
  }
  return true;
}

bool ResolveTypeArguments(const std::vector<std::string>& type_names,
                          TypeArguments* out,
                          std::string* error);

bool ResolveType(const std::string& raw_type_name,
                 AbstractType* out,
                 std::string* error) {
  std::string type_name;
  std::vector<std::string> type_argument_names;
  if (!ParseTypeName(raw_type_name, &type_name, &type_argument_names, error)) {
    return false;
  }
  if (!type_argument_names.empty() &&
      (type_name == "dynamic" || type_name == "void")) {
    return SetErrorMessage(error, "type argument count mismatch");
  }

  if (type_name == "dynamic") {
    *out = Type::DynamicType();
  } else if (type_name == "void") {
    *out = Type::VoidType();
  } else if (type_name == "Object" && type_argument_names.empty()) {
    *out = Type::ObjectType();
  } else if (type_name == "Null" && type_argument_names.empty()) {
    *out = Type::NullType();
  } else if (type_name == "bool" && type_argument_names.empty()) {
    *out = Type::BoolType();
  } else if (type_name == "int" && type_argument_names.empty()) {
    *out = Type::IntType();
  } else if (type_name == "double" && type_argument_names.empty()) {
    *out = Type::Double();
  } else if (type_name == "num" && type_argument_names.empty()) {
    *out = Type::Number();
  } else if (type_name == "String" && type_argument_names.empty()) {
    *out = Type::StringType();
  } else if (type_name == "List" && type_argument_names.empty()) {
    *out = Type::ArrayType();
  } else {
    Thread* thread = Thread::Current();
    Zone* zone = thread->zone();
    Class& cls = Class::Handle(zone);
    if (!ResolveTypeClass(type_name, &cls, error)) {
      return false;
    }
    if (type_argument_names.empty()) {
      *out = cls.DeclarationType();
    } else {
      const intptr_t expected_count = cls.NumTypeArguments();
      if (expected_count != static_cast<intptr_t>(type_argument_names.size())) {
        return SetErrorMessage(error, "type argument count mismatch");
      }
      TypeArguments& type_arguments = TypeArguments::Handle(zone);
      if (!ResolveTypeArguments(type_argument_names, &type_arguments, error)) {
        return false;
      }
      AbstractType& type = AbstractType::Handle(
          zone, Type::New(cls, type_arguments, Nullability::kNonNullable));
      type ^= ClassFinalizer::FinalizeType(type);
      *out = type.ptr();
    }
  }
  ASSERT(!out->IsNull());
  ASSERT(out->IsFinalized());
  return true;
}

bool ResolveTypeArguments(const std::vector<std::string>& type_names,
                          TypeArguments* out,
                          std::string* error) {
  if (type_names.empty()) {
    *out = Object::null_type_arguments().ptr();
    return true;
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  const TypeArguments& type_arguments =
      TypeArguments::Handle(zone, TypeArguments::New(type_names.size()));
  for (intptr_t i = 0; i < static_cast<intptr_t>(type_names.size()); i++) {
    AbstractType& type = AbstractType::Handle(zone);
    if (!ResolveType(type_names[i], &type, error)) {
      return false;
    }
    type_arguments.SetTypeAt(i, type);
  }
  *out ^= type_arguments.Canonicalize(thread);
  return true;
}

bool DartIsType(Value value,
                const std::string& type_name,
                bool* out,
                std::string* error) {
  if (!HasCurrentDartThread()) {
    return SetErrorMessage(error, "IsType requires a Dart VM runtime");
  }
  if (type_name == "dynamic" || type_name == "void") {
    *out = true;
    return true;
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  ObjectPtr raw_value = nullptr;
  if (!TryMaterializeDartObject(&value, &raw_value, error)) {
    return false;
  }
  const Object& object = Object::Handle(zone, raw_value);
  if (!object.IsInstance()) {
    return SetErrorMessage(error, "value is not a Dart instance");
  }
  const Instance& instance = Instance::Cast(object);
  AbstractType& type = AbstractType::Handle(zone);
  if (!ResolveType(type_name, &type, error)) {
    return false;
  }
  *out = instance.IsInstanceOf(type, Object::null_type_arguments(),
                               Object::null_type_arguments());
  return true;
}

bool DartCallOriginal(const std::string& function_id,
                      std::vector<Value> arguments,
                      Value* out,
                      std::string* error,
                      Value* exception) {
  if (!HasCurrentDartThread()) {
    return SetErrorMessage(error, "CallOriginal requires a Dart VM runtime");
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  std::vector<std::string> named_argument_names;
  std::vector<std::string> type_argument_names;
  const std::string function_without_named =
      StripNamedArguments(function_id, &named_argument_names);
  std::string resolved_function_id;
  if (!StripConstructorTypeArguments(function_without_named,
                                     &type_argument_names,
                                     &resolved_function_id, error)) {
    return false;
  }
  if (named_argument_names.size() > arguments.size()) {
    return SetErrorMessage(error, "too many named function arguments");
  }
  Function& target = Function::Handle(zone);
  if (!ResolveOriginalFunction(resolved_function_id, &target, error)) {
    return false;
  }
  TypeArguments& type_arguments = TypeArguments::Handle(zone);
  if (!ResolveTypeArguments(type_argument_names, &type_arguments, error)) {
    return false;
  }
  const intptr_t type_argument_count =
      static_cast<intptr_t>(type_argument_names.size());

  const intptr_t argument_count = static_cast<intptr_t>(arguments.size());
  const intptr_t named_argument_count =
      static_cast<intptr_t>(named_argument_names.size());
  Array& argument_names = Array::Handle(zone);
  if (named_argument_count > 0) {
    argument_names = Array::New(named_argument_count);
    for (intptr_t i = 0; i < named_argument_count; i++) {
      const String& name = String::Handle(
          zone, Symbols::New(thread, named_argument_names[i].c_str()));
      argument_names.SetAt(i, name, thread);
    }
  }
  const Array& descriptor = Array::Handle(
      zone,
      named_argument_count == 0
          ? ArgumentsDescriptor::NewBoxed(type_argument_count, argument_count)
          : ArgumentsDescriptor::NewBoxed(type_argument_count, argument_count,
                                          argument_names));
  const ArgumentsDescriptor args_descriptor(descriptor);
  String& argument_count_error = String::Handle(zone);
  if (!target.AreValidArgumentCounts(type_argument_count, argument_count,
                                     named_argument_count,
                                     &argument_count_error)) {
    std::string message = "wrong function argument count";
    if (!argument_count_error.IsNull()) {
      message += ": ";
      message += argument_count_error.ToCString();
    }
    return SetErrorMessage(error, message);
  }

  const intptr_t first_argument_index = type_argument_count > 0 ? 1 : 0;
  const Array& dart_arguments =
      Array::Handle(zone, Array::New(argument_count + first_argument_index));
  if (type_argument_count > 0) {
    dart_arguments.SetAt(0, type_arguments, thread);
  }
  for (intptr_t i = 0; i < argument_count; i++) {
    ObjectPtr raw_argument = nullptr;
    if (!TryMaterializeDartObject(&arguments[i], &raw_argument, error)) {
      return false;
    }
    const Object& argument = Object::Handle(zone, raw_argument);
    dart_arguments.SetAt(i + first_argument_index, argument, thread);
  }

  ObjectPtr type_error =
      target.DoArgumentTypesMatch(dart_arguments, args_descriptor);
  if (type_error != Error::null()) {
    const Object& type_error_object = Object::Handle(zone, type_error);
    if (type_error_object.IsError()) {
      if (exception != nullptr) {
        *exception = Value::FromDart(type_error);
      }
      return SetErrorMessage(error,
                             Error::Cast(type_error_object).ToErrorCString());
    }
    return SetErrorMessage(error, "function argument type mismatch");
  }

  ScopedSuppressPatchInvocation suppress_patch_invocation;
  const Object& result = Object::Handle(
      zone, DartEntry::InvokeFunction(target, dart_arguments, descriptor));
  if (result.IsError()) {
    if (exception != nullptr) {
      *exception = Value::FromDart(result.ptr());
    }
    return SetErrorMessage(error, Error::Cast(result).ToErrorCString());
  }
  *out = Value::FromDart(result.ptr());
  return true;
}

bool DartMakeClosure(const std::string& function_id,
                     Value* out,
                     std::string* error) {
  if (!HasCurrentDartThread()) {
    return SetErrorMessage(error, "MakeClosure requires a Dart VM runtime");
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  Function& target = Function::Handle(zone);
  if (!ResolveOriginalFunction(function_id, &target, error)) {
    return false;
  }
  const Function& closure_function =
      Function::Handle(zone, target.ImplicitClosureFunction());
  if (closure_function.IsNull()) {
    return SetErrorMessage(error, "implicit closure function not found");
  }
  const Closure& closure =
      Closure::Handle(zone, closure_function.ImplicitStaticClosure());
  if (closure.IsNull()) {
    return SetErrorMessage(error, "implicit static closure not found");
  }
  *out = Value::FromDart(closure.ptr());
  return true;
}

bool DartNewObject(const std::string& constructor_id,
                   std::vector<Value> arguments,
                   Value* out,
                   std::string* error,
                   Value* exception) {
  if (!HasCurrentDartThread()) {
    return SetErrorMessage(error, "NewObject requires a Dart VM runtime");
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  std::vector<std::string> named_argument_names;
  std::vector<std::string> type_argument_names;
  const std::string constructor_without_named =
      StripNamedArguments(constructor_id, &named_argument_names);
  std::string resolved_constructor_id;
  if (!StripConstructorTypeArguments(constructor_without_named,
                                     &type_argument_names,
                                     &resolved_constructor_id, error)) {
    return false;
  }
  if (named_argument_names.size() > arguments.size()) {
    return SetErrorMessage(error, "too many named constructor arguments");
  }
  Class& cls = Class::Handle(zone);
  Function& constructor = Function::Handle(zone);
  if (!ResolveConstructor(resolved_constructor_id, &cls, &constructor, error)) {
    return false;
  }
  TypeArguments& type_arguments = TypeArguments::Handle(zone);
  if (!ResolveTypeArguments(type_argument_names, &type_arguments, error)) {
    return false;
  }
  const intptr_t type_argument_count =
      static_cast<intptr_t>(type_argument_names.size());

  const intptr_t argument_count = static_cast<intptr_t>(arguments.size()) + 1;
  const intptr_t named_argument_count =
      static_cast<intptr_t>(named_argument_names.size());
  Array& argument_names = Array::Handle(zone);
  if (named_argument_count > 0) {
    argument_names = Array::New(named_argument_count);
    for (intptr_t i = 0; i < named_argument_count; i++) {
      const String& name = String::Handle(
          zone, Symbols::New(thread, named_argument_names[i].c_str()));
      argument_names.SetAt(i, name, thread);
    }
  }
  const Array& descriptor = Array::Handle(
      zone,
      named_argument_count == 0
          ? ArgumentsDescriptor::NewBoxed(/*type_args_len=*/0, argument_count)
          : ArgumentsDescriptor::NewBoxed(
                /*type_args_len=*/0, argument_count, argument_names));
  ArgumentsDescriptor args_descriptor(descriptor);
  String& argument_count_error = String::Handle(zone);
  if (!constructor.AreValidArgumentCounts(/*num_type_arguments=*/0,
                                          argument_count, named_argument_count,
                                          &argument_count_error)) {
    std::string message = "wrong constructor argument count";
    if (!argument_count_error.IsNull()) {
      message += ": ";
      message += argument_count_error.ToCString();
    }
    return SetErrorMessage(error, message);
  }

  Instance& instance = Instance::Handle(zone);
  if (constructor.IsGenerativeConstructor()) {
    instance = Instance::New(cls);
    if (type_argument_count > 0) {
      instance.SetTypeArguments(type_arguments);
    }
  }
  const Array& dart_arguments = Array::Handle(zone, Array::New(argument_count));
  if (constructor.IsGenerativeConstructor()) {
    dart_arguments.SetAt(0, instance, thread);
  } else {
    dart_arguments.SetAt(0, type_arguments, thread);
  }
  for (intptr_t i = 0; i < static_cast<intptr_t>(arguments.size()); i++) {
    ObjectPtr raw_argument = nullptr;
    if (!TryMaterializeDartObject(&arguments[i], &raw_argument, error)) {
      return false;
    }
    const Object& argument = Object::Handle(zone, raw_argument);
    dart_arguments.SetAt(i + 1, argument, thread);
  }

  ObjectPtr type_error = constructor.DoArgumentTypesMatch(
      dart_arguments, args_descriptor, type_arguments,
      Object::empty_type_arguments());
  if (type_error != Error::null()) {
    const Object& type_error_object = Object::Handle(zone, type_error);
    if (type_error_object.IsError()) {
      if (exception != nullptr) {
        *exception = Value::FromDart(type_error);
      }
      return SetErrorMessage(error,
                             Error::Cast(type_error_object).ToErrorCString());
    }
    return SetErrorMessage(error, "constructor argument type mismatch");
  }

  ScopedSuppressPatchInvocation suppress_patch_invocation;
  const Object& result = Object::Handle(
      zone, DartEntry::InvokeFunction(constructor, dart_arguments, descriptor));
  if (result.IsError()) {
    if (exception != nullptr) {
      *exception = Value::FromDart(result.ptr());
    }
    return SetErrorMessage(error, Error::Cast(result).ToErrorCString());
  }
  if (constructor.IsFactory()) {
    if (!result.IsNull() && !result.IsInstance()) {
      return SetErrorMessage(error,
                             "factory constructor returned non-instance");
    }
    instance ^= result.ptr();
  }
  *out = Value::FromDart(instance.ptr());
  return true;
}

bool DartCallClosure(Value closure,
                     const std::string& call_metadata,
                     std::vector<Value> arguments,
                     Value* out,
                     std::string* error,
                     Value* exception) {
  if (!HasCurrentDartThread()) {
    return SetErrorMessage(error, "CallClosure requires a Dart VM runtime");
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  ObjectPtr raw_closure = nullptr;
  if (!TryMaterializeDartObject(&closure, &raw_closure, error)) {
    return false;
  }
  const Object& closure_object = Object::Handle(zone, raw_closure);
  if (!closure_object.IsInstance()) {
    return SetErrorMessage(error, "closure is not a Dart instance");
  }

  std::vector<std::string> named_argument_names;
  StripNamedArguments(call_metadata, &named_argument_names);
  if (named_argument_names.size() > arguments.size()) {
    return SetErrorMessage(error, "too many named closure arguments");
  }
  const intptr_t argument_count = static_cast<intptr_t>(arguments.size()) + 1;
  const intptr_t named_argument_count =
      static_cast<intptr_t>(named_argument_names.size());
  Array& argument_names = Array::Handle(zone);
  if (named_argument_count > 0) {
    argument_names = Array::New(named_argument_count);
    for (intptr_t i = 0; i < named_argument_count; i++) {
      const String& argument_name = String::Handle(
          zone, Symbols::New(thread, named_argument_names[i].c_str()));
      argument_names.SetAt(i, argument_name, thread);
    }
  }
  const Array& descriptor = Array::Handle(
      zone,
      named_argument_count == 0
          ? ArgumentsDescriptor::NewBoxed(/*type_args_len=*/0, argument_count)
          : ArgumentsDescriptor::NewBoxed(
                /*type_args_len=*/0, argument_count, argument_names));
  const Array& dart_arguments = Array::Handle(zone, Array::New(argument_count));
  dart_arguments.SetAt(0, closure_object, thread);
  for (intptr_t i = 0; i < static_cast<intptr_t>(arguments.size()); i++) {
    ObjectPtr raw_argument = nullptr;
    if (!TryMaterializeDartObject(&arguments[i], &raw_argument, error)) {
      return false;
    }
    const Object& argument = Object::Handle(zone, raw_argument);
    dart_arguments.SetAt(i + 1, argument, thread);
  }

  ScopedSuppressPatchInvocation suppress_patch_invocation;
  const Object& result = Object::Handle(
      zone, DartEntry::InvokeClosure(thread, dart_arguments, descriptor));
  if (result.IsError()) {
    if (exception != nullptr) {
      *exception = Value::FromDart(result.ptr());
    }
    return SetErrorMessage(error, Error::Cast(result).ToErrorCString());
  }
  *out = Value::FromDart(result.ptr());
  return true;
}
#else
void RecordPatchStackTraceLocation(const BytecodeFunction& function,
                                   uint32_t bytecode_offset) {
  (void)function;
  (void)bytecode_offset;
}

const char* LastPatchStackTraceLocation() {
  return nullptr;
}

std::size_t PatchStackTraceLocationCount() {
  return 0;
}

const char* PatchStackTraceLocationAt(std::size_t index) {
  (void)index;
  return nullptr;
}

PatchStackTraceFrameInfo PatchStackTraceFrameInfoAt(std::size_t index) {
  (void)index;
  return PatchStackTraceFrameInfo{
      nullptr, nullptr, 0, nullptr, nullptr, 0, 0, nullptr, 0, 0, 0, 0};
}

void ClearPatchStackTraceLocation() {}

void PushActivePatchFrame(const BytecodeFunction& function,
                          uint32_t bytecode_offset,
                          std::size_t captured_argument_count,
                          const std::vector<Value>* arguments,
                          const std::vector<Value>* locals,
                          std::size_t active_handler_count,
                          uint32_t innermost_handler_offset,
                          uint32_t innermost_handler_end_offset) {
  (void)function;
  (void)bytecode_offset;
  (void)captured_argument_count;
  (void)arguments;
  (void)locals;
  (void)active_handler_count;
  (void)innermost_handler_offset;
  (void)innermost_handler_end_offset;
}

void UpdateActivePatchFrame(const BytecodeFunction& function,
                            uint32_t bytecode_offset,
                            std::size_t captured_argument_count,
                            const std::vector<Value>* arguments,
                            const std::vector<Value>* locals,
                            std::size_t active_handler_count,
                            uint32_t innermost_handler_offset,
                            uint32_t innermost_handler_end_offset) {
  (void)function;
  (void)bytecode_offset;
  (void)captured_argument_count;
  (void)arguments;
  (void)locals;
  (void)active_handler_count;
  (void)innermost_handler_offset;
  (void)innermost_handler_end_offset;
}

void PopActivePatchFrame() {}

std::size_t ActivePatchFrameCount() {
  return 0;
}

PatchStackTraceFrameInfo ActivePatchFrameInfoAt(std::size_t index) {
  (void)index;
  return PatchStackTraceFrameInfo{
      nullptr, nullptr, 0, nullptr, nullptr, 0, 0, nullptr, 0, 0, 0, 0};
}

void SetActivePatchFrameUpdateCallback(
    ActivePatchFrameUpdateCallback callback) {
  (void)callback;
}

ScopedActivePatchFrame::ScopedActivePatchFrame(
    const BytecodeFunction& function,
    const std::vector<Value>& arguments,
    const std::vector<Value>& locals,
    std::size_t captured_argument_count)
    : function_(function),
      arguments_(arguments),
      locals_(locals),
      captured_argument_count_(captured_argument_count) {
  PushActivePatchFrame(function_, 0, captured_argument_count_, &arguments_,
                       &locals_);
}

ScopedActivePatchFrame::~ScopedActivePatchFrame() {
  PopActivePatchFrame();
}

ObjectPtr MaterializeDartObject(Value* value) {
  return value->object_value;
}

bool ContainsBytecodeClosure(const Value& value) {
  if (value.kind == ValueKind::kBytecodeClosure) {
    return true;
  }
  for (const Value& element : value.list_value) {
    if (ContainsBytecodeClosure(element)) {
      return true;
    }
  }
  for (const Value& entry : value.map_entries) {
    if (ContainsBytecodeClosure(entry)) {
      return true;
    }
  }
  return false;
}

bool TryMaterializeDartObject(Value* value,
                              ObjectPtr* out,
                              std::string* error) {
  if (out == nullptr) {
    return false;
  }
  if (value->kind == ValueKind::kBytecodeClosure) {
    return TryMaterializeBytecodeClosure(value, out, error);
  }
  *out = MaterializeDartObject(value);
  return true;
}

void PopulateScalarFromDartObject(Value* value) {}

bool DartInstanceGetField(const Value& receiver,
                          const std::string& field_name,
                          Value* out,
                          std::string* error) {
  (void)receiver;
  (void)field_name;
  (void)out;
  (void)error;
  return false;
}

bool DartInstanceSetField(Value* receiver,
                          const std::string& field_name,
                          Value value,
                          std::string* error) {
  (void)receiver;
  (void)field_name;
  (void)value;
  (void)error;
  return false;
}

bool DartInstanceCallDynamic(Value receiver,
                             const std::string& method_name,
                             std::vector<Value> arguments,
                             Value* out,
                             std::string* error,
                             Value* exception) {
  (void)receiver;
  (void)method_name;
  (void)arguments;
  (void)out;
  (void)exception;
  return SetErrorMessage(error, "CallDynamic requires a Dart VM runtime");
}

bool DartCallOriginal(const std::string& function_id,
                      std::vector<Value> arguments,
                      Value* out,
                      std::string* error,
                      Value* exception) {
  (void)function_id;
  (void)arguments;
  (void)out;
  (void)exception;
  return SetErrorMessage(error, "CallOriginal requires a Dart VM runtime");
}

bool DartMakeClosure(const std::string& function_id,
                     Value* out,
                     std::string* error) {
  (void)function_id;
  (void)out;
  return SetErrorMessage(error, "MakeClosure requires a Dart VM runtime");
}

bool DartNewObject(const std::string& constructor_id,
                   std::vector<Value> arguments,
                   Value* out,
                   std::string* error,
                   Value* exception) {
  (void)constructor_id;
  (void)arguments;
  (void)out;
  (void)exception;
  return SetErrorMessage(error, "NewObject requires a Dart VM runtime");
}

bool DartCallClosure(Value closure,
                     const std::string& call_metadata,
                     std::vector<Value> arguments,
                     Value* out,
                     std::string* error,
                     Value* exception) {
  (void)closure;
  (void)call_metadata;
  (void)arguments;
  (void)out;
  (void)exception;
  return SetErrorMessage(error, "CallClosure requires a Dart VM runtime");
}

bool DartIsType(Value value,
                const std::string& type_name,
                bool* out,
                std::string* error) {
  (void)value;
  (void)type_name;
  (void)out;
  return SetErrorMessage(error, "IsType requires a Dart VM runtime");
}
#endif

}  // namespace internal
}  // namespace fcb
}  // namespace dart
