// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dart {
namespace fcb {
namespace {

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

bool ReturnConventionFromString(const std::string& value,
                                ReturnConvention* out) {
  if (value == "tagged") {
    *out = ReturnConvention::kTagged;
    return true;
  }
  if (value == "unboxed_int64") {
    *out = ReturnConvention::kUnboxedInt64;
    return true;
  }
  return false;
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

uint8_t OperandLength(uint8_t opcode) {
  switch (opcode) {
    case 0x01:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x40:
    case 0x41:
      return 2;
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
    case 0xff:
      return "Return";
    default:
      return "Unknown";
  }
}

class JsonReader {
 public:
  JsonReader(std::string input, std::string* error)
      : input_(std::move(input)), error_(error) {}

  bool ReadModule(BytecodeModule* module) {
    if (module == nullptr) {
      return Fail("module output is null");
    }
    module->version = 0;
    module->bytecode.clear();
    module->functions.clear();

    if (!Consume('{')) return false;
    bool saw_version = false;
    bool saw_functions = false;
    while (!Peek('}')) {
      std::string key;
      if (!ReadString(&key) || !Consume(':')) return false;
      if (key == "version") {
        uint64_t version = 0;
        if (!ReadUnsigned(&version)) return false;
        module->version = static_cast<uint32_t>(version);
        saw_version = true;
      } else if (key == "functions") {
        if (!ReadFunctions(module)) return false;
        saw_functions = true;
      } else {
        if (!SkipValue()) return false;
      }
      if (!ConsumeOptional(',')) break;
    }
    if (!Consume('}')) return false;
    SkipWhitespace();
    if (pos_ != input_.size()) {
      return Fail("unexpected trailing JSON data");
    }
    if (!saw_version) return Fail("FCB bytecode module missing version");
    if (!saw_functions) return Fail("FCB bytecode module missing functions");
    return true;
  }

 private:
  bool ReadFunctions(BytecodeModule* module) {
    if (!Consume('[')) return false;
    while (!Peek(']')) {
      BytecodeFunction function;
      const uint32_t offset = static_cast<uint32_t>(module->bytecode.size());
      if (!ReadFunction(&function, &module->bytecode)) return false;
      function.bytecode_offset = offset;
      function.bytecode_length =
          static_cast<uint32_t>(module->bytecode.size() - offset);
      module->functions.push_back(std::move(function));
      if (!ConsumeOptional(',')) break;
    }
    return Consume(']');
  }

  bool ReadFunction(BytecodeFunction* function, std::vector<uint8_t>* bytecode) {
    if (!Consume('{')) return false;
    bool saw_name = false;
    bool saw_param_count = false;
    bool saw_local_count = false;
    bool saw_code = false;
    while (!Peek('}')) {
      std::string key;
      if (!ReadString(&key) || !Consume(':')) return false;
      if (key == "name") {
        if (!ReadString(&function->function_id)) return false;
        saw_name = true;
      } else if (key == "return_convention") {
        std::string value;
        if (!ReadString(&value)) return false;
        if (!ReturnConventionFromString(value,
                                        &function->return_convention)) {
          return Fail("unsupported FCB bytecode return_convention");
        }
      } else if (key == "param_count") {
        uint64_t value = 0;
        if (!ReadUnsigned(&value) || !ReadU8(value, &function->parameter_count)) {
          return false;
        }
        saw_param_count = true;
      } else if (key == "local_count") {
        uint64_t value = 0;
        if (!ReadUnsigned(&value) || !ReadU8(value, &function->register_count)) {
          return false;
        }
        saw_local_count = true;
      } else if (key == "constants") {
        if (!ReadConstants(function)) return false;
      } else if (key == "code") {
        if (!ReadCode(bytecode)) return false;
        saw_code = true;
      } else {
        if (!SkipValue()) return false;
      }
      if (!ConsumeOptional(',')) break;
    }
    if (!Consume('}')) return false;
    if (!saw_name) return Fail("FCB bytecode function missing name");
    if (!saw_param_count) {
      return Fail("FCB bytecode function missing param_count");
    }
    if (!saw_local_count) {
      return Fail("FCB bytecode function missing local_count");
    }
    if (!saw_code) return Fail("FCB bytecode function missing code");
    return true;
  }

  bool ReadConstants(BytecodeFunction* function) {
    if (!Consume('[')) return false;
    while (!Peek(']')) {
      Value constant;
      if (!ReadConstant(&constant)) return false;
      function->constants.push_back(std::move(constant));
      if (!ConsumeOptional(',')) break;
    }
    return Consume(']');
  }

  bool ReadConstant(Value* constant) {
    if (!Consume('{')) return false;
    bool saw_type = false;
    std::string type;
    while (!Peek('}')) {
      std::string key;
      if (!ReadString(&key) || !Consume(':')) return false;
      if (key == "type") {
        if (!ReadString(&type)) return false;
        saw_type = true;
      } else if (key == "value") {
        if (!ReadConstantValue(type, constant)) return false;
      } else {
        if (!SkipValue()) return false;
      }
      if (!ConsumeOptional(',')) break;
    }
    if (!Consume('}')) return false;
    if (!saw_type) return Fail("FCB bytecode constant missing type");
    if (type == "Null") {
      *constant = Value::Null();
    }
    return true;
  }

  bool ReadConstantValue(const std::string& type, Value* constant) {
    if (type == "Int") {
      int64_t value = 0;
      if (!ReadSigned(&value)) return false;
      *constant = Value::Int(value);
      return true;
    }
    if (type == "Double") {
      double value = 0;
      if (!ReadDouble(&value)) return false;
      *constant = Value::Double(value);
      return true;
    }
    if (type == "Bool") {
      if (MatchLiteral("true")) {
        *constant = Value::Bool(true);
        return true;
      }
      if (MatchLiteral("false")) {
        *constant = Value::Bool(false);
        return true;
      }
      return Fail("expected JSON bool constant value");
    }
    if (type == "String") {
      std::string value;
      if (!ReadString(&value)) return false;
      *constant = Value::String(std::move(value));
      return true;
    }
    if (type == "Null") {
      if (!MatchLiteral("null")) return Fail("expected JSON null constant");
      *constant = Value::Null();
      return true;
    }
    return Fail("unsupported FCB bytecode constant type");
  }

  bool ReadCode(std::vector<uint8_t>* bytecode) {
    if (!Consume('[')) return false;
    while (!Peek(']')) {
      uint64_t value = 0;
      if (!ReadUnsigned(&value)) return false;
      if (value > 255) return Fail("FCB bytecode code byte exceeds u8");
      bytecode->push_back(static_cast<uint8_t>(value));
      if (!ConsumeOptional(',')) break;
    }
    return Consume(']');
  }

  bool ReadU8(uint64_t value, uint8_t* out) {
    if (value > 255) return Fail("FCB bytecode u8 field exceeds range");
    *out = static_cast<uint8_t>(value);
    return true;
  }

  bool SkipValue() {
    SkipWhitespace();
    if (Peek('{')) return SkipObject();
    if (Peek('[')) return SkipArray();
    if (Peek('"')) {
      std::string ignored;
      return ReadString(&ignored);
    }
    if (MatchLiteral("true") || MatchLiteral("false") ||
        MatchLiteral("null")) {
      return true;
    }
    double ignored = 0;
    return ReadDouble(&ignored);
  }

  bool SkipObject() {
    if (!Consume('{')) return false;
    while (!Peek('}')) {
      std::string key;
      if (!ReadString(&key) || !Consume(':') || !SkipValue()) return false;
      if (!ConsumeOptional(',')) break;
    }
    return Consume('}');
  }

  bool SkipArray() {
    if (!Consume('[')) return false;
    while (!Peek(']')) {
      if (!SkipValue()) return false;
      if (!ConsumeOptional(',')) break;
    }
    return Consume(']');
  }

  bool ReadString(std::string* out) {
    SkipWhitespace();
    if (pos_ >= input_.size() || input_[pos_] != '"') {
      return Fail("expected JSON string");
    }
    pos_++;
    out->clear();
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= input_.size()) return Fail("unterminated JSON escape");
        const char escaped = input_[pos_++];
        switch (escaped) {
          case '"':
          case '\\':
          case '/':
            out->push_back(escaped);
            break;
          case 'b':
            out->push_back('\b');
            break;
          case 'f':
            out->push_back('\f');
            break;
          case 'n':
            out->push_back('\n');
            break;
          case 'r':
            out->push_back('\r');
            break;
          case 't':
            out->push_back('\t');
            break;
          default:
            return Fail("unsupported JSON string escape");
        }
      } else {
        out->push_back(c);
      }
    }
    return Fail("unterminated JSON string");
  }

