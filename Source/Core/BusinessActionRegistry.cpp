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
  if (!Definition.Handler) {
    return RegistrationError{RegistrationErrorCode::EmptyHandler};
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

} // namespace EmulativeDialog::Core
