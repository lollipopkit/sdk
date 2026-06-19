// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <utility>

#include "vm/fcb_patch_runtime_internal.h"
#include "vm/visitor.h"

namespace dart {
namespace fcb {

Value Value::Null() {
  Value out;
  internal::MaterializeDartObject(&out);
  return out;
}

Value Value::Int(int64_t value) {
  Value out;
  out.kind = ValueKind::kInt;
  out.int_value = value;
  internal::MaterializeDartObject(&out);
  return out;
}

Value Value::Double(double value) {
  Value out;
  out.kind = ValueKind::kDouble;
  out.double_value = value;
  internal::MaterializeDartObject(&out);
  return out;
}

Value Value::Bool(bool value) {
  Value out;
  out.kind = ValueKind::kBool;
  out.bool_value = value;
  internal::MaterializeDartObject(&out);
  return out;
}

Value Value::String(std::string value) {
  Value out;
  out.kind = ValueKind::kString;
  out.string_value = std::move(value);
  internal::MaterializeDartObject(&out);
  return out;
}

Value Value::List(std::vector<Value> value) {
  Value out;
  out.kind = ValueKind::kList;
  out.list_value = std::move(value);
  if (!internal::ContainsBytecodeClosure(out)) {
    internal::MaterializeDartObject(&out);
  }
  return out;
}

Value Value::Map(std::vector<Value> entries) {
  Value out;
  out.kind = ValueKind::kMap;
  out.map_entries = std::move(entries);
  if (!internal::ContainsBytecodeClosure(out)) {
    internal::MaterializeDartObject(&out);
  }
  return out;
}

Value Value::BytecodeClosure(std::string function_id,
                             std::vector<Value> captures,
                             intptr_t optional_positional_count,
                             intptr_t type_parameter_count,
                             std::vector<std::string> named_parameters) {
  Value out;
  out.kind = ValueKind::kBytecodeClosure;
  out.closure_function_id = std::move(function_id);
  out.closure_captures = std::move(captures);
  out.closure_optional_positional_count = optional_positional_count;
  out.closure_type_parameter_count = type_parameter_count;
  out.closure_named_parameters = std::move(named_parameters);
  return out;
}

Value Value::FromDart(ObjectPtr value) {
  Value out;
  out.object_value = value;
  internal::PopulateScalarFromDartObject(&out);
  return out;
}

ObjectPtr Value::ToDart() {
  return internal::MaterializeDartObject(this);
}

void Value::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  if (object_value != nullptr && object_value->IsHeapObject()) {
    visitor->VisitPointer(&object_value);
  }
  for (Value& element : list_value)
    element.VisitObjectPointers(visitor);
  for (Value& entry : map_entries)
    entry.VisitObjectPointers(visitor);
  for (Value& capture : closure_captures)
    capture.VisitObjectPointers(visitor);
}

void BytecodeFunction::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  for (Value& constant : constants)
    constant.VisitObjectPointers(visitor);
}

}  // namespace fcb
}  // namespace dart
