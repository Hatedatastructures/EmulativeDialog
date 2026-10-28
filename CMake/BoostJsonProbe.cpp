#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include <boost/system/error_code.hpp>

#include <cstdio>
#include <string>

namespace {

int RunProbe() {
  boost::system::error_code Error;

  // Invalid input must be rejected instead of becoming a partial value.
  boost::json::parse("{\"unterminated\":", Error);
  if (!Error) {
    std::fputs("invalid JSON was accepted\n", stderr);
    return 2;
  }

  boost::json::value Parsed = boost::json::parse(
      "{\"name\":\"synthetic-\\u4e2d\\u6587\",\"count\":36,"
      "\"items\":[1,2,3],\"nested\":{\"ok\":true}}",
      Error);
  if (Error) {
    std::fprintf(stderr, "valid JSON failed to parse: %s\n",
                 Error.message().c_str());
    return 3;
  }

  const boost::json::object* Object = Parsed.if_object();
  if (Object == nullptr) {
    std::fputs("parsed value is not an object\n", stderr);
    return 4;
  }
  const boost::json::value* Name = Object->if_contains("name");
  const boost::json::value* Count = Object->if_contains("count");
  const boost::json::value* Items = Object->if_contains("items");
  if (Name == nullptr || !Name->is_string() || Count == nullptr ||
      !Count->is_int64() || Items == nullptr || !Items->is_array()) {
    std::fputs("parsed object fields have unexpected types\n", stderr);
    return 5;
  }

  const boost::json::array& ItemArray = Items->as_array();
  if (ItemArray.size() != 3) {
    std::fputs("parsed array has unexpected size\n", stderr);
    return 6;
  }

  const std::int64_t Sum = Count->as_int64() +
                           ItemArray.at(0).as_int64() +
                           ItemArray.at(1).as_int64() +
                           ItemArray.at(2).as_int64();
  if (Sum != 42) {
    std::fprintf(stderr, "unexpected numeric result: %lld\n",
                 static_cast<long long>(Sum));
    return 7;
  }

  // Serialization must round-trip through a fresh parse.
  const std::string Encoded = boost::json::serialize(Parsed);
  const boost::json::value Reparsed = boost::json::parse(Encoded, Error);
  if (Error || Reparsed != Parsed) {
    std::fputs("serialize/parse round trip diverged\n", stderr);
    return 8;
  }

  boost::json::object Built;
  Built["text"] = "synthetic";
  Built["numbers"] = boost::json::array{1, 2, 3};
  Built["ratio"] = 1.5;
  const std::string BuiltEncoded = boost::json::serialize(Built);
  const boost::json::value BuiltReparsed =
      boost::json::parse(BuiltEncoded, Error);
  if (Error || BuiltReparsed != boost::json::value(Built)) {
    std::fputs("constructed object round trip diverged\n", stderr);
    return 9;
  }

  std::printf("BoostJsonCxx20 parse/serialize result=%lld\n",
              static_cast<long long>(Sum));
  return 0;
}

} // namespace

int main() { return RunProbe(); }
