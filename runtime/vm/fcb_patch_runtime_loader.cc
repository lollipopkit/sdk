// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace dart {
namespace fcb {
namespace {

constexpr char kBinaryMagic[] = "FCBM";
constexpr std::size_t kBinaryMagicLength = sizeof(kBinaryMagic) - 1;

bool SetError(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
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
      } else if (key == "source_map") {
        if (!ReadSourceMap(function)) return false;
      } else if (key == "debug_locals") {
        if (!ReadDebugLocals(function)) return false;
      } else {
        if (!SkipValue()) return false;
      }
      if (!ConsumeOptional(',')) break;
    }
    if (!Consume('}')) return false;
    if (!saw_name) return Fail("FCB bytecode function missing name");
    if (!saw_param_count) return Fail("FCB bytecode function missing param_count");
    if (!saw_local_count) return Fail("FCB bytecode function missing local_count");
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

  bool ReadSourceMap(BytecodeFunction* function) {
    if (!Consume('[')) return false;
    while (!Peek(']')) {
      SourceMapEntry entry;
      if (!ReadSourceMapEntry(&entry)) return false;
      function->source_map.push_back(std::move(entry));
      if (!ConsumeOptional(',')) break;
    }
    return Consume(']');
  }

  bool ReadSourceMapEntry(SourceMapEntry* entry) {
    if (!Consume('{')) return false;
    bool saw_offset = false;
    bool saw_location = false;
    while (!Peek('}')) {
      std::string key;
      if (!ReadString(&key) || !Consume(':')) return false;
      if (key == "bytecode_offset") {
        uint64_t value = 0;
        if (!ReadUnsigned(&value)) return false;
        if (value > std::numeric_limits<uint32_t>::max()) {
          return Fail("source_map bytecode_offset exceeds u32");
        }
        entry->bytecode_offset = static_cast<uint32_t>(value);
        saw_offset = true;
      } else if (key == "source_location") {
        if (!ReadString(&entry->source_location)) return false;
        saw_location = true;
      } else {
        if (!SkipValue()) return false;
      }
      if (!ConsumeOptional(',')) break;
    }
    if (!Consume('}')) return false;
    if (!saw_offset) return Fail("source_map entry missing bytecode_offset");
    if (!saw_location) return Fail("source_map entry missing source_location");
    return true;
  }

  bool ReadDebugLocals(BytecodeFunction* function) {
    if (!Consume('[')) return false;
    while (!Peek(']')) {
      DebugLocalEntry entry;
      if (!ReadDebugLocalEntry(&entry)) return false;
      function->debug_locals.push_back(std::move(entry));
      if (!ConsumeOptional(',')) break;
    }
    return Consume(']');
  }

