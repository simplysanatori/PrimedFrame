#include "port_json.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace {

bool Parse(const std::string& text, PortJson::Value& value, size_t* offset = nullptr,
           const char** reason = nullptr) {
  size_t localOffset = 999999;
  const char* localReason = "unchanged";
  const bool parsed = PortJson::Parse(text, value, localOffset, &localReason);
  if (offset != nullptr)
    *offset = localOffset;
  if (reason != nullptr)
    *reason = localReason;
  return parsed;
}

} // namespace

int main() {
  using PortJson::Value;
  Value value;
  CHECK_MSG(Parse("null", value) && value.IsNull(), "null literal");
  CHECK_MSG(Parse("true", value) && value.IsBool() && value.AsBool(), "true literal");
  CHECK_MSG(Parse("false", value) && value.IsBool() && !value.AsBool(true), "false literal");
  CHECK_MSG(Parse("  \t\r\n[true,false,null,-7,0,1.25,3e2,\"s\",[],{}] \n", value),
            "array, nested values, and JSON whitespace");
  CHECK_MSG(value.IsArray() && value.Size() == 10, "array type and size");
  CHECK_MSG(value.AsArray()[3].AsInt() == -7 && value.AsArray()[5].AsNumber() == 1.25 &&
                value.AsArray()[6].AsNumber() == 300.0,
            "negative, fractional, and exponent numbers");
  std::string atDepthLimit(128, '[');
  atDepthLimit.push_back('0');
  atDepthLimit.append(128, ']');
  CHECK_MSG(Parse(atDepthLimit, value), "nesting at the 128-level limit is accepted");

  const std::string escaped = R"("\"\\\/\b\f\n\r\t\u0041\uD83D\uDE00")";
  CHECK_MSG(Parse(escaped, value) && value.IsString(), "all string escapes and Unicode escapes");
  const std::string expectedEscapes = std::string("\"\\/\b\f\n\r\tA") + "\xF0\x9F\x98\x80";
  CHECK_MSG(value.AsString() == expectedEscapes, "escaped string decodes to expected UTF-8 bytes");
  CHECK_MSG(value.Size() == expectedEscapes.size(), "string size counts UTF-8 bytes");

  CHECK_MSG(Parse("{\"second\":2,\"first\":1,\"str\":\"yes\",\"n\":null}", value),
            "object syntax");
  CHECK_MSG(value.IsObject() && value.Size() == 4 && value.AsObject()[0].first == "second" &&
                value.AsObject()[1].first == "first",
            "object insertion order and member count");
  CHECK_MSG(value.Find("first") != nullptr && value.Find("first")->AsInt() == 1 &&
                value.Find("missing") == nullptr && value.Find(nullptr) == nullptr,
            "Find lookup and missing members");
  CHECK_MSG(value.StringOr("str", "fallback") == "yes" &&
                value.StringOr("n", "fallback") == "fallback" &&
                value.StringOr("missing", "fallback") == "fallback" &&
                value.StringOr("str", nullptr) == "yes",
            "StringOr string-only lookup and fallback");
  CHECK_MSG(value.AsBool(true) && value.AsNumber(42.0) == 42.0 && value.AsArray().empty() &&
                value.AsString().empty() && Value::MakeBool(true).AsObject().empty(),
            "wrong-type accessors return fallbacks or empty values");

  CHECK_MSG(Value::MakeNumber(12.9).AsInt(-5) == 12 &&
                Value::MakeNumber(-12.9).AsInt(-5) == -12 &&
                Value::MakeNumber(std::numeric_limits<double>::infinity()).AsInt(-5) == -5 &&
                Value::MakeNumber(std::ldexp(1.0, 63)).AsInt(-5) == -5 &&
                Value::MakeNumber(-std::ldexp(1.0, 63)).AsInt(-5) == std::numeric_limits<int64_t>::min(),
            "AsInt truncation and int64 range handling");
  CHECK_MSG(PortJson::TypeName(Value::Type::Null) == std::string("null") &&
                PortJson::TypeName(Value::Type::Bool) == std::string("bool") &&
                PortJson::TypeName(Value::Type::Number) == std::string("number") &&
                PortJson::TypeName(Value::Type::String) == std::string("string") &&
                PortJson::TypeName(Value::Type::Array) == std::string("array") &&
                PortJson::TypeName(Value::Type::Object) == std::string("object"),
            "TypeName values");
  CHECK_MSG(Value().Size() == 0 && Value::MakeBool(true).Size() == 0 &&
                Value::MakeNumber(1).Size() == 0,
            "non-container sizes are zero");

  const std::string invalid[] = {
      "", "NaN", "Infinity", "+1", ".5", "1.", "0x10", "01", "//comment\nnull",
      "/*comment*/null",
      "[1,]", "{\"x\":1,}", "[1 2]", "{\"x\" 1}", "true false", "\"bad\nstring\"",
      R"("\q")", "\"unterminated", R"("\uD800")", R"("\uDC00")",
      R"("\uD800\u0041")"};
  for (const std::string& text : invalid) {
    size_t offset = 0;
    const char* reason = nullptr;
    const bool parsed = Parse(text, value, &offset, &reason);
    CHECK_MSG(!parsed, "invalid JSON form must be rejected");
    CHECK_MSG(offset <= text.size(), "failure offset should identify a byte in/at the input");
    CHECK_MSG(reason != nullptr && std::string(reason) != "unchanged", "failure reason is set");
  }
  size_t offset = 0;
  CHECK_MSG(!Parse("", value, &offset) && offset == 0, "empty input reports offset zero");
  CHECK_MSG(!Parse("01", value, &offset) && offset == 1, "leading-zero error points at the extra digit");
  CHECK_MSG(!Parse("true false", value, &offset) && offset == 5,
            "trailing-data error points at the first extra byte");
  CHECK_MSG(!Parse(R"("\q")", value, &offset) && offset == 2,
        "bad escape error points at the escape character");

  std::string tooDeep(129, '[');
  tooDeep.append(129, ']');
  size_t deepOffset = 0;
  const char* deepReason = nullptr;
  CHECK_MSG(!Parse(tooDeep, value, &deepOffset, &deepReason) && deepReason != nullptr &&
                std::string(deepReason) == "nesting too deep" && deepOffset == 128,
            "nesting limit has the specified error");

  size_t untouchedOffset = 321;
  const char* untouchedReason = "unchanged";
  CHECK_MSG(PortJson::Parse("0", value, untouchedOffset, &untouchedReason) &&
                untouchedOffset == 321 && untouchedReason == std::string("unchanged"),
            "success leaves error outputs untouched");

  if (port_test::Failures() == 0) {
    std::puts("[json-tests] passed");
  }
  return PORT_TEST_RESULT();
}
