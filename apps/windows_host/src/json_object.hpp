#pragma once
#include "support.hpp"
#include <iomanip>

namespace host {
inline std::string jsonObject(const Json& value) {
  if (const auto* array = std::get_if<Json::Array>(&value.value)) { std::string out = "["; for (const auto& child : *array) { if (out.size() > 1) out += ','; out += jsonObject(child); } return out + ']'; }
  if (const auto* object = std::get_if<Json::Object>(&value.value)) { std::string out = "{"; for (const auto& [key, child] : *object) { if (out.size() > 1) out += ','; out += jsonQuote(key) + ':' + jsonObject(child); } return out + '}'; }
  if (const auto* text = std::get_if<std::string>(&value.value)) return jsonQuote(*text);
  if (const auto* number = std::get_if<double>(&value.value)) { std::ostringstream out; out << std::setprecision(16) << *number; return out.str(); }
  if (const auto* flag = std::get_if<bool>(&value.value)) return *flag ? "true" : "false";
  return "null";
}
}