  bool ReadDebugLocalEntry(DebugLocalEntry* entry) {
    if (!Consume('{')) return false;
    bool saw_slot = false;
    bool saw_name = false;
    while (!Peek('}')) {
      std::string key;
      if (!ReadString(&key) || !Consume(':')) return false;
      if (key == "slot") {
        uint64_t value = 0;
        if (!ReadUnsigned(&value)) return false;
        if (value > std::numeric_limits<uint16_t>::max()) {
          return Fail("debug_locals slot exceeds u16");
        }
        entry->slot = static_cast<uint16_t>(value);
        saw_slot = true;
      } else if (key == "name") {
        if (!ReadString(&entry->name)) return false;
        saw_name = true;
      } else {
        if (!SkipValue()) return false;
      }
      if (!ConsumeOptional(',')) break;
    }
    if (!Consume('}')) return false;
    if (!saw_slot) return Fail("debug_locals entry missing slot");
    if (!saw_name) return Fail("debug_locals entry missing name");
    return true;
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

class BinaryReader {
 public:
  BinaryReader(const std::vector<uint8_t>& bytes, std::string* error)
      : bytes_(bytes), error_(error) {}

  bool ReadModule(BytecodeModule* module) {
    if (module == nullptr) {
      return Fail("module output is null");
    }
    module->version = 0;
    module->bytecode.clear();
    module->functions.clear();

    if (!ExpectMagic()) return false;
    if (!ReadU32(&module->version)) return false;
    uint16_t function_count = 0;
    if (!ReadU16(&function_count)) return false;
    module->functions.reserve(function_count);
    for (uint16_t i = 0; i < function_count; i++) {
      BytecodeFunction function;
      const uint32_t offset = static_cast<uint32_t>(module->bytecode.size());
      if (!ReadFunction(module->version, &function, &module->bytecode)) {
        return false;
      }
      function.bytecode_offset = offset;
      function.bytecode_length =
          static_cast<uint32_t>(module->bytecode.size() - offset);
      module->functions.push_back(std::move(function));
    }
    if (pos_ != bytes_.size()) {
      return Fail("trailing FCB bytecode binary data");
    }
    return true;
  }

 private:
  bool ReadFunction(uint32_t module_version,
                    BytecodeFunction* function,
                    std::vector<uint8_t>* bytecode) {
    if (!ReadString(&function->function_id)) return false;
    uint8_t return_convention = 0;
    if (!ReadU8(&return_convention)) return false;
    switch (return_convention) {
      case 0:
        function->return_convention = ReturnConvention::kTagged;
        break;
      case 1:
        function->return_convention = ReturnConvention::kUnboxedInt64;
        break;
      default:
        return Fail("unsupported binary return convention");
    }
    if (!ReadU8(&function->parameter_count)) return false;
    if (!ReadU8(&function->register_count)) return false;
    uint16_t constant_count = 0;
    if (!ReadU16(&constant_count)) return false;
    function->constants.reserve(constant_count);
    for (uint16_t i = 0; i < constant_count; i++) {
      Value constant;
      if (!ReadConstant(&constant)) return false;
      function->constants.push_back(std::move(constant));
    }
    uint32_t code_length = 0;
    if (!ReadU32(&code_length)) return false;
    if (code_length > bytes_.size() - pos_) {
      return Fail("truncated FCB bytecode binary code");
    }
    bytecode->insert(bytecode->end(), bytes_.begin() + pos_,
                     bytes_.begin() + pos_ + code_length);
    pos_ += code_length;
    uint16_t source_map_count = 0;
    if (!ReadU16(&source_map_count)) return false;
    function->source_map.reserve(source_map_count);
    for (uint16_t i = 0; i < source_map_count; i++) {
      SourceMapEntry entry;
      if (!ReadU32(&entry.bytecode_offset)) return false;
      if (!ReadString(&entry.source_location)) return false;
      function->source_map.push_back(std::move(entry));
    }
    if (module_version >= 2) {
      uint16_t debug_local_count = 0;
      if (!ReadU16(&debug_local_count)) return false;
      function->debug_locals.reserve(debug_local_count);
      for (uint16_t i = 0; i < debug_local_count; i++) {
        DebugLocalEntry entry;
        if (!ReadU16(&entry.slot)) return false;
        if (!ReadString(&entry.name)) return false;
        function->debug_locals.push_back(std::move(entry));
      }
    }
    return true;
  }

  bool ReadConstant(Value* constant) {
    uint8_t tag = 0;
    if (!ReadU8(&tag)) return false;
    switch (tag) {
      case 0:
        *constant = Value::Null();
        return true;
      case 1: {
        int64_t value = 0;
        if (!ReadI64(&value)) return false;
        *constant = Value::Int(value);
        return true;
      }
      case 2: {
        double value = 0;
        if (!ReadF64(&value)) return false;
        *constant = Value::Double(value);
        return true;
      }
      case 3: {
        uint8_t value = 0;
        if (!ReadU8(&value)) return false;
        if (value > 1) return Fail("invalid binary bool constant byte");
        *constant = Value::Bool(value == 1);
        return true;
      }
      case 4: {
        std::string value;
        if (!ReadString(&value)) return false;
        *constant = Value::String(std::move(value));
        return true;
      }
      default:
        return Fail("unsupported binary constant tag");
    }
  }

  bool ExpectMagic() {
    if (bytes_.size() < kBinaryMagicLength ||
        std::memcmp(bytes_.data(), kBinaryMagic, kBinaryMagicLength) != 0) {
      return Fail("invalid FCB bytecode binary magic");
    }
    pos_ = kBinaryMagicLength;
    return true;
  }

  bool ReadU8(uint8_t* out) {
    if (pos_ + 1 > bytes_.size()) return Fail("truncated FCB bytecode binary");
    *out = bytes_[pos_++];
    return true;
  }

  bool ReadU16(uint16_t* out) {
    if (pos_ + 2 > bytes_.size()) return Fail("truncated FCB bytecode binary");
    *out = static_cast<uint16_t>((bytes_[pos_] << 8) | bytes_[pos_ + 1]);
    pos_ += 2;
    return true;
  }

  bool ReadU32(uint32_t* out) {
    if (pos_ + 4 > bytes_.size()) return Fail("truncated FCB bytecode binary");
    *out = (static_cast<uint32_t>(bytes_[pos_]) << 24) |
           (static_cast<uint32_t>(bytes_[pos_ + 1]) << 16) |
           (static_cast<uint32_t>(bytes_[pos_ + 2]) << 8) |
           static_cast<uint32_t>(bytes_[pos_ + 3]);
    pos_ += 4;
    return true;
  }

  bool ReadI64(int64_t* out) {
    if (pos_ + 8 > bytes_.size()) return Fail("truncated FCB bytecode binary");
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
      value = (value << 8) | bytes_[pos_ + i];
    }
    pos_ += 8;
    *out = static_cast<int64_t>(value);
    return true;
  }

  bool ReadF64(double* out) {
    if (pos_ + 8 > bytes_.size()) return Fail("truncated FCB bytecode binary");
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) {
      bits = (bits << 8) | bytes_[pos_ + i];
    }
    pos_ += 8;
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  bool ReadString(std::string* out) {
    uint16_t size = 0;
    if (!ReadU16(&size)) return false;
    if (pos_ + size > bytes_.size()) {
      return Fail("truncated FCB bytecode binary string");
    }
    out->assign(reinterpret_cast<const char*>(bytes_.data() + pos_), size);
    pos_ += size;
    return true;
  }

  bool Fail(const std::string& message) {
    std::ostringstream os;
    os << message << " at byte " << pos_;
    return SetError(error_, os.str());
  }

  const std::vector<uint8_t>& bytes_;
  std::string* error_;
  std::size_t pos_ = 0;
};

}  // namespace

bool LoadBytecodeModuleFromFile(const std::string& path,
                                BytecodeModule* module,
                                std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return SetError(error, "failed to open FCB bytecode module file");
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() >= kBinaryMagicLength &&
      std::memcmp(bytes.data(), kBinaryMagic, kBinaryMagicLength) == 0) {
    BinaryReader reader(bytes, error);
    if (!reader.ReadModule(module)) {
      return false;
    }
  } else {
    std::string input(bytes.begin(), bytes.end());
    JsonReader reader(std::move(input), error);
    if (!reader.ReadModule(module)) {
      return false;
    }
  }
  return ValidateModule(*module, error);
}

}  // namespace fcb
}  // namespace dart
