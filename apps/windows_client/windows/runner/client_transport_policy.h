#pragma once
#include "../../../windows_host/src/broker_origin.hpp"
#include <map>
#include <optional>
#include <string>

namespace multitap {
// Same pure canonical parser as Host. Neither parser nor this DTO reads the OS.
struct ClientTransportSettings {
  std::string origin, httpConsentOrigin;
  int version = 0;
};
inline std::optional<ClientTransportSettings> ClientTransportSettingsParse(const std::string& json) {
  if (json.empty() || json.size() > 4096) return std::nullopt;
  size_t at = 0;
  auto space = [&] { while (at < json.size() && (json[at]==' ' || json[at]=='\r' || json[at]=='\n' || json[at]=='\t')) ++at; };
  auto quoted = [&]() -> std::optional<std::string> {
    space(); if (at == json.size() || json[at++] != '"') return std::nullopt;
    std::string value;
    while (at < json.size()) {
      const unsigned char c = static_cast<unsigned char>(json[at++]);
      if (c == '"') return value;
      // The app writes canonical ASCII origins/keys, with no JSON escapes.
      if (c < 32 || c >= 127 || c == '\\') return std::nullopt;
      value += static_cast<char>(c);
    }
    return std::nullopt;
  };
  space(); if (at == json.size() || json[at++] != '{') return std::nullopt;
  std::map<std::string,std::string> fields;
  for (;;) {
    const auto key = quoted(); if (!key || fields.count(*key)) return std::nullopt;
    space(); if (at == json.size() || json[at++] != ':') return std::nullopt;
    if (*key == "version") {
      space(); if (at == json.size() || json[at++] != '2') return std::nullopt;
      fields.emplace(*key,"2");
    } else {
      if (*key != "serverOrigin" && *key != "httpConsentOrigin") return std::nullopt;
      const auto value = quoted(); if (!value) return std::nullopt;
      fields.emplace(*key,*value);
    }
    space(); if (at == json.size()) return std::nullopt;
    if (json[at++] == '}') break;
    if (json[at-1] != ',') return std::nullopt;
  }
  space(); if (at != json.size() || !fields.count("serverOrigin")) return std::nullopt;
  try {
    const auto origin = host::transport::parse(fields.at("serverOrigin"));
    if (origin.text != fields.at("serverOrigin")) return std::nullopt;
    if (fields.size() == 1 && origin.secure()) return ClientTransportSettings{origin.text,{},1};
    if (!fields.count("version") || fields.size() != (origin.secure() ? 2u : 3u)) return std::nullopt;
    if (origin.secure()) {
      if (fields.count("httpConsentOrigin")) return std::nullopt;
      return ClientTransportSettings{origin.text,{},2};
    }
    if (!fields.count("httpConsentOrigin") || fields.at("httpConsentOrigin") != origin.text) return std::nullopt;
    return ClientTransportSettings{origin.text,origin.text,2};
  } catch (...) { return std::nullopt; }
}
inline bool ClientTransportSyntaxValid(const std::string& origin) {
  try { return host::transport::parse(origin).text == origin; } catch (...) { return false; }
}
inline bool ClientTransportAuthorized(const std::string& origin,const std::optional<ClientTransportSettings>& stored) {
  try {
    const auto parsed = host::transport::parse(origin);
    return parsed.text == origin && (parsed.secure() || (stored && stored->version == 2 &&
      stored->origin == origin && stored->httpConsentOrigin == origin));
  } catch (...) { return false; }
}
inline bool ClientLaunchTransportAllowed(const std::string& origin,const std::string& consent,
    const std::optional<ClientTransportSettings>& stored) {
  if (!ClientTransportAuthorized(origin,stored)) return false;
  return origin.rfind("https://",0) == 0 || consent == origin;
}
} // namespace multitap
