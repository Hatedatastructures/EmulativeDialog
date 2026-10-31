#include "Source/Core/BusinessActionRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace EmulativeDialog::Core {
namespace {

using RegistrationError = BusinessActionRegistrationError;
using RegistrationErrorCode = BusinessActionRegistrationErrorCode;

std::string ToLowerAscii(std::string_view Text) {
  std::string Result;
  Result.reserve(Text.size());
  for (const char Value : Text) {
    Result.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(Value))));
  }
  return Result;
}

bool StartsWithAny(std::string_view Value,
                   std::initializer_list<std::string_view> Prefixes) {
  return std::any_of(Prefixes.begin(), Prefixes.end(),
                     [Value](std::string_view Prefix) {
                       return Value.starts_with(Prefix);
                     });
}

std::vector<std::string> SplitPascalWords(std::string_view Name) {
  std::vector<std::string> Words;
  std::string Current;
  for (std::size_t Index = 0; Index < Name.size(); ++Index) {
    const char Value = Name[Index];
    const bool StartsNewWord =
        !Current.empty() && Value >= 'A' && Value <= 'Z' &&
        Name[Index - 1] >= 'a' && Name[Index - 1] <= 'z';
    if (StartsNewWord) {
      Words.push_back(ToLowerAscii(Current));
      Current.clear();
    }
    Current.push_back(Value);
  }
  if (!Current.empty()) {
    Words.push_back(ToLowerAscii(Current));
  }
  return Words;
}

bool IsValidSchemaField(std::string_view Field) {
  if (Field.empty() || Field.size() > 64U) {
    return false;
  }
  return std::all_of(Field.begin(), Field.end(), [](char Value) {
    const auto Character = static_cast<unsigned char>(Value);
    return std::isalnum(Character) != 0 || Value == '_' || Value == '-';
  });
}

class JsonParser final {
public:
  explicit JsonParser(std::string_view TextValue) : Text(TextValue) {}

  bool ParseRootObject(std::vector<std::string>& Keys) {
    SkipWhitespace();
    if (!Consume('{')) {
      return false;
    }
    SkipWhitespace();
    if (Consume('}')) {
      return AtEnd();
    }
    while (true) {
      SkipWhitespace();
      std::string Key;
      if (!ParseString(Key)) {
        return false;
      }
      if (std::find(Keys.begin(), Keys.end(), Key) != Keys.end()) {
        return false;
      }
      Keys.push_back(Key);
      SkipWhitespace();
      if (!Consume(':')) {
        return false;
      }
      if (!ParseValue(0)) {
        return false;
      }
      SkipWhitespace();
      if (Consume(',')) {
        continue;
      }
      if (Consume('}')) {
        return AtEnd();
      }
      return false;
    }
  }

private:
  static constexpr std::size_t MaximumDepth = 32;

  bool ParseValue(std::size_t Depth) {
    if (Depth > MaximumDepth) {
      return false;
    }
    SkipWhitespace();
    if (AtEnd()) {
      return false;
    }
    switch (Text[Position]) {
    case '{':
      return ParseObject(Depth + 1U);
    case '[':
      return ParseArray(Depth + 1U);
    case '"': {
      std::string Ignored;
      return ParseString(Ignored);
    }
    case 't':
      return ParseLiteral("true");
    case 'f':
      return ParseLiteral("false");
    case 'n':
      return ParseLiteral("null");
    default:
      return ParseNumber();
    }
  }

  bool ParseObject(std::size_t Depth) {
    if (!Consume('{')) {
      return false;
    }
    SkipWhitespace();
    if (Consume('}')) {
      return true;
    }
    while (true) {
      SkipWhitespace();
      std::string Key;
      if (!ParseString(Key)) {
        return false;
      }
      SkipWhitespace();
      if (!Consume(':')) {
        return false;
      }
      if (!ParseValue(Depth)) {
        return false;
      }
      SkipWhitespace();
      if (Consume(',')) {
        continue;
      }
      return Consume('}');
    }
  }