  bool ReadUnsigned(uint64_t* out) {
    SkipWhitespace();
    if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
      return Fail("expected unsigned JSON number");
    }
    uint64_t value = 0;
    while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
      value = value * 10 + static_cast<uint64_t>(input_[pos_] - '0');
      pos_++;
    }
    *out = value;
    return true;
  }

  bool ReadSigned(int64_t* out) {
    SkipWhitespace();
    const std::size_t start = pos_;
    if (pos_ < input_.size() && input_[pos_] == '-') {
      pos_++;
    }
    if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
      pos_ = start;
      return Fail("expected signed JSON integer");
    }
    while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
      pos_++;
    }
    const std::string token = input_.substr(start, pos_ - start);
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(token.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0') {
      return Fail("JSON integer exceeds i64 range");
    }
    *out = static_cast<int64_t>(value);
    return true;
  }

  bool ReadDouble(double* out) {
    SkipWhitespace();
    const std::size_t start = pos_;
    if (pos_ < input_.size() && input_[pos_] == '-') {
      pos_++;
    }
    if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
      pos_ = start;
      return Fail("expected JSON number");
    }
    while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
      pos_++;
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      pos_++;
      if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
        return Fail("expected JSON number fraction");
      }
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        pos_++;
      }
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      pos_++;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
        pos_++;
      }
      if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
        return Fail("expected JSON number exponent");
      }
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        pos_++;
      }
    }
    const std::string token = input_.substr(start, pos_ - start);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(token.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0') {
      return Fail("JSON number exceeds double range");
    }
    *out = value;
    return true;
  }

  bool Consume(char c) {
    SkipWhitespace();
    if (pos_ >= input_.size() || input_[pos_] != c) {
      std::ostringstream os;
      os << "expected '" << c << "'";
      if (pos_ < input_.size()) {
        os << ", got byte " << static_cast<int>(
            static_cast<unsigned char>(input_[pos_]));
      }
      return Fail(os.str());
    }
    pos_++;
    return true;
  }

  bool ConsumeOptional(char c) {
    SkipWhitespace();
    if (pos_ < input_.size() && input_[pos_] == c) {
      pos_++;
      return true;
    }
    return false;
  }

  bool Peek(char c) {
    SkipWhitespace();
    return pos_ < input_.size() && input_[pos_] == c;
  }

  bool MatchLiteral(const char* literal) {
    SkipWhitespace();
    const std::string text(literal);
    if (input_.compare(pos_, text.size(), text) != 0) {
      return false;
    }
    pos_ += text.size();
    return true;
  }

  void SkipWhitespace() {
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return;
      pos_++;
    }
  }

  bool Fail(const std::string& message) {
    std::ostringstream os;
    os << message << " at byte " << pos_;
    return SetError(error_, os.str());
  }

  const std::string input_;
  std::string* error_;
  std::size_t pos_ = 0;
};

}  // namespace

