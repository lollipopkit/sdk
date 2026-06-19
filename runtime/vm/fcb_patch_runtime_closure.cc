// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime_internal.h"

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
#include <cstring>

#include "vm/class_finalizer.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/symbols.h"
#include "vm/thread.h"
#endif

namespace dart {
namespace fcb {
namespace internal {
namespace {

bool SetErrorMessage(std::string* error, const std::string& message);

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
constexpr char kFcbBytecodeClosurePrefix[] = "FCBBytecodeClosure::";

bool IsOptionalNamedParameter(const std::string& name) {
  return !name.empty() && name[0] == '?';
}

const char* DartParameterName(const std::string& name) {
  return IsOptionalNamedParameter(name) ? name.c_str() + 1 : name.c_str();
}

bool ConfigureTrampolineSignature(Thread* thread,
                                  intptr_t parameter_count,
                                  intptr_t optional_positional_count,
                                  intptr_t type_parameter_count,
                                  const std::vector<std::string>& named_names,
                                  Function* function,
                                  std::string* error) {
  const intptr_t named_count = static_cast<intptr_t>(named_names.size());
  if (optional_positional_count < 0) {
    return SetErrorMessage(error,
                           "bytecode closure optional positional count is "
                           "negative");
  }
  if (optional_positional_count > 0 && named_count > 0) {
    return SetErrorMessage(error,
                           "bytecode closure cannot mix optional positional "
                           "and named parameters");
  }
  if (type_parameter_count < 0) {
    return SetErrorMessage(error,
                           "bytecode closure type parameter count is negative");
  }
  if (named_count > parameter_count) {
    return SetErrorMessage(error,
                           "bytecode closure named parameter count exceeds "
                           "exposed parameter count");
  }
  if (optional_positional_count > parameter_count) {
    return SetErrorMessage(error,
                           "bytecode closure optional positional count "
                           "exceeds exposed parameter count");
  }
  Zone* zone = thread->zone();
  FunctionType& signature = FunctionType::Handle(zone, function->signature());
  if (type_parameter_count > 0) {
    const auto& type_parameters =
        TypeParameters::Handle(zone, TypeParameters::New(type_parameter_count));
    auto& bound = Type::Handle(
        zone, IsolateGroup::Current()->object_store()->nullable_object_type());
    for (intptr_t i = 0; i < type_parameter_count; i++) {
      type_parameters.SetNameAt(i, Symbols::OptimizedOut());
      type_parameters.SetBoundAt(i, bound);
      type_parameters.SetDefaultAt(i, Object::dynamic_type());
    }
    signature.SetTypeParameters(type_parameters);
  }
  const intptr_t total_parameter_count = parameter_count + 1;
  const intptr_t fixed_parameter_count =
      total_parameter_count - named_count - optional_positional_count;
  signature.set_num_fixed_parameters(fixed_parameter_count);
  signature.SetNumOptionalParameters(
      named_count > 0 ? named_count : optional_positional_count,
      named_count == 0);
  signature.set_parameter_types(
      Array::Handle(zone, Array::New(total_parameter_count, Heap::kOld)));
  for (intptr_t i = 0; i < total_parameter_count; i++) {
    signature.SetParameterTypeAt(i, Object::dynamic_type());
  }
  if (named_count > 0) {
    signature.CreateNameArrayIncludingFlags();
    for (intptr_t i = 0; i < named_count; i++) {
      const intptr_t parameter_index = fixed_parameter_count + i;
      const String& name =
          String::Handle(zone,
                         Symbols::New(thread,
                                      DartParameterName(named_names[i])));
      signature.SetParameterNameAt(parameter_index, name);
      if (!IsOptionalNamedParameter(named_names[i])) {
        signature.SetIsRequiredAt(parameter_index);
      }
    }
    signature.FinalizeNameArray();
  }
  signature.set_result_type(Object::dynamic_type());
  signature ^= ClassFinalizer::FinalizeType(signature);
  if (signature.IsNull()) {
    return SetErrorMessage(error,
                           "failed to finalize bytecode closure signature");
  }
  function->SetSignature(signature);
  return true;
}

bool TryMaterializeCapture(Value* capture,
                           ObjectPtr* out,
                           std::string* error) {
  if (!TryMaterializeDartObject(capture, out, error)) {
    return false;
  }
  if (*out == nullptr) {
    return SetErrorMessage(error, "capture materialized to null ObjectPtr");
  }
  return true;
}
#endif

bool SetErrorMessage(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

bool TryMaterializeBytecodeClosure(Value* value,
                                   ObjectPtr* out,
                                   std::string* error) {
  if (out == nullptr) {
    return false;
  }
  if (value == nullptr || value->kind != ValueKind::kBytecodeClosure) {
    return SetErrorMessage(error, "value is not a bytecode closure");
  }
#if defined(FCB_PATCH_RUNTIME_STANDALONE)
  *out = nullptr;
  return SetErrorMessage(
      error,
      "bytecode closure cannot be materialized as Dart _Closure yet: "
      "VM trampoline is not installed");
#else
  Thread* thread = Thread::Current();
  if (thread == nullptr || !thread->IsDartMutatorThread()) {
    *out = nullptr;
    return SetErrorMessage(error,
                           "bytecode closure materialization requires a Dart "
                           "mutator thread");
  }
  IsolateGroup* isolate_group = thread->isolate_group();
  if (isolate_group == nullptr ||
      isolate_group->fcb_patch_runtime() == nullptr) {
    *out = nullptr;
    return SetErrorMessage(error,
                           "bytecode closure materialization requires an "
                           "installed FCB patch runtime");
  }

  const DispatchDecision decision =
      isolate_group->fcb_patch_runtime()->Resolve(value->closure_function_id);
  if (decision.state != PatchState::kPatchedInterpreted ||
      decision.function == nullptr) {
    *out = nullptr;
    return SetErrorMessage(error,
                           "bytecode closure target is not installed: " +
                               value->closure_function_id);
  }
  if (decision.function->parameter_count <
      static_cast<intptr_t>(value->closure_captures.size())) {
    *out = nullptr;
    return SetErrorMessage(error,
                           "bytecode closure capture count exceeds target "
                           "parameter count");
  }
  const intptr_t exposed_parameter_count =
      static_cast<intptr_t>(decision.function->parameter_count) -
      static_cast<intptr_t>(value->closure_captures.size());

  Zone* zone = thread->zone();
  FunctionType& signature = FunctionType::Handle(zone, FunctionType::New());

  const Class& owner = Class::Handle(
      zone, thread->isolate_group()->object_store()->closure_class());
  if (owner.IsNull()) {
    *out = nullptr;
    return SetErrorMessage(error, "closure class is unavailable");
  }
  const std::string trampoline_name =
      std::string(kFcbBytecodeClosurePrefix) + value->closure_function_id;
  const String& name =
      String::Handle(zone, Symbols::New(thread, trampoline_name.c_str()));
  const Function& function = Function::Handle(
      zone, Function::New(signature, name, UntaggedFunction::kClosureFunction,
                          /*is_static=*/false, /*is_const=*/false,
                          /*is_abstract=*/false, /*is_external=*/false,
                          /*is_native=*/false, owner,
                          TokenPosition::kMinSource));
  Function& mutable_function = Function::Handle(zone, function.ptr());
  if (!ConfigureTrampolineSignature(thread, exposed_parameter_count,
                                    value->closure_optional_positional_count,
                                    value->closure_type_parameter_count,
                                    value->closure_named_parameters,
                                    &mutable_function, error)) {
    *out = nullptr;
    return false;
  }
  function.set_is_debuggable(false);
  function.set_is_visible(false);
  function.set_is_reflectable(false);

  const Context& context = Context::Handle(
      zone, Context::New(value->closure_captures.size(), Heap::kOld));
  for (intptr_t i = 0;
       i < static_cast<intptr_t>(value->closure_captures.size()); i++) {
    ObjectPtr raw_capture = Object::null();
    if (!TryMaterializeCapture(&value->closure_captures[i], &raw_capture,
                               error)) {
      *out = nullptr;
      return false;
    }
    context.SetAt(i, Object::Handle(zone, raw_capture));
  }

  const Closure& closure =
      Closure::Handle(zone,
                      Closure::New(Object::null_type_arguments(),
                                   Object::null_type_arguments(), function,
                                   context, Heap::kOld));
  *out = closure.ptr();
  value->object_value = closure.ptr();
  return true;
#endif
}

#if !defined(FCB_PATCH_RUNTIME_STANDALONE)
bool IsFcbBytecodeClosureTrampoline(const Function& function) {
  if (function.IsNull() || !function.IsClosureFunction()) {
    return false;
  }
  const char* name = String::Handle(function.name()).ToCString();
  return name != nullptr &&
         std::strncmp(name, kFcbBytecodeClosurePrefix,
                      sizeof(kFcbBytecodeClosurePrefix) - 1) == 0;
}

std::string FcbBytecodeClosureTargetId(const Function& function) {
  if (!IsFcbBytecodeClosureTrampoline(function)) {
    return {};
  }
  const char* name = String::Handle(function.name()).ToCString();
  return std::string(name + sizeof(kFcbBytecodeClosurePrefix) - 1);
}
#else
bool IsFcbBytecodeClosureTrampoline(const Function& function) {
  return false;
}

std::string FcbBytecodeClosureTargetId(const Function& function) {
  return {};
}
#endif

}  // namespace internal
}  // namespace fcb
}  // namespace dart
