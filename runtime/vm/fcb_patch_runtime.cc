// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vm/fcb_patch_runtime_internal.h"
#include "vm/visitor.h"

namespace dart {
namespace fcb {
namespace {

constexpr uint32_t kMinSupportedModuleVersion = 1;
constexpr uint32_t kMaxSupportedModuleVersion = 2;
constexpr uint32_t kMaxCallStaticDepth = 64;

struct ExceptionHandler {
  uint32_t handler_offset = 0;
  uint32_t end_offset = 0;
  std::size_t stack_height = 0;
};

bool SetError(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}
bool IsTruthy(const Value& value, bool* out) {
  if (value.kind != ValueKind::kBool) {
    return false;
  }
  *out = value.bool_value;
  return true;
}
bool ValuesEqual(const Value& left, const Value& right) {
  if (left.kind != right.kind) {
    return false;
  }
  switch (left.kind) {
    case ValueKind::kNull:
      return true;
    case ValueKind::kInt:
      return left.int_value == right.int_value;
    case ValueKind::kDouble:
      return left.double_value == right.double_value;
    case ValueKind::kBool:
      return left.bool_value == right.bool_value;
    case ValueKind::kString:
      return left.string_value == right.string_value;
    case ValueKind::kList:
      if (left.list_value.size() != right.list_value.size()) return false;
      for (std::size_t i = 0; i < left.list_value.size(); i++) {
        if (!ValuesEqual(left.list_value[i], right.list_value[i])) return false;
      }
      return true;
    case ValueKind::kMap:
      if (left.map_entries.size() != right.map_entries.size()) return false;
      for (std::size_t i = 0; i < left.map_entries.size(); i++) {
        if (!ValuesEqual(left.map_entries[i], right.map_entries[i])) {
          return false;
        }
      }
      return true;
    case ValueKind::kBytecodeClosure:
      if (left.closure_function_id != right.closure_function_id ||
          left.closure_captures.size() != right.closure_captures.size() ||
          left.closure_optional_positional_count !=
              right.closure_optional_positional_count ||
          left.closure_type_parameter_count !=
              right.closure_type_parameter_count) {
        return false;
      }
      if (left.closure_named_parameters != right.closure_named_parameters) {
        return false;
      }
      for (std::size_t i = 0; i < left.closure_captures.size(); i++) {
        if (!ValuesEqual(left.closure_captures[i],
                         right.closure_captures[i])) {
          return false;
        }
      }
      return true;
  }
  return false;
}

bool FunctionsEquivalent(const BytecodeFunction& left,
                         const BytecodeFunction& right,
                         const std::vector<uint8_t>& bytecode) {
  if (left.parameter_count != right.parameter_count ||
      left.register_count != right.register_count ||
      left.return_convention != right.return_convention ||
      left.bytecode_length != right.bytecode_length ||
      left.constants.size() != right.constants.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.constants.size(); ++i) {
    if (!ValuesEqual(left.constants[i], right.constants[i])) {
      return false;
    }
  }
  for (uint32_t i = 0; i < left.bytecode_length; ++i) {
    if (bytecode[left.bytecode_offset + i] !=
        bytecode[right.bytecode_offset + i]) {
      return false;
    }
  }
  return true;
}

bool IsNumber(const Value& value) {
  return value.kind == ValueKind::kInt || value.kind == ValueKind::kDouble;
}

double NumberToDouble(const Value& value) {
  return value.kind == ValueKind::kDouble
             ? value.double_value
             : static_cast<double>(value.int_value);
}

bool ReadU16(const std::vector<uint8_t>& code,
             uint32_t base,
             uint32_t length,
             uint32_t ip,
             uint16_t* out) {
  if (ip + 1 >= length) {
    return false;
  }
  *out = static_cast<uint16_t>((code[base + ip] << 8) | code[base + ip + 1]);
  return true;
}
bool ParseBytecodeClosureTarget(const std::string& raw,
                                std::string* function_id,
                                std::size_t* capture_count,
                                intptr_t* optional_positional_count,
                                intptr_t* type_parameter_count,
                                std::vector<std::string>* named_parameters,
                                std::string* error) {
  constexpr char kMarker[] = ";captures:";
  const std::size_t marker_index = raw.find(kMarker);
  if (marker_index == std::string::npos) {
    *function_id = raw;
    *capture_count = 0;
    if (optional_positional_count != nullptr) {
      *optional_positional_count = 0;
    }
    if (type_parameter_count != nullptr) {
      *type_parameter_count = 0;
    }
    if (named_parameters != nullptr) {
      named_parameters->clear();
    }
    return true;
  }
  *function_id = raw.substr(0, marker_index);
  const std::size_t count_start = marker_index + sizeof(kMarker) - 1;
  const std::size_t next_marker = raw.find(';', count_start);
  const std::string count = raw.substr(
      count_start, next_marker == std::string::npos
                       ? std::string::npos
                       : next_marker - count_start);
  if (function_id->empty() || count.empty()) {
    return SetError(error, "malformed bytecode closure target");
  }
  std::size_t parsed = 0;
  for (char ch : count) {
    if (ch < '0' || ch > '9') {
      return SetError(error, "malformed bytecode closure capture count");
    }
    parsed = (parsed * 10) + static_cast<std::size_t>(ch - '0');
    if (parsed > 255) {
      return SetError(error, "bytecode closure captures exceed operand space");
    }
  }
  *capture_count = parsed;
  if (optional_positional_count != nullptr) {
    *optional_positional_count = 0;
  }
  if (type_parameter_count != nullptr) {
    *type_parameter_count = 0;
  }
  if (named_parameters != nullptr) {
    named_parameters->clear();
  }
  std::size_t segment_start = next_marker;
  while (segment_start != std::string::npos) {
    const std::size_t value_start = segment_start + 1;
    const std::size_t segment_end = raw.find(';', value_start);
    const std::string segment = raw.substr(
        value_start, segment_end == std::string::npos
                         ? std::string::npos
                         : segment_end - value_start);
    if (segment.rfind("named:", 0) == 0) {
      if (named_parameters != nullptr) {
        std::size_t start = 6;
        if (start >= segment.size()) {
          return SetError(error,
                          "malformed bytecode closure named parameters");
        }
        while (start <= segment.size()) {
          const std::size_t comma = segment.find(',', start);
          const std::size_t end =
              comma == std::string::npos ? segment.size() : comma;
          if (end <= start) {
            return SetError(error,
                            "malformed bytecode closure named parameters");
          }
          named_parameters->push_back(segment.substr(start, end - start));
          if (comma == std::string::npos) {
            break;
          }
          start = comma + 1;
        }
      }
    } else if (segment.rfind("optional-pos:", 0) == 0) {
      if (optional_positional_count != nullptr) {
        const std::string optional_count = segment.substr(13);
        if (optional_count.empty()) {
          return SetError(error,
                          "malformed bytecode closure optional positional "
                          "count");
        }
        intptr_t optional_parsed = 0;
        for (char ch : optional_count) {
          if (ch < '0' || ch > '9') {
            return SetError(error,
                            "malformed bytecode closure optional positional "
                            "count");
          }
          optional_parsed = (optional_parsed * 10) + (ch - '0');
          if (optional_parsed > 255) {
            return SetError(error,
                            "bytecode closure optional positional count "
                            "exceeds operand space");
          }
        }
        *optional_positional_count = optional_parsed;
      }
    } else if (segment.rfind("type-params:", 0) == 0) {
      if (type_parameter_count != nullptr) {
        const std::string type_count = segment.substr(12);
        if (type_count.empty()) {
          return SetError(error,
                          "malformed bytecode closure type parameter count");
        }
        intptr_t type_parsed = 0;
        for (char ch : type_count) {
          if (ch < '0' || ch > '9') {
            return SetError(
                error, "malformed bytecode closure type parameter count");
          }
          type_parsed = (type_parsed * 10) + (ch - '0');
          if (type_parsed > 255) {
            return SetError(
                error,
                "bytecode closure type parameter count exceeds operand space");
          }
        }
        *type_parameter_count = type_parsed;
      }
    } else {
      return SetError(error, "malformed bytecode closure target metadata");
    }
    segment_start = segment_end;
  }
  return true;
}

InterpretResult BinaryOp(uint8_t opcode, const Value& left, const Value& right) {
  if (opcode == 0x21) {
    return InterpretResult::Ok(Value::Bool(ValuesEqual(left, right)));
  }

  if (!IsNumber(left) || !IsNumber(right)) {
    return InterpretResult::Error("FCB interpreter binary op requires numbers");
  }

  if (left.kind == ValueKind::kDouble || right.kind == ValueKind::kDouble ||
      opcode == 0x13) {
    const double left_value = NumberToDouble(left);
    const double right_value = NumberToDouble(right);
    switch (opcode) {
      case 0x10:
        return InterpretResult::Ok(Value::Double(left_value + right_value));
      case 0x11:
        return InterpretResult::Ok(Value::Double(left_value - right_value));
      case 0x12:
        return InterpretResult::Ok(Value::Double(left_value * right_value));
      case 0x13:
        if (right_value == 0.0) {
          return InterpretResult::Error("FCB interpreter division by zero");
        }
        return InterpretResult::Ok(Value::Double(left_value / right_value));
      case 0x20:
        return InterpretResult::Ok(Value::Bool(left_value > right_value));
      default:
        return InterpretResult::Error("FCB interpreter unsupported binary op");
    }
  }

  switch (opcode) {
    case 0x10:
      return InterpretResult::Ok(Value::Int(left.int_value + right.int_value));
    case 0x11:
      return InterpretResult::Ok(Value::Int(left.int_value - right.int_value));
    case 0x12:
      return InterpretResult::Ok(Value::Int(left.int_value * right.int_value));
    case 0x20:
      return InterpretResult::Ok(Value::Bool(left.int_value > right.int_value));
    default:
      return InterpretResult::Error("FCB interpreter unsupported binary op");
  }
}

bool PopValue(std::vector<Value>* stack, Value* out) {
  if (stack->empty()) {
    return false;
  }
  *out = std::move(stack->back());
  stack->pop_back();
  return true;
}

bool ValueToString(const Value& value, std::string* out) {
  switch (value.kind) {
    case ValueKind::kNull:
      *out = "null";
      return true;
    case ValueKind::kInt:
      *out = std::to_string(value.int_value);
      return true;
    case ValueKind::kDouble: {
      std::ostringstream os;
      os << value.double_value;
      *out = os.str();
      return true;
    }
    case ValueKind::kBool:
      *out = value.bool_value ? "true" : "false";
      return true;
    case ValueKind::kString:
      *out = value.string_value;
      return true;
    case ValueKind::kBytecodeClosure:
      *out = "<fcb bytecode closure>";
      return true;
    case ValueKind::kList:
    case ValueKind::kMap:
      return false;
  }
  return false;
}

bool MapGetField(const Value& receiver,
                 const std::string& field_name,
                 Value* out) {
  if (receiver.kind != ValueKind::kMap) {
    return false;
  }
  const Value field = Value::String(field_name);
  for (std::size_t i = 0; i + 1 < receiver.map_entries.size(); i += 2) {
    if (ValuesEqual(receiver.map_entries[i], field)) {
      *out = receiver.map_entries[i + 1];
      return true;
    }
  }
  return false;
}

bool MapSetField(Value* receiver,
                 const std::string& field_name,
                 Value value) {
  if (receiver->kind != ValueKind::kMap) {
    return false;
  }
  const Value field = Value::String(field_name);
  for (std::size_t i = 0; i + 1 < receiver->map_entries.size(); i += 2) {
    if (ValuesEqual(receiver->map_entries[i], field)) {
      receiver->map_entries[i + 1] = std::move(value);
      receiver->object_value = nullptr;
      return true;
    }
  }
  receiver->map_entries.push_back(field);
  receiver->map_entries.push_back(std::move(value));
  receiver->object_value = nullptr;
  return true;
}

bool StandaloneIsType(const Value& value, const std::string& type_name) {
  if (type_name == "dynamic" || type_name == "void") {
    return true;
  }
  if (type_name == "Object") {
    return value.kind != ValueKind::kNull;
  }
  if (type_name == "Null") {
    return value.kind == ValueKind::kNull;
  }
  if (type_name == "int") {
    return value.kind == ValueKind::kInt;
  }
  if (type_name == "double") {
    return value.kind == ValueKind::kDouble;
  }
  if (type_name == "num") {
    return value.kind == ValueKind::kInt || value.kind == ValueKind::kDouble;
  }
  if (type_name == "bool") {
    return value.kind == ValueKind::kBool;
  }
  if (type_name == "String") {
    return value.kind == ValueKind::kString;
  }
  if (type_name == "List") {
    return value.kind == ValueKind::kList;
  }
  if (type_name == "Map") {
    return value.kind == ValueKind::kMap;
  }
  return false;
}

uint8_t OperandLength(uint8_t opcode) {
  switch (opcode) {
    case 0x01:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x54:
      return 2;
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x55:
      return 3;
    case 0x61:
      return 4;
    case 0x02:
    case 0x03:
    case 0x04:
      return 1;
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x20:
    case 0x21:
    case 0x60:
    case 0xff:
      return 0;
    default:
      return std::numeric_limits<uint8_t>::max();
  }
}

std::string OpcodeName(uint8_t opcode) {
  switch (opcode) {
    case 0x01:
      return "LoadConst";
    case 0x02:
      return "LoadArg";
    case 0x03:
      return "LoadLocal";
    case 0x04:
      return "StoreLocal";
    case 0x10:
      return "Add";
    case 0x11:
      return "Sub";
    case 0x12:
      return "Mul";
    case 0x13:
      return "Div";
    case 0x20:
      return "Greater";
    case 0x21:
      return "Equal";
    case 0x30:
      return "Jump";
    case 0x31:
      return "JumpIfFalse";
    case 0x32:
      return "JumpIfTrue";
    case 0x40:
      return "MakeList";
    case 0x41:
      return "MakeMap";
    case 0x42:
      return "StringConcat";
    case 0x43:
      return "GetField";
    case 0x44:
      return "SetField";
    case 0x45:
      return "IsType";
    case 0x46:
      return "AsType";
    case 0x50:
      return "CallStatic";
    case 0x51:
      return "CallDynamic";
    case 0x52:
      return "CallOriginal";
    case 0x53:
      return "CallClosure";
    case 0x54:
      return "MakeClosure";
    case 0x55:
      return "NewObject";
    case 0x60:
      return "Throw";
    case 0x61:
      return "TryBegin";
    case 0xff:
      return "Return";
    default:
      return "Unknown";
  }
}

std::string FormatPatchError(const BytecodeFunction& function,
                             uint32_t bytecode_offset,
                             const std::string& message) {
  std::ostringstream os;
  os << message << " at bytecode offset " << bytecode_offset;

  const SourceMapEntry* best = nullptr;
  for (const SourceMapEntry& entry : function.source_map) {
    if (entry.bytecode_offset > bytecode_offset) {
      continue;
    }
    if (best == nullptr || entry.bytecode_offset >= best->bytecode_offset) {
      best = &entry;
    }
  }
  if (best != nullptr && !best->source_location.empty()) {
    os << " (" << best->source_location << " FCB patch)";
  } else {
    os << " (FCB patch)";
  }
  return os.str();
}

}  // namespace

InterpretResult InterpretResult::Ok(Value value) {
  InterpretResult result;
  result.ok = true;
  result.value = std::move(value);
  return result;
}

InterpretResult InterpretResult::Error(std::string error) {
  InterpretResult result;
  result.ok = false;
  result.error = std::move(error);
  return result;
}

bool ValidateModule(const BytecodeModule& module, std::string* error) {
  if (module.version < kMinSupportedModuleVersion ||
      module.version > kMaxSupportedModuleVersion) {
    return SetError(error, "unexpected FCB bytecode module version");
  }
  if (module.functions.empty()) {
    return SetError(error, "FCB bytecode module has no functions");
  }

  std::unordered_set<std::string> names;
  for (const BytecodeFunction& function : module.functions) {
    if (function.function_id.empty()) {
      return SetError(error, "FCB bytecode function id is empty");
    }
    if (!names.insert(function.function_id).second) {
      return SetError(error, "duplicate FCB bytecode function id");
    }
    if (function.register_count < function.parameter_count) {
      return SetError(error,
                      "FCB bytecode register count is smaller than parameter "
                      "count");
    }
    const uint64_t end = static_cast<uint64_t>(function.bytecode_offset) +
                         static_cast<uint64_t>(function.bytecode_length);
    if (end > module.bytecode.size()) {
      return SetError(error,
                      "FCB bytecode function range exceeds module bytecode");
    }
    if (function.bytecode_length == 0) {
      return SetError(error, "FCB bytecode function has empty bytecode");
    }
    std::unordered_set<uint32_t> starts;
    uint32_t pos = 0;
    while (pos < function.bytecode_length) {
      starts.insert(pos);
      const uint8_t opcode = module.bytecode[function.bytecode_offset + pos];
      const uint8_t operand_length = OperandLength(opcode);
      if (operand_length == std::numeric_limits<uint8_t>::max()) {
        std::ostringstream os;
        os << "invalid FCB bytecode opcode 0x" << std::hex
           << static_cast<int>(opcode) << " at offset " << std::dec << pos;
        return SetError(error, os.str());
      }
      if (pos + 1 + operand_length > function.bytecode_length) {
        std::ostringstream os;
        os << "opcode " << OpcodeName(opcode) << " at offset " << pos
           << " requires " << static_cast<int>(operand_length)
           << " operand bytes";
        return SetError(error, os.str());
      }
      if (opcode == 0x01) {
        uint16_t index = 0;
        if (!ReadU16(module.bytecode, function.bytecode_offset,
                     function.bytecode_length, pos + 1, &index)) {
          return SetError(error, "LoadConst missing operand");
        }
        if (index >= function.constants.size()) {
          std::ostringstream os;
          os << "LoadConst at offset " << pos
             << " references missing constant " << index;
          return SetError(error, os.str());
        }
      } else if (opcode == 0x02) {
        const uint8_t index = module.bytecode[function.bytecode_offset + pos + 1];
        if (index >= function.parameter_count) {
          return SetError(error, "LoadArg references missing argument");
        }
      } else if (opcode == 0x43 || opcode == 0x44 || opcode == 0x45 ||
                 opcode == 0x46 || opcode == 0x50 || opcode == 0x51 ||
                 opcode == 0x52 || opcode == 0x54 || opcode == 0x55) {
        uint16_t index = 0;
        if (!ReadU16(module.bytecode, function.bytecode_offset, function.bytecode_length, pos + 1, &index)) {
          return SetError(error, OpcodeName(opcode) + " missing string operand");
        }
        if (index >= function.constants.size() || function.constants[index].kind != ValueKind::kString) {
          return SetError(error, OpcodeName(opcode) + " missing string constant");
        }
        // Named-argument counts are semantic, not structural: validate them
        // per-function at interpret time (fcb_patch_runtime.cc:1394/1419,
        // fcb_patch_runtime_vm.cc:993) so one malformed call does not reject the
        // whole module at load and defeat per-function AOT fallback (E4 / ADR-#2).
      } else if (opcode == 0x53) {
        uint16_t metadata_index = 0;
        if (!ReadU16(module.bytecode, function.bytecode_offset, function.bytecode_length, pos + 1, &metadata_index)) {
          return SetError(error, "CallClosure missing metadata operand");
        }
        if (metadata_index != 0) {
          const std::size_t index = static_cast<std::size_t>(metadata_index - 1);
          if (index >= function.constants.size() || function.constants[index].kind != ValueKind::kString) {
            return SetError(error, "CallClosure missing metadata constant");
          }
          // ;named: prefix and named-argument-count validated at interpret time.
        }
      } else if (opcode == 0x03 || opcode == 0x04) {
        const uint8_t index = module.bytecode[function.bytecode_offset + pos + 1];
        if (index >= function.register_count) {
          return SetError(error, OpcodeName(opcode) + " references missing local");
        }
      }
      pos += 1 + operand_length;
    }
    pos = 0;
    while (pos < function.bytecode_length) {
      const uint8_t opcode =
          module.bytecode[function.bytecode_offset + pos];
      const uint8_t operand_length = OperandLength(opcode);
      if (opcode == 0x30 || opcode == 0x31 || opcode == 0x32 ||
          opcode == 0x61) {
        uint16_t target = 0;
        if (!ReadU16(module.bytecode, function.bytecode_offset,
                     function.bytecode_length, pos + 1, &target)) {
          return SetError(error, "control-flow opcode missing operand");
        }
        if (target >= function.bytecode_length) {
          std::ostringstream os;
          os << OpcodeName(opcode) << " at offset " << pos
             << " targets out-of-range offset " << target;
          return SetError(error, os.str());
        }
        if (starts.find(target) == starts.end()) {
          std::ostringstream os;
          os << OpcodeName(opcode) << " at offset " << pos
             << " targets non-instruction offset " << target;
          return SetError(error, os.str());
        }
        if (opcode == 0x61) {
          uint16_t end = 0;
          if (!ReadU16(module.bytecode, function.bytecode_offset,
                       function.bytecode_length, pos + 3, &end)) {
            return SetError(error, "TryBegin missing end operand");
          }
          if (end >= function.bytecode_length) {
            std::ostringstream os;
            os << "TryBegin at offset " << pos
               << " has out-of-range end offset " << end;
            return SetError(error, os.str());
          }
          if (starts.find(end) == starts.end()) {
            std::ostringstream os;
            os << "TryBegin at offset " << pos
               << " has non-instruction end offset " << end;
            return SetError(error, os.str());
          }
          if (target <= pos || target >= end) {
            std::ostringstream os;
            os << "TryBegin at offset " << pos
               << " requires current < handler < end";
            return SetError(error, os.str());
          }
        }
      }
      pos += 1 + operand_length;
    }
    for (const SourceMapEntry& entry : function.source_map) {
      if (entry.bytecode_offset >= function.bytecode_length) {
        std::ostringstream os;
        os << "source_map for function " << function.function_id
           << " targets out-of-range bytecode offset "
           << entry.bytecode_offset;
        return SetError(error, os.str());
      }
      if (entry.source_location.empty()) {
        std::ostringstream os;
        os << "source_map for function " << function.function_id
           << " has empty source_location";
        return SetError(error, os.str());
      }
    }
  }
  return true;
}

bool PatchTable::Install(const BytecodeModule& module, std::string* error) {
  if (!ValidateModule(module, error)) {
    return false;
  }

  std::unordered_map<std::string, PatchEntry> next;
  next.reserve(module.functions.size());
  for (const BytecodeFunction& function : module.functions) {
    PatchEntry entry;
    entry.state = PatchState::kPatchedInterpreted;
    entry.function = function;
    next.emplace(function.function_id, std::move(entry));
  }
  bytecode_ = module.bytecode;
  entries_ = std::move(next);
  return true;
}

DispatchDecision PatchTable::Resolve(const std::string& function_id) const {
  const auto it = entries_.find(function_id);
  if (it == entries_.end()) {
    return {};
  }
  const PatchEntry& entry = it->second;
  if (entry.state == PatchState::kPatchedInterpreted) {
    return {entry.state, &entry.function};
  }
  return {entry.state, nullptr};
}

DispatchDecision PatchTable::ResolveUniqueByArity(
    std::size_t parameter_count,
    std::string* function_id) const {
  const PatchEntry* match = nullptr;
  const std::string* match_id = nullptr;
  for (const auto& item : entries_) {
    const PatchEntry& entry = item.second;
    if (entry.state != PatchState::kPatchedInterpreted ||
        entry.function.parameter_count != parameter_count) {
      continue;
    }
    if (match != nullptr &&
        !FunctionsEquivalent(match->function, entry.function, bytecode_)) {
      return {};
    }
    if (match == nullptr) {
      match = &entry;
      match_id = &item.first;
    }
  }
  if (match == nullptr) {
    return {};
  }
  if (function_id != nullptr) {
    *function_id = *match_id;
  }
  return {match->state, &match->function};
}

bool PatchTable::Disable(const std::string& function_id) {
  const auto it = entries_.find(function_id);
  if (it == entries_.end()) {
    return false;
  }
  it->second.state = PatchState::kDisabledBadPatch;
  return true;
}

void PatchTable::Clear() {
  bytecode_.clear();
  entries_.clear();
}

void PatchTable::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  for (auto& item : entries_) item.second.function.VisitObjectPointers(visitor);
}

bool PatchRuntime::LoadModule(const BytecodeModule& module,
                              std::string* error) {
  return table_.Install(module, error);
}

bool PatchRuntime::LoadModuleFromFile(const std::string& path,
                                      std::string* error) {
  BytecodeModule module;
  if (!LoadBytecodeModuleFromFile(path, &module, error)) {
    return false;
  }
  return LoadModule(module, error);
}

DispatchDecision PatchRuntime::Resolve(const std::string& function_id) const {
  return table_.Resolve(function_id);
}

DispatchDecision PatchRuntime::ResolveUniqueByArity(
    std::size_t parameter_count,
    std::string* function_id) const {
  return table_.ResolveUniqueByArity(parameter_count, function_id);
}

bool PatchRuntime::FunctionUsesArgument(const BytecodeFunction& function,
                                        uint8_t argument_index) const {
  if (argument_index >= function.parameter_count) {
    return false;
  }
  uint32_t ip = 0;
  while (ip < function.bytecode_length) {
    const uint8_t opcode = table_.bytecode_[function.bytecode_offset + ip++];
    const uint8_t operand_length = OperandLength(opcode);
    if (operand_length == std::numeric_limits<uint8_t>::max() ||
        ip + operand_length > function.bytecode_length) {
      return true;
    }
    if (opcode == 0x02 &&
        table_.bytecode_[function.bytecode_offset + ip] == argument_index) {
      return true;
    }
    ip += operand_length;
  }
  return false;
}

InterpretResult PatchRuntime::Interpret(
    const std::string& function_id,
    const std::vector<Value>& arguments,
    std::size_t captured_argument_count) const {
  internal::ClearPatchStackTraceLocation();
  return InterpretFunction(function_id, arguments, 0, captured_argument_count);
}

InterpretResult PatchRuntime::InterpretFunction(
    const std::string& function_id,
    const std::vector<Value>& arguments,
    uint32_t depth,
    std::size_t captured_argument_count) const {
  if (depth > kMaxCallStaticDepth) {
    return InterpretResult::Error("FCB bytecode CallStatic recursion limit");
  }
  const DispatchDecision decision = Resolve(function_id);
  if (decision.state != PatchState::kPatchedInterpreted ||
      decision.function == nullptr) {
    return InterpretResult::Error("FCB bytecode patch is not active");
  }
  const BytecodeFunction& function = *decision.function;
  if (arguments.size() != function.parameter_count) {
    return InterpretResult::Error("FCB bytecode argument count mismatch");
  }
  std::vector<Value> locals(function.register_count);
  std::vector<Value> stack;
  std::vector<ExceptionHandler> handlers;
  internal::ScopedActivePatchFrame active_frame(
      function, arguments, locals, captured_argument_count);
  uint32_t ip = 0;
  const auto error_at = [&function](uint32_t bytecode_offset,
                                    const std::string& message) {
    internal::RecordPatchStackTraceLocation(function, bytecode_offset);
    return InterpretResult::Error(
        FormatPatchError(function, bytecode_offset, message));
  };
  auto throw_value = [&](uint32_t bytecode_offset,
                         Value thrown) -> InterpretResult {
    if (handlers.empty()) {
      std::string message = "Unhandled throw";
      std::string text;
      if (ValueToString(thrown, &text)) {
        message += ": " + text;
      }
      return error_at(bytecode_offset, message);
    }
    internal::ClearPatchStackTraceLocation();
    const ExceptionHandler handler = handlers.back();
    handlers.pop_back();
    stack.resize(handler.stack_height);
    stack.push_back(std::move(thrown));
    ip = handler.handler_offset;
    return InterpretResult::Ok(Value::Null());
  };
  auto fail_or_throw = [&](uint32_t bytecode_offset,
                           const std::string& message,
                           Value* exception = nullptr) -> InterpretResult {
    if (handlers.empty()) {
      return error_at(bytecode_offset, message);
    }
    if (exception != nullptr && exception->object_value != nullptr) {
      return throw_value(bytecode_offset, std::move(*exception));
    }
    return throw_value(bytecode_offset, Value::String(message));
  };
  while (ip < function.bytecode_length) {
    while (!handlers.empty() && ip >= handlers.back().handler_offset) {
      handlers.pop_back();
    }
    const uint32_t instruction_offset = ip;
    internal::UpdateActivePatchFrame(function, instruction_offset,
                                     captured_argument_count, &arguments,
                                     &locals, handlers.size(),
                                     handlers.empty()
                                         ? 0
                                         : handlers.back().handler_offset,
                                     handlers.empty() ? 0
                                                      : handlers.back().end_offset);
    const uint8_t opcode = table_.bytecode_[function.bytecode_offset + ip++];
    switch (opcode) {
      case 0x01: {
        uint16_t index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &index)) {
          return error_at(instruction_offset, "LoadConst missing operand");
        }
        ip += 2;
        if (index >= function.constants.size()) {
          return error_at(instruction_offset, "LoadConst missing constant");
        }
        stack.push_back(function.constants[index]);
        break;
      }
      case 0x02: {
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "LoadArg missing operand");
        }
        const uint8_t index = table_.bytecode_[function.bytecode_offset + ip++];
        if (index >= arguments.size()) {
          return error_at(instruction_offset, "LoadArg missing argument");
        }
        stack.push_back(arguments[index]);
        break;
      }
      case 0x03: {
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "LoadLocal missing operand");
        }
        const uint8_t index = table_.bytecode_[function.bytecode_offset + ip++];
        if (index >= locals.size()) {
          return error_at(instruction_offset, "LoadLocal missing local");
        }
        stack.push_back(locals[index]);
        break;
      }
      case 0x04: {
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "StoreLocal missing operand");
        }
        const uint8_t index = table_.bytecode_[function.bytecode_offset + ip++];
        if (index >= locals.size()) {
          return error_at(instruction_offset, "StoreLocal missing local");
        }
        Value value;
        if (!PopValue(&stack, &value)) {
          return error_at(instruction_offset, "StoreLocal stack underflow");
        }
        locals[index] = std::move(value);
        break;
      }
      case 0x10:
      case 0x11:
      case 0x12:
      case 0x13:
      case 0x20:
      case 0x21: {
        Value right;
        Value left;
        if (!PopValue(&stack, &right) || !PopValue(&stack, &left)) {
          return error_at(instruction_offset, "binary op stack underflow");
        }
        InterpretResult computed = BinaryOp(opcode, left, right);
        if (!computed.ok) return error_at(instruction_offset, computed.error);
        stack.push_back(std::move(computed.value));
        break;
      }
      case 0x30: {
        uint16_t target = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &target)) {
          return error_at(instruction_offset, "Jump missing operand");
        }
        if (target >= function.bytecode_length) {
          return error_at(instruction_offset, "Jump target out of bounds");
        }
        ip = target;
        break;
      }
      case 0x31:
      case 0x32: {
        uint16_t target = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &target)) {
          return error_at(instruction_offset,
                          "conditional jump missing operand");
        }
        ip += 2;
        if (target >= function.bytecode_length) {
          return error_at(instruction_offset,
                          "conditional jump target out of bounds");
        }
        Value condition;
        if (!PopValue(&stack, &condition)) {
          return error_at(instruction_offset,
                          "conditional jump stack underflow");
        }
        bool bool_condition = false;
        if (!IsTruthy(condition, &bool_condition)) {
          return error_at(instruction_offset, "conditional jump requires bool");
        }
        if ((opcode == 0x31 && !bool_condition) ||
            (opcode == 0x32 && bool_condition)) {
          ip = target;
        }
        break;
      }
      case 0x40: {
        uint16_t count = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &count)) {
          return error_at(instruction_offset, "MakeList missing operand");
        }
        ip += 2;
        if (stack.size() < count) {
          return error_at(instruction_offset, "MakeList stack underflow");
        }
        std::vector<Value> items(count);
        for (uint16_t i = count; i > 0; i--) {
          if (!PopValue(&stack, &items[i - 1])) {
            return error_at(instruction_offset, "MakeList stack underflow");
          }
        }
        stack.push_back(Value::List(std::move(items)));
        break;
      }
      case 0x41: {
        uint16_t count = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &count)) {
          return error_at(instruction_offset, "MakeMap missing operand");
        }
        ip += 2;
        const std::size_t value_count = static_cast<std::size_t>(count) * 2;
        if (stack.size() < value_count) {
          return error_at(instruction_offset, "MakeMap stack underflow");
        }
        std::vector<Value> entries(static_cast<std::size_t>(count) * 2);
        for (uint16_t i = count; i > 0; i--) {
          Value value;
          Value key;
          if (!PopValue(&stack, &value) || !PopValue(&stack, &key)) {
            return error_at(instruction_offset, "MakeMap stack underflow");
          }
          const std::size_t base = static_cast<std::size_t>(i - 1) * 2;
          entries[base] = std::move(key);
          entries[base + 1] = std::move(value);
        }
        stack.push_back(Value::Map(std::move(entries)));
        break;
      }
      case 0x42: {
        uint16_t count = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &count)) {
          return error_at(instruction_offset, "StringConcat missing operand");
        }
        ip += 2;
        if (stack.size() < count) {
          return error_at(instruction_offset, "StringConcat stack underflow");
        }
        std::vector<Value> parts(count);
        for (uint16_t i = count; i > 0; i--) {
          if (!PopValue(&stack, &parts[i - 1])) {
            return error_at(instruction_offset,
                            "StringConcat stack underflow");
          }
        }
        std::string joined;
        for (const Value& part : parts) {
          std::string text;
          if (!ValueToString(part, &text)) {
            return error_at(instruction_offset,
                            "StringConcat requires scalar values");
          }
          joined += text;
        }
        stack.push_back(Value::String(std::move(joined)));
        break;
      }
      case 0x43: {
        uint16_t field_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &field_index)) {
          return error_at(instruction_offset, "GetField missing field");
        }
        ip += 2;
        if (field_index >= function.constants.size() ||
            function.constants[field_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "GetField missing field constant");
        }
        Value receiver;
        if (!PopValue(&stack, &receiver)) {
          return error_at(instruction_offset, "GetField stack underflow");
        }
        Value field_value;
        std::string field_error;
        const std::string& field_name =
            function.constants[field_index].string_value;
        if (internal::DartInstanceGetField(receiver, field_name, &field_value,
                                           &field_error)) {
          stack.push_back(std::move(field_value));
          break;
        }
        if (!MapGetField(receiver, field_name, &field_value)) {
          const std::string reason =
              field_error.empty() ? "missing map field" : field_error;
          return error_at(instruction_offset,
                          "GetField failed: " + reason);
        }
        stack.push_back(std::move(field_value));
        break;
      }
      case 0x44: {
        uint16_t field_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &field_index)) {
          return error_at(instruction_offset, "SetField missing field");
        }
        ip += 2;
        if (field_index >= function.constants.size() ||
            function.constants[field_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "SetField missing field constant");
        }
        Value value;
        Value receiver;
        if (!PopValue(&stack, &value) || !PopValue(&stack, &receiver)) {
          return error_at(instruction_offset, "SetField stack underflow");
        }
        std::string field_error;
        const std::string& field_name =
            function.constants[field_index].string_value;
        if (internal::DartInstanceSetField(&receiver, field_name, value,
                                           &field_error)) {
          stack.push_back(std::move(receiver));
          break;
        }
        if (!MapSetField(&receiver, field_name, std::move(value))) {
          const std::string reason =
              field_error.empty() ? "receiver is not a map" : field_error;
          return error_at(instruction_offset,
                          "SetField failed: " + reason);
        }
        stack.push_back(std::move(receiver));
        break;
      }
      case 0x45:
      case 0x46: {
        uint16_t type_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &type_index)) {
          return error_at(instruction_offset, "type check missing type");
        }
        ip += 2;
        if (type_index >= function.constants.size() ||
            function.constants[type_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "type check missing type constant");
        }
        Value value;
        if (!PopValue(&stack, &value)) {
          return error_at(instruction_offset, "type check stack underflow");
        }
        const std::string& type_name =
            function.constants[type_index].string_value;
        bool matches = false;
        std::string type_error;
        if (!internal::DartIsType(value, type_name, &matches, &type_error)) {
          if (!type_error.empty() &&
              type_error != "IsType requires a Dart VM runtime") {
            return error_at(instruction_offset,
                            "type check failed: " + type_error);
          }
          matches = StandaloneIsType(value, type_name);
        }
        if (opcode == 0x45) {
          stack.push_back(Value::Bool(matches));
        } else if (matches) {
          stack.push_back(std::move(value));
        } else {
          InterpretResult transfer = fail_or_throw(
              instruction_offset, "AsType failed: value is not " + type_name);
          if (!transfer.ok) return transfer;
        }
        break;
      }
      case 0x50: {
        uint16_t function_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &function_index)) {
          return error_at(instruction_offset, "CallStatic missing function");
        }
        ip += 2;
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "CallStatic missing argc");
        }
        const uint8_t argc =
            table_.bytecode_[function.bytecode_offset + ip++];
        if (function_index >= function.constants.size() ||
            function.constants[function_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "CallStatic missing function constant");
        }
        if (stack.size() < argc) {
          return error_at(instruction_offset, "CallStatic stack underflow");
        }
        std::vector<Value> call_args(argc);
        for (uint8_t i = argc; i > 0; i--) {
          if (!PopValue(&stack, &call_args[i - 1])) {
            return error_at(instruction_offset, "CallStatic stack underflow");
          }
        }
        const std::string& target_id =
            function.constants[function_index].string_value;
        InterpretResult result =
            InterpretFunction(target_id, call_args, depth + 1, 0);
        if (!result.ok) {
          InterpretResult transfer = fail_or_throw(
              instruction_offset, "CallStatic failed: " + result.error);
          if (!transfer.ok) return transfer;
          break;
        }
        stack.push_back(std::move(result.value));
        break;
      }
      case 0x51: {
        uint16_t method_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &method_index)) {
          return error_at(instruction_offset, "CallDynamic missing method");
        }
        ip += 2;
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "CallDynamic missing argc");
        }
        const uint8_t argc =
            table_.bytecode_[function.bytecode_offset + ip++];
        if (method_index >= function.constants.size() ||
            function.constants[method_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "CallDynamic missing method constant");
        }
        if (stack.size() < static_cast<std::size_t>(argc) + 1) {
          return error_at(instruction_offset, "CallDynamic stack underflow");
        }
        std::vector<Value> call_args(argc);
        for (uint8_t i = argc; i > 0; i--) {
          if (!PopValue(&stack, &call_args[i - 1])) {
            return error_at(instruction_offset, "CallDynamic stack underflow");
          }
        }
        Value receiver;
        if (!PopValue(&stack, &receiver)) {
          return error_at(instruction_offset, "CallDynamic stack underflow");
        }
        Value result;
        Value exception;
        std::string call_error;
        const std::string& method_name =
            function.constants[method_index].string_value;
        if (!internal::DartInstanceCallDynamic(
                std::move(receiver), method_name, std::move(call_args),
                &result, &call_error, &exception)) {
          InterpretResult transfer = fail_or_throw(
              instruction_offset, "CallDynamic failed: " + call_error,
              &exception);
          if (!transfer.ok) return transfer;
          break;
        }
        stack.push_back(std::move(result));
        break;
      }
      case 0x52: {
        uint16_t function_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &function_index)) {
          return error_at(instruction_offset, "CallOriginal missing function");
        }
        ip += 2;
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "CallOriginal missing argc");
        }
        const uint8_t argc =
            table_.bytecode_[function.bytecode_offset + ip++];
        if (function_index >= function.constants.size() ||
            function.constants[function_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "CallOriginal missing function constant");
        }
        if (stack.size() < argc) {
          return error_at(instruction_offset, "CallOriginal stack underflow");
        }
        std::vector<Value> call_args(argc);
        for (uint8_t i = argc; i > 0; i--) {
          if (!PopValue(&stack, &call_args[i - 1])) {
            return error_at(instruction_offset,
                            "CallOriginal stack underflow");
          }
        }
        Value result;
        Value exception;
        std::string call_error;
        const std::string& target_id =
            function.constants[function_index].string_value;
        if (!internal::DartCallOriginal(target_id, std::move(call_args),
                                        &result, &call_error, &exception)) {
          InterpretResult transfer = fail_or_throw(
              instruction_offset, "CallOriginal failed: " + call_error,
              &exception);
          if (!transfer.ok) return transfer;
          break;
        }
        stack.push_back(std::move(result));
        break;
      }
      case 0x53: {
        uint16_t call_metadata_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &call_metadata_index)) {
          return error_at(instruction_offset, "CallClosure missing operand");
        }
        ip += 2;
        std::string call_metadata;
        if (call_metadata_index != 0) {
          const std::size_t constant_index =
              static_cast<std::size_t>(call_metadata_index - 1);
          if (constant_index >= function.constants.size() ||
              function.constants[constant_index].kind != ValueKind::kString) {
            return error_at(instruction_offset,
                            "CallClosure missing metadata constant");
          }
          call_metadata = function.constants[constant_index].string_value;
          if (call_metadata.rfind(";named:", 0) != 0) {
            return error_at(instruction_offset,
                            "CallClosure metadata must start with ;named:");
          }
        }
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "CallClosure missing argc");
        }
        const uint8_t argc =
            table_.bytecode_[function.bytecode_offset + ip++];
        if (stack.size() < static_cast<std::size_t>(argc) + 1) {
          return error_at(instruction_offset, "CallClosure stack underflow");
        }
        std::vector<Value> call_args(argc);
        for (uint8_t i = argc; i > 0; i--) {
          if (!PopValue(&stack, &call_args[i - 1])) {
            return error_at(instruction_offset,
                            "CallClosure stack underflow");
          }
        }
        Value closure;
        if (!PopValue(&stack, &closure)) {
          return error_at(instruction_offset, "CallClosure stack underflow");
        }
        if (closure.kind == ValueKind::kBytecodeClosure) {
          if (internal::CountNamedArguments(call_metadata) > call_args.size()) {
            return error_at(instruction_offset,
                            "too many named bytecode closure arguments");
          }
          const DispatchDecision closure_decision =
              Resolve(closure.closure_function_id);
          if (closure_decision.state != PatchState::kPatchedInterpreted ||
              closure_decision.function == nullptr) {
            return error_at(instruction_offset,
                            "CallClosure failed: bytecode target not found");
          }
          const std::size_t total_argument_count =
              closure.closure_captures.size() + call_args.size();
          if (total_argument_count !=
              closure_decision.function->parameter_count) {
            return error_at(instruction_offset,
                            "bytecode closure argument count mismatch");
          }
          std::vector<Value> bytecode_args = closure.closure_captures;
          bytecode_args.insert(bytecode_args.end(),
                               std::make_move_iterator(call_args.begin()),
                               std::make_move_iterator(call_args.end()));
          InterpretResult closure_result =
              InterpretFunction(closure.closure_function_id, bytecode_args,
                                depth + 1, closure.closure_captures.size());
          if (!closure_result.ok) {
            InterpretResult transfer = fail_or_throw(
                instruction_offset, "CallClosure failed: " + closure_result.error);
            if (!transfer.ok) return transfer;
            break;
          }
          stack.push_back(std::move(closure_result.value));
          break;
        }
        Value result;
        Value exception;
        std::string call_error;
        if (!internal::DartCallClosure(std::move(closure),
                                       call_metadata, std::move(call_args),
                                       &result, &call_error, &exception)) {
          InterpretResult transfer = fail_or_throw(
              instruction_offset, "CallClosure failed: " + call_error,
              &exception);
          if (!transfer.ok) return transfer;
          break;
        }
        stack.push_back(std::move(result));
        break;
      }
      case 0x54: {
        uint16_t function_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &function_index)) {
          return error_at(instruction_offset, "MakeClosure missing function");
        }
        ip += 2;
        if (function_index >= function.constants.size() ||
            function.constants[function_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "MakeClosure missing function constant");
        }
        Value closure;
        std::string closure_error;
        const std::string& target_id =
            function.constants[function_index].string_value;
        std::string bytecode_target_id;
        std::size_t capture_count = 0;
        intptr_t optional_positional_count = 0;
        intptr_t type_parameter_count = 0;
        std::vector<std::string> named_parameters;
        if (!ParseBytecodeClosureTarget(target_id, &bytecode_target_id,
                                        &capture_count,
                                        &optional_positional_count,
                                        &type_parameter_count,
                                        &named_parameters,
                                        &closure_error)) {
          return error_at(instruction_offset,
                          "MakeClosure failed: " + closure_error);
        }
        if (capture_count > 0 || bytecode_target_id != target_id) {
          if (stack.size() < capture_count) {
            return error_at(instruction_offset,
                            "MakeClosure capture stack underflow");
          }
          const auto target_it = table_.entries_.find(bytecode_target_id);
          if (target_it == table_.entries_.end() ||
              target_it->second.state != PatchState::kPatchedInterpreted) {
            return error_at(instruction_offset,
                            "MakeClosure failed: bytecode target not found");
          }
          std::vector<Value> captures(capture_count);
          for (std::size_t i = capture_count; i > 0; i--) {
            if (!PopValue(&stack, &captures[i - 1])) {
              return error_at(instruction_offset,
                              "MakeClosure capture stack underflow");
            }
          }
          stack.push_back(Value::BytecodeClosure(
              bytecode_target_id, std::move(captures),
              optional_positional_count, type_parameter_count,
              std::move(named_parameters)));
          break;
        }
        if (!internal::DartMakeClosure(target_id, &closure, &closure_error)) {
          InterpretResult transfer = fail_or_throw(
              instruction_offset, "MakeClosure failed: " + closure_error);
          if (!transfer.ok) return transfer;
          break;
        }
        stack.push_back(std::move(closure));
        break;
      }
      case 0x55: {
        uint16_t constructor_index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &constructor_index)) {
          return error_at(instruction_offset, "NewObject missing constructor");
        }
        ip += 2;
        if (ip >= function.bytecode_length) {
          return error_at(instruction_offset, "NewObject missing argc");
        }
        const uint8_t argc =
            table_.bytecode_[function.bytecode_offset + ip++];
        if (constructor_index >= function.constants.size() ||
            function.constants[constructor_index].kind != ValueKind::kString) {
          return error_at(instruction_offset,
                          "NewObject missing constructor constant");
        }
        if (stack.size() < argc) {
          return error_at(instruction_offset, "NewObject stack underflow");
        }
        std::vector<Value> call_args(argc);
        for (uint8_t i = argc; i > 0; i--) {
          if (!PopValue(&stack, &call_args[i - 1])) {
            return error_at(instruction_offset,
                            "NewObject stack underflow");
          }
        }
        Value result;
        Value exception;
        std::string new_error;
        const std::string& constructor_id =
            function.constants[constructor_index].string_value;
        if (!internal::DartNewObject(constructor_id, std::move(call_args),
                                     &result, &new_error, &exception)) {
          InterpretResult transfer = fail_or_throw(
              instruction_offset, "NewObject failed: " + new_error,
              &exception);
          if (!transfer.ok) return transfer;
          break;
        }
        stack.push_back(std::move(result));
        break;
      }
      case 0x60: {
        Value thrown;
        if (!PopValue(&stack, &thrown)) {
          return error_at(instruction_offset, "Throw stack underflow");
        }
        InterpretResult transfer =
            throw_value(instruction_offset, std::move(thrown));
        if (!transfer.ok) return transfer;
        break;
      }
      case 0x61: {
        uint16_t handler_offset = 0;
        uint16_t end_offset = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &handler_offset) ||
            !ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip + 2, &end_offset)) {
          return error_at(instruction_offset, "TryBegin missing operand");
        }
        ip += 4;
        if (handler_offset <= instruction_offset ||
            handler_offset >= function.bytecode_length ||
            end_offset <= handler_offset ||
            end_offset >= function.bytecode_length) {
          return error_at(instruction_offset,
                          "TryBegin target out of bounds");
        }
        handlers.push_back({handler_offset, end_offset, stack.size()});
        break;
      }
      case 0xff: {
        Value value;
        if (!PopValue(&stack, &value)) {
          return error_at(instruction_offset, "Return stack underflow");
        }
        return InterpretResult::Ok(std::move(value));
      }
      default:
        return error_at(instruction_offset, "unsupported FCB bytecode opcode");
    }
  }
  return InterpretResult::Error("FCB bytecode reached end without Return");
}

bool PatchRuntime::DisablePatch(const std::string& function_id) {
  return table_.Disable(function_id);
}

void PatchRuntime::Clear() {
  table_.Clear();
}

void PatchRuntime::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  table_.VisitObjectPointers(visitor);
}

}  // namespace fcb
}  // namespace dart