  bool ParseArray(std::size_t Depth) {
    if (!Consume('[')) {
      return false;
    }
    SkipWhitespace();
    if (Consume(']')) {
      return true;
    }
    while (true) {
      if (!ParseValue(Depth)) {
        return false;
      }
      SkipWhitespace();
      if (Consume(',')) {
        continue;
      }
      return Consume(']');
    }
  }

  bool ParseString(std::string& Out) {
    if (!Consume('"')) {
      return false;
    }
    while (!AtEnd()) {
      const char Value = Text[Position++];
      if (Value == '"') {
        return true;
      }
      if (static_cast<unsigned char>(Value) < 0x20U) {
        return false;
      }
      if (Value != '\\') {
        Out.push_back(Value);
        continue;
      }
      if (AtEnd()) {
        return false;
      }
      const char Escape = Text[Position++];
      switch (Escape) {
      case '"':
      case '\\':
      case '/':
        Out.push_back(Escape);
        break;
      case 'b':
        Out.push_back('\b');
        break;
      case 'f':
        Out.push_back('\f');
        break;
      case 'n':
        Out.push_back('\n');
        break;
      case 'r':
        Out.push_back('\r');
        break;
      case 't':
        Out.push_back('\t');
        break;
      case 'u': {
        if (Text.size() - Position < 4U) {
          return false;
        }
        for (int Index = 0; Index < 4; ++Index) {
          if (!std::isxdigit(static_cast<unsigned char>(
                  Text[Position + static_cast<std::size_t>(Index)]))) {
            return false;
          }
        }
        Position += 4U;
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }

  bool ParseNumber() {
    if (!AtEnd() && Text[Position] == '-') {
      ++Position;
    }
    if (AtEnd()) {
      return false;
    }
    if (Text[Position] == '0') {
      ++Position;
    } else if (std::isdigit(static_cast<unsigned char>(Text[Position])) !=
               0) {
      while (!AtEnd() &&
             std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
        ++Position;
      }
    } else {
      return false;
    }
    if (!AtEnd() && Text[Position] == '.') {
      ++Position;
      if (AtEnd() || std::isdigit(static_cast<unsigned char>(
                         Text[Position])) == 0) {
        return false;
      }
      while (!AtEnd() &&
             std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
        ++Position;
      }
    }
    if (!AtEnd() && (Text[Position] == 'e' || Text[Position] == 'E')) {
      ++Position;
      if (!AtEnd() && (Text[Position] == '+' || Text[Position] == '-')) {
        ++Position;
      }
      if (AtEnd() || std::isdigit(static_cast<unsigned char>(
                         Text[Position])) == 0) {
        return false;
      }
      while (!AtEnd() &&
             std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
        ++Position;
      }
    }
    return true;
  }

  bool ParseLiteral(std::string_view Literal) {
    if (Text.substr(Position).starts_with(Literal)) {
      Position += Literal.size();
      return true;
    }
    return false;
  }

  void SkipWhitespace() {
    while (!AtEnd()) {
      const char Value = Text[Position];
      if (Value != ' ' && Value != '\t' && Value != '\n' && Value != '\r') {
        return;
      }
      ++Position;
    }
  }

  bool Consume(char Expected) {
    if (!AtEnd() && Text[Position] == Expected) {
      ++Position;
      return true;
    }
    return false;
  }

  bool AtEnd() const noexcept { return Position >= Text.size(); }

  std::string_view Text;
  std::size_t Position{0};
};

enum class JsonKind : std::uint8_t { Object, Array, String, Number, Boolean, Null };

struct JsonNode final {
  JsonKind Kind{JsonKind::Null};
  std::string Scalar;
  std::vector<std::pair<std::string, JsonNode>> Properties;
  std::vector<JsonNode> Elements;
};

class JsonDocumentParser final {
public:
  explicit JsonDocumentParser(std::string_view TextValue) : Text(TextValue) {}

  bool Parse(JsonNode& Root) {
    SkipWhitespace();
    if (!ParseValue(Root, 0U)) {
      return false;
    }
    SkipWhitespace();
    return AtEnd();
  }

private:
  static constexpr std::size_t MaximumDepth = 32U;

  bool ParseValue(JsonNode& Value, std::size_t Depth) {
    if (Depth > MaximumDepth) {
      return false;
    }
    SkipWhitespace();
    if (AtEnd()) {
      return false;
    }
    switch (Text[Position]) {
    case '{':
      Value.Kind = JsonKind::Object;
      return ParseObject(Value, Depth + 1U);
    case '[':
      Value.Kind = JsonKind::Array;
      return ParseArray(Value, Depth + 1U);
    case '"':
      Value.Kind = JsonKind::String;
      return ParseString(Value.Scalar);
    case 't':
      Value.Kind = JsonKind::Boolean;
      Value.Scalar = "true";
      return ParseLiteral("true");
    case 'f':
      Value.Kind = JsonKind::Boolean;
      Value.Scalar = "false";
      return ParseLiteral("false");
    case 'n':
      Value.Kind = JsonKind::Null;
      Value.Scalar = "null";
      return ParseLiteral("null");
    default:
      Value.Kind = JsonKind::Number;
      return ParseNumber(Value.Scalar);
    }
  }

  bool ParseObject(JsonNode& Object, std::size_t Depth) {
    if (!Consume('{')) {
      return false;
    }
    SkipWhitespace();
    if (Consume('}')) {
      return true;
    }
    while (true) {
      SkipWhitespace();
      std::string Key;
      if (!ParseString(Key) ||
          std::any_of(Object.Properties.begin(), Object.Properties.end(),
                      [&Key](const auto& Member) {
                        return Member.first == Key;
                      })) {
        return false;
      }
      SkipWhitespace();
      if (!Consume(':')) {
        return false;
      }
      JsonNode MemberValue;
      if (!ParseValue(MemberValue, Depth)) {
        return false;
      }
      Object.Properties.emplace_back(std::move(Key),
                                     std::move(MemberValue));
      SkipWhitespace();
      if (Consume(',')) {
        continue;
      }
      return Consume('}');
    }
  }

  bool ParseArray(JsonNode& Array, std::size_t Depth) {
    if (!Consume('[')) {
      return false;
    }
    SkipWhitespace();
    if (Consume(']')) {
      return true;
    }
    while (true) {
      JsonNode Element;
      if (!ParseValue(Element, Depth)) {
        return false;
      }
      Array.Elements.push_back(std::move(Element));
      SkipWhitespace();
      if (Consume(',')) {
        continue;
      }
      return Consume(']');
    }
  }

  bool ParseString(std::string& Output) {
    if (!Consume('"')) {
      return false;
    }
    while (!AtEnd()) {
      const char Value = Text[Position++];
      if (Value == '"') {
        return true;
      }
      if (static_cast<unsigned char>(Value) < 0x20U) {
        return false;
      }
      if (Value != '\\') {
        Output.push_back(Value);
        continue;
      }
      if (AtEnd()) {
        return false;
      }
      const char Escape = Text[Position++];
      switch (Escape) {
      case '"':
      case '\\':
      case '/':
        Output.push_back(Escape);
        break;
      case 'b':
        Output.push_back('\b');
        break;
      case 'f':
        Output.push_back('\f');
        break;
      case 'n':
        Output.push_back('\n');
        break;
      case 'r':
        Output.push_back('\r');
        break;
      case 't':
        Output.push_back('\t');
        break;
      case 'u': {
        std::uint32_t CodePoint{};
        if (!ParseUnicodeCodePoint(CodePoint)) {
          return false;
        }
        AppendUtf8(Output, CodePoint);
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }

  bool ParseUnicodeCodePoint(std::uint32_t& CodePoint) {
    std::uint16_t First{};
    if (!ParseHexCodeUnit(First)) {
      return false;
    }
    if (First >= 0xDC00U && First <= 0xDFFFU) {
      return false;
    }
    if (First < 0xD800U || First > 0xDBFFU) {
      CodePoint = First;
      return true;
    }
    if (Text.size() - Position < 6U || Text[Position] != '\\' ||
        Text[Position + 1U] != 'u') {
      return false;
    }
    Position += 2U;
    std::uint16_t Second{};
    if (!ParseHexCodeUnit(Second) || Second < 0xDC00U ||
        Second > 0xDFFFU) {
      return false;
    }
    CodePoint = 0x10000U +
                ((static_cast<std::uint32_t>(First) - 0xD800U) << 10U) +
                (static_cast<std::uint32_t>(Second) - 0xDC00U);
    return true;
  }

  bool ParseHexCodeUnit(std::uint16_t& CodeUnit) {
    if (Text.size() - Position < 4U) {
      return false;
    }
    std::uint16_t Value{};
    for (std::size_t Index = 0; Index < 4U; ++Index) {
      const char Digit = Text[Position + Index];
      std::uint16_t Nibble{};
      if (Digit >= '0' && Digit <= '9') {
        Nibble = static_cast<std::uint16_t>(Digit - '0');
      } else if (Digit >= 'a' && Digit <= 'f') {
        Nibble = static_cast<std::uint16_t>(Digit - 'a' + 10);
      } else if (Digit >= 'A' && Digit <= 'F') {
        Nibble = static_cast<std::uint16_t>(Digit - 'A' + 10);
      } else {
        return false;
      }
      Value = static_cast<std::uint16_t>((Value << 4U) | Nibble);
    }
    Position += 4U;
    CodeUnit = Value;
    return true;
  }

  static void AppendUtf8(std::string& Output, std::uint32_t CodePoint) {
    if (CodePoint <= 0x7FU) {
      Output.push_back(static_cast<char>(CodePoint));
    } else if (CodePoint <= 0x7FFU) {
      Output.push_back(static_cast<char>(0xC0U | (CodePoint >> 6U)));
      Output.push_back(static_cast<char>(0x80U | (CodePoint & 0x3FU)));
    } else if (CodePoint <= 0xFFFFU) {
      Output.push_back(static_cast<char>(0xE0U | (CodePoint >> 12U)));
      Output.push_back(
          static_cast<char>(0x80U | ((CodePoint >> 6U) & 0x3FU)));
      Output.push_back(static_cast<char>(0x80U | (CodePoint & 0x3FU)));
    } else {
      Output.push_back(static_cast<char>(0xF0U | (CodePoint >> 18U)));
      Output.push_back(
          static_cast<char>(0x80U | ((CodePoint >> 12U) & 0x3FU)));
      Output.push_back(
          static_cast<char>(0x80U | ((CodePoint >> 6U) & 0x3FU)));
      Output.push_back(static_cast<char>(0x80U | (CodePoint & 0x3FU)));
    }
  }

  bool ParseNumber(std::string& Output) {
    const std::size_t Start = Position;
    if (!AtEnd() && Text[Position] == '-') {
      ++Position;
    }
    if (AtEnd()) {
      return false;
    }
    if (Text[Position] == '0') {
      ++Position;
      if (!AtEnd() && std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
        return false;
      }
    } else if (std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
      while (!AtEnd() &&
             std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
        ++Position;
      }
    } else {
      return false;
    }
    if (!AtEnd() && Text[Position] == '.') {
      ++Position;
      if (AtEnd() || std::isdigit(static_cast<unsigned char>(Text[Position])) == 0) {
        return false;
      }
      while (!AtEnd() &&
             std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
        ++Position;
      }
    }
    if (!AtEnd() && (Text[Position] == 'e' || Text[Position] == 'E')) {
      ++Position;
      if (!AtEnd() && (Text[Position] == '+' || Text[Position] == '-')) {
        ++Position;
      }
      if (AtEnd() || std::isdigit(static_cast<unsigned char>(Text[Position])) == 0) {
        return false;
      }
      while (!AtEnd() &&
             std::isdigit(static_cast<unsigned char>(Text[Position])) != 0) {
        ++Position;
      }
    }
    Output.assign(Text.substr(Start, Position - Start));
    return true;
  }

  bool ParseLiteral(std::string_view Literal) {
    if (!Text.substr(Position).starts_with(Literal)) {
      return false;
    }
    Position += Literal.size();
    return true;
  }

  void SkipWhitespace() {
    while (!AtEnd()) {
      const char Value = Text[Position];
      if (Value != ' ' && Value != '\t' && Value != '\n' && Value != '\r') {
        return;
      }
      ++Position;
    }
  }

  bool Consume(char Expected) {
    if (!AtEnd() && Text[Position] == Expected) {
      ++Position;
      return true;
    }
    return false;
  }

  bool AtEnd() const noexcept { return Position >= Text.size(); }

  std::string_view Text;
  std::size_t Position{0};
};

const JsonNode* FindProperty(const JsonNode& Object,
                             std::string_view Name) noexcept {
  if (Object.Kind != JsonKind::Object) {
    return nullptr;
  }
  const auto Match = std::find_if(
      Object.Properties.begin(), Object.Properties.end(),
      [Name](const auto& Member) { return Member.first == Name; });
  return Match == Object.Properties.end() ? nullptr : &Match->second;
}

enum class PrimitiveSchemaKind : std::uint8_t {
  String,
  Number,
  Integer,
  Boolean,
  Null,
};

struct SupportedToolSchema final {
  std::vector<std::pair<std::string, PrimitiveSchemaKind>> Properties;
  std::vector<std::string> Required;
  bool AdditionalProperties{false};
};

std::optional<PrimitiveSchemaKind> ParsePrimitiveKind(
    const JsonNode& Type) noexcept {
  if (Type.Kind != JsonKind::String) {
    return std::nullopt;
  }
  if (Type.Scalar == "string") {
    return PrimitiveSchemaKind::String;
  }
  if (Type.Scalar == "number") {
    return PrimitiveSchemaKind::Number;
  }
  if (Type.Scalar == "integer") {
    return PrimitiveSchemaKind::Integer;
  }
  if (Type.Scalar == "boolean") {
    return PrimitiveSchemaKind::Boolean;
  }
  if (Type.Scalar == "null") {
    return PrimitiveSchemaKind::Null;
  }
  return std::nullopt;
}

bool ParseSupportedToolSchema(std::string_view JsonText,
                              SupportedToolSchema& Schema) {
  JsonNode Root;
  JsonDocumentParser Parser(JsonText);
  if (!Parser.Parse(Root) || Root.Kind != JsonKind::Object ||
      Root.Properties.size() != 4U) {
    return false;
  }
  const JsonNode* RootType = FindProperty(Root, "type");
  const JsonNode* Properties = FindProperty(Root, "properties");
  const JsonNode* Required = FindProperty(Root, "required");
  const JsonNode* Additional = FindProperty(Root, "additionalProperties");
  if (RootType == nullptr || RootType->Kind != JsonKind::String ||
      RootType->Scalar != "object" || Properties == nullptr ||
      Properties->Kind != JsonKind::Object || Required == nullptr ||
      Required->Kind != JsonKind::Array || Additional == nullptr ||
      Additional->Kind != JsonKind::Boolean) {
    return false;
  }
  Schema.AdditionalProperties = Additional->Scalar == "true";
  for (const auto& [Name, Definition] : Properties->Properties) {
    if (!IsValidSchemaField(Name) || Definition.Kind != JsonKind::Object ||
        Definition.Properties.size() != 1U) {
      return false;
    }
    const JsonNode* Type = FindProperty(Definition, "type");
    if (Type == nullptr) {
      return false;
    }
    const std::optional<PrimitiveSchemaKind> Parsed = ParsePrimitiveKind(*Type);
    if (!Parsed.has_value()) {
      return false;
    }
    Schema.Properties.emplace_back(Name, *Parsed);
  }
  for (const JsonNode& RequiredItem : Required->Elements) {
    if (RequiredItem.Kind != JsonKind::String ||
        !IsValidSchemaField(RequiredItem.Scalar) ||
        std::find(Schema.Required.begin(), Schema.Required.end(),
                  RequiredItem.Scalar) != Schema.Required.end()) {
      return false;
    }
    const bool Declared = std::any_of(
        Schema.Properties.begin(), Schema.Properties.end(),
        [&RequiredItem](const auto& Property) {
          return Property.first == RequiredItem.Scalar;
        });
    if (!Declared) {
      return false;
    }
    Schema.Required.push_back(RequiredItem.Scalar);
  }
  return true;
}

bool MatchesPrimitive(const JsonNode& Value,
                      PrimitiveSchemaKind Expected) noexcept {
  switch (Expected) {
  case PrimitiveSchemaKind::String:
    return Value.Kind == JsonKind::String;
  case PrimitiveSchemaKind::Number:
    return Value.Kind == JsonKind::Number;
  case PrimitiveSchemaKind::Integer:
    return Value.Kind == JsonKind::Number &&
           Value.Scalar.find_first_of(".eE") == std::string::npos;
  case PrimitiveSchemaKind::Boolean:
    return Value.Kind == JsonKind::Boolean;
  case PrimitiveSchemaKind::Null:
    return Value.Kind == JsonKind::Null;
  }
  return false;
}

bool SameFields(const std::vector<std::string>& Left,
                const std::vector<std::string>& Right) {
  if (Left.size() != Right.size()) {
    return false;
  }
  return std::all_of(Left.begin(), Left.end(), [&Right](const std::string& Field) {
    return std::find(Right.begin(), Right.end(), Field) != Right.end();
  });
}

} // namespace

bool BusinessActionRegistry::IsValidActionName(
    std::string_view Name) noexcept {
  if (Name.empty() || Name.size() > 64U) {
    return false;
  }
  if (Name.front() < 'A' || Name.front() > 'Z') {
    return false;
  }
  return std::all_of(Name.begin(), Name.end(), [](char Value) {
    const auto Character = static_cast<unsigned char>(Value);
    return std::isalnum(Character) != 0;
  });
}

bool BusinessActionRegistry::IsForbiddenToolName(std::string_view Name) {
  const std::string Normalized = ToLowerAscii(Name);
  static constexpr std::string_view ExactForbidden[] = {
      "shell", "exec", "execute", "command", "terminal", "cmd",
      "powershell", "eval", "script", "process", "system", "os",
      "mcp", "browser",
  };
  for (const std::string_view Forbidden : ExactForbidden) {
    if (Normalized == Forbidden) {
      return true;
    }
  }
  if (StartsWithAny(Normalized,
                    {"shell", "exec", "command", "terminal", "file",
                     "filesystem", "network", "http", "mcp", "browser"})) {
    return true;
  }
  // Reject generic tool words in any PascalCase position, such as ReadFile or
  // OpenBrowser, without rejecting legitimate words like Profile.
  static constexpr std::string_view ForbiddenWords[] = {
      "shell",  "exec",    "execute",  "eval",   "script",
      "command", "terminal", "cmd",     "powershell",
      "process", "system",  "os",       "mcp",    "browser",
      "file",    "filesystem", "network", "http", "https",
      "socket",  "tcp",     "udp",
  };
  const std::vector<std::string> Words = SplitPascalWords(Name);
  return std::any_of(
      Words.begin(), Words.end(), [](const std::string& Word) {
        return std::any_of(
            std::begin(ForbiddenWords), std::end(ForbiddenWords),
            [&Word](std::string_view Forbidden) {
              return Word == Forbidden;
            });
      });
}

bool BusinessActionRegistry::ValidateArguments(
    std::string_view JsonText,
    const std::vector<std::string>& RequiredFields) noexcept {
  try {
    std::vector<std::string> Keys;
    JsonParser Parser(JsonText);
    if (!Parser.ParseRootObject(Keys)) {
      return false;
    }
    return std::all_of(
        RequiredFields.begin(), RequiredFields.end(),
        [&Keys](const std::string& Required) {
          return std::find(Keys.begin(), Keys.end(), Required) != Keys.end();
        });
  } catch (...) {
    return false;
  }
}

bool BusinessActionRegistry::IsValidToolDefinition(
    const Providers::ToolDefinition& Tool) noexcept {
  try {
    if (!IsValidActionName(Tool.Name) || IsForbiddenToolName(Tool.Name)) {
      return false;
    }
    SupportedToolSchema Schema;
    return ParseSupportedToolSchema(Tool.Parameters.JsonText, Schema);
  } catch (...) {
    return false;
  }
}

bool BusinessActionRegistry::ValidateArguments(
    const Providers::JsonValue& Arguments,
    const Providers::ToolDefinition& Tool) noexcept {
  try {
    SupportedToolSchema Schema;
    if (!IsValidToolDefinition(Tool) ||
        !ParseSupportedToolSchema(Tool.Parameters.JsonText, Schema)) {
      return false;
    }
    JsonNode Root;
    JsonDocumentParser Parser(Arguments.JsonText);
    if (!Parser.Parse(Root) || Root.Kind != JsonKind::Object) {
      return false;
    }
    for (const std::string& Required : Schema.Required) {
      if (FindProperty(Root, Required) == nullptr) {
        return false;
      }
    }
    for (const auto& [Name, Value] : Root.Properties) {
      const auto Definition = std::find_if(
          Schema.Properties.begin(), Schema.Properties.end(),
          [&Name](const auto& Property) { return Property.first == Name; });
      if (Definition == Schema.Properties.end()) {
        if (!Schema.AdditionalProperties) {
          return false;
        }
        continue;
      }
      if (!MatchesPrimitive(Value, Definition->second)) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<BusinessActionRegistrationError>
BusinessActionRegistry::Register(BusinessActionDefinition Definition) {
  if (!IsValidActionName(Definition.Name)) {
    return RegistrationError{RegistrationErrorCode::InvalidActionName};
  }
  if (IsForbiddenToolName(Definition.Name)) {
    return RegistrationError{RegistrationErrorCode::ForbiddenActionName};
  }
  if (Find(Definition.Name) != nullptr) {
    return RegistrationError{RegistrationErrorCode::DuplicateAction};
  }
  const bool HasAsyncHandler = static_cast<bool>(Definition.AsyncHandler);
  if (!Definition.Handler && !HasAsyncHandler) {
    return RegistrationError{RegistrationErrorCode::EmptyHandler};
  }

  if (HasAsyncHandler) {
    if (Definition.EffectClass !=
        BusinessActionEffectClass::InProcessReadOnly) {
      return RegistrationError{
          RegistrationErrorCode::IneligibleEffectClass};
    }
    if (Definition.ModelTool.Name != Definition.Name ||
        !IsValidToolDefinition(Definition.ModelTool)) {
      return RegistrationError{
          RegistrationErrorCode::InvalidToolDefinition};
    }
    SupportedToolSchema Schema;
    if (!ParseSupportedToolSchema(Definition.ModelTool.Parameters.JsonText,
                                  Schema) ||
        !SameFields(Definition.RequiredArgumentFields, Schema.Required)) {
      return RegistrationError{
          RegistrationErrorCode::InvalidToolDefinition};
    }
    std::unordered_set<std::string> SeenCapabilities;
    for (const std::string& Capability : Definition.RequiredCapabilities) {
      if (Capability.empty() ||
          !SeenCapabilities.emplace(Capability).second) {
        return RegistrationError{
            RegistrationErrorCode::InvalidSchemaField};
      }
    }
  }

  std::unordered_set<std::string> SeenFields;
  for (const std::string& Field : Definition.RequiredArgumentFields) {
    if (!IsValidSchemaField(Field) ||
        !SeenFields.insert(Field).second) {
      return RegistrationError{RegistrationErrorCode::InvalidSchemaField};
    }
  }

  Definitions.push_back(std::move(Definition));
  return std::nullopt;
}

const BusinessActionDefinition* BusinessActionRegistry::Find(
    std::string_view Name) const noexcept {
  const auto Match = std::find_if(
      Definitions.begin(), Definitions.end(),
      [Name](const BusinessActionDefinition& Definition) {
        return Definition.Name == Name;
      });
  if (Match == Definitions.end()) {
    return nullptr;
  }
  return &*Match;
}

std::vector<const BusinessActionDefinition*>
BusinessActionRegistry::GetAsyncDefinitions() const {
  std::vector<const BusinessActionDefinition*> AsyncDefinitions;
  for (const BusinessActionDefinition& Definition : Definitions) {
    if (Definition.AsyncHandler &&
        Definition.EffectClass ==
            BusinessActionEffectClass::InProcessReadOnly &&
        Definition.ModelTool.Name == Definition.Name) {
      AsyncDefinitions.push_back(&Definition);
    }
  }
  return AsyncDefinitions;
}

} // namespace EmulativeDialog::Core