Value Value::Null() {
  return {};
}

Value Value::Int(int64_t value) {
  Value out;
  out.kind = ValueKind::kInt;
  out.int_value = value;
  return out;
}

Value Value::Double(double value) {
  Value out;
  out.kind = ValueKind::kDouble;
  out.double_value = value;
  return out;
}

Value Value::Bool(bool value) {
  Value out;
  out.kind = ValueKind::kBool;
  out.bool_value = value;
  return out;
}

Value Value::String(std::string value) {
  Value out;
  out.kind = ValueKind::kString;
  out.string_value = std::move(value);
  return out;
}

Value Value::List(std::vector<Value> value) {
  Value out;
  out.kind = ValueKind::kList;
  out.list_value = std::move(value);
  return out;
}

Value Value::Map(std::vector<Value> entries) {
  Value out;
  out.kind = ValueKind::kMap;
  out.map_entries = std::move(entries);
  return out;
}

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
  if (module.version != 1) {
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
      const uint8_t opcode =
          module.bytecode[function.bytecode_offset + pos];
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
          std::ostringstream os;
          os << "LoadArg at offset " << pos << " references missing argument "
             << static_cast<int>(index);
          return SetError(error, os.str());
        }
      } else if (opcode == 0x03 || opcode == 0x04) {
        const uint8_t index = module.bytecode[function.bytecode_offset + pos + 1];
        if (index >= function.register_count) {
          std::ostringstream os;
          os << OpcodeName(opcode) << " at offset " << pos
             << " references missing local " << static_cast<int>(index);
          return SetError(error, os.str());
        }
      }
      pos += 1 + operand_length;
    }
    pos = 0;
    while (pos < function.bytecode_length) {
      const uint8_t opcode =
          module.bytecode[function.bytecode_offset + pos];
      const uint8_t operand_length = OperandLength(opcode);
      if (opcode == 0x30 || opcode == 0x31 || opcode == 0x32) {
        uint16_t target = 0;
        if (!ReadU16(module.bytecode, function.bytecode_offset,
                     function.bytecode_length, pos + 1, &target)) {
          return SetError(error, "jump missing operand");
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
      }
      pos += 1 + operand_length;
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
    const std::vector<Value>& arguments) const {
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
  uint32_t ip = 0;
  while (ip < function.bytecode_length) {
    const uint8_t opcode = table_.bytecode_[function.bytecode_offset + ip++];
    switch (opcode) {
      case 0x01: {
        uint16_t index = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &index)) {
          return InterpretResult::Error("LoadConst missing operand");
        }
        ip += 2;
        if (index >= function.constants.size()) {
          return InterpretResult::Error("LoadConst missing constant");
        }
        stack.push_back(function.constants[index]);
        break;
      }
      case 0x02: {
        if (ip >= function.bytecode_length) {
          return InterpretResult::Error("LoadArg missing operand");
        }
        const uint8_t index = table_.bytecode_[function.bytecode_offset + ip++];
        if (index >= arguments.size()) {
          return InterpretResult::Error("LoadArg missing argument");
        }
        stack.push_back(arguments[index]);
        break;
      }
      case 0x03: {
        if (ip >= function.bytecode_length) {
          return InterpretResult::Error("LoadLocal missing operand");
        }
        const uint8_t index = table_.bytecode_[function.bytecode_offset + ip++];
        if (index >= locals.size()) {
          return InterpretResult::Error("LoadLocal missing local");
        }
        stack.push_back(locals[index]);
        break;
      }
      case 0x04: {
        if (ip >= function.bytecode_length) {
          return InterpretResult::Error("StoreLocal missing operand");
        }
        const uint8_t index = table_.bytecode_[function.bytecode_offset + ip++];
        if (index >= locals.size()) {
          return InterpretResult::Error("StoreLocal missing local");
        }
        Value value;
        if (!PopValue(&stack, &value)) {
          return InterpretResult::Error("StoreLocal stack underflow");
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
          return InterpretResult::Error("binary op stack underflow");
        }
        InterpretResult computed = BinaryOp(opcode, left, right);
        if (!computed.ok) return computed;
        stack.push_back(std::move(computed.value));
        break;
      }
      case 0x30: {
        uint16_t target = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &target)) {
          return InterpretResult::Error("Jump missing operand");
        }
        if (target >= function.bytecode_length) {
          return InterpretResult::Error("Jump target out of bounds");
        }
        ip = target;
        break;
      }
      case 0x31:
      case 0x32: {
        uint16_t target = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &target)) {
          return InterpretResult::Error("conditional jump missing operand");
        }
        ip += 2;
        if (target >= function.bytecode_length) {
          return InterpretResult::Error(
              "conditional jump target out of bounds");
        }
        Value condition;
        if (!PopValue(&stack, &condition)) {
          return InterpretResult::Error("conditional jump stack underflow");
        }
        bool bool_condition = false;
        if (!IsTruthy(condition, &bool_condition)) {
          return InterpretResult::Error("conditional jump requires bool");
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
          return InterpretResult::Error("MakeList missing operand");
        }
        ip += 2;
        if (stack.size() < count) {
          return InterpretResult::Error("MakeList stack underflow");
        }
        std::vector<Value> items(count);
        for (uint16_t i = count; i > 0; i--) {
          if (!PopValue(&stack, &items[i - 1])) {
            return InterpretResult::Error("MakeList stack underflow");
          }
        }
        stack.push_back(Value::List(std::move(items)));
        break;
      }
      case 0x41: {
        uint16_t count = 0;
        if (!ReadU16(table_.bytecode_, function.bytecode_offset,
                     function.bytecode_length, ip, &count)) {
          return InterpretResult::Error("MakeMap missing operand");
        }
        ip += 2;
        const std::size_t value_count = static_cast<std::size_t>(count) * 2;
        if (stack.size() < value_count) {
          return InterpretResult::Error("MakeMap stack underflow");
        }
        std::vector<Value> entries(static_cast<std::size_t>(count) * 2);
        for (uint16_t i = count; i > 0; i--) {
          Value value;
          Value key;
          if (!PopValue(&stack, &value) || !PopValue(&stack, &key)) {
            return InterpretResult::Error("MakeMap stack underflow");
          }
          const std::size_t base = static_cast<std::size_t>(i - 1) * 2;
          entries[base] = std::move(key);
          entries[base + 1] = std::move(value);
        }
        stack.push_back(Value::Map(std::move(entries)));
        break;
      }
      case 0xff: {
        Value value;
        if (!PopValue(&stack, &value)) {
          return InterpretResult::Error("Return stack underflow");
        }
        return InterpretResult::Ok(std::move(value));
      }
      default:
        return InterpretResult::Error("unsupported FCB bytecode opcode");
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

bool LoadBytecodeModuleFromFile(const std::string& path,
                                BytecodeModule* module,
                                std::string* error) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return SetError(error, "failed to open FCB bytecode module file");
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  JsonReader reader(buffer.str(), error);
  if (!reader.ReadModule(module)) {
    return false;
  }
  return ValidateModule(*module, error);
}

}  // namespace fcb
}  // namespace dart
