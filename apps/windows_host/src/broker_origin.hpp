#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Pure origin/path policy. No Windows, sockets, settings or secret access.
namespace host::transport {
struct Origin {
  std::string scheme, host, text;
  uint16_t port = 0;
  bool secure() const { return scheme == "https"; }
};
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline std::string lower(std::string value) {
  for (auto& c : value) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
  return value;
}
[[noreturn]] inline void invalid() { throw std::runtime_error("invalid_broker_origin"); }
inline std::string ipv6(const std::string& input) {
  const auto gap = input.find("::");
  if (gap != std::string::npos && input.find("::", gap + 2) != std::string::npos) invalid();
  auto part = [](const std::string& text) {
    std::vector<uint16_t> result;
    if (text.empty()) return result;
    size_t at = 0;
    for (;;) {
      const auto end = text.find(':', at);
      const auto word = text.substr(at, end == std::string::npos ? end : end - at);
      if(word.find('.')!=std::string::npos){
        if(end!=std::string::npos)invalid();std::array<unsigned,4> octets{};size_t position=0;
        for(size_t index=0;index<4;++index){
          const auto dot=word.find('.',position);const auto label=word.substr(position,dot==std::string::npos?dot:dot-position);
          if(label.empty()||label.size()>3||(label.size()>1&&label.front()=='0')||(index==3)!=(dot==std::string::npos))invalid();
          for(const auto c:label){if(!digit(c))invalid();octets[index]=octets[index]*10+unsigned(c-'0');}if(octets[index]>255)invalid();position=dot+1;
        }
        result.push_back(uint16_t((octets[0]<<8)|octets[1]));result.push_back(uint16_t((octets[2]<<8)|octets[3]));break;
      }
      if (word.empty() || word.size() > 4) invalid();
      uint16_t value = 0;
      for (const auto c : word) {
        const auto ch = lower(std::string(1, c))[0];
        if (!digit(ch) && (ch < 'a' || ch > 'f')) invalid();
        value = uint16_t(value * 16 + (digit(ch) ? ch - '0' : ch - 'a' + 10));
      }
      result.push_back(value);
      if (end == std::string::npos) break;
      at = end + 1;
    }
    return result;
  };
  auto words = part(input.substr(0, gap));
  if (gap == std::string::npos) { if (words.size() != 8) invalid(); }
  else {
    const auto tail = part(input.substr(gap + 2));
    if (words.size() + tail.size() >= 8) invalid();
    words.resize(8 - tail.size(), 0); words.insert(words.end(), tail.begin(), tail.end());
  }
  size_t best = 8, count = 0;
  for (size_t at = 0; at < 8;) {
    if (words[at]) { ++at; continue; }
    const auto start = at; while (at < 8 && !words[at]) ++at;
    if (at - start > count && at - start >= 2) { best = start; count = at - start; }
  }
  std::string out;
  constexpr char hex[] = "0123456789abcdef";
  for (size_t at = 0; at < 8;) {
    if (at == best) { out += "::"; at += count; continue; }
    if (!out.empty() && out.back() != ':') out += ':';
    const auto value = words[at++]; bool emitted = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
      const auto n = (value >> shift) & 15;
      if (n || emitted || !shift) { out += hex[n]; emitted = true; }
    }
  }
  return out;
}
inline Origin parse(const std::string& input) {
  if (input.empty() || input.size() > 512) invalid();
  for (const auto c : input) if (static_cast<unsigned char>(c) <= 32 || static_cast<unsigned char>(c) >= 127 || std::string("\\@?#%").find(c) != std::string::npos) invalid();
  const auto separator = input.find("://"); if (separator == std::string::npos) invalid();
  Origin out; out.scheme = lower(input.substr(0, separator));
  if (out.scheme != "http" && out.scheme != "https") invalid();
  auto authority = input.substr(separator + 3);
  const auto slash = authority.find('/');
  if (slash != std::string::npos) { if (authority.substr(slash) != "/") invalid(); authority.resize(slash); }
  if (authority.empty()) invalid();
  std::string port;
  if (authority.front() == '[') {
    const auto end = authority.find(']'); if (end == std::string::npos) invalid();
    out.host = ipv6(authority.substr(1, end - 1));
    const auto suffix = authority.substr(end + 1);
    if (!suffix.empty()) { if (suffix.front() != ':' || suffix.size() == 1) invalid(); port = suffix.substr(1); }
  } else {
    const auto colon = authority.find(':');
    out.host = lower(authority.substr(0, colon));
    if (colon != std::string::npos) { if (authority.find(':', colon + 1) != std::string::npos) invalid(); port = authority.substr(colon + 1); if (port.empty()) invalid(); }
    if (out.host.empty() || out.host.size() > 253) invalid();
    // Only canonical dotted decimal IPv4; WHATWG shorthand/octal/hex aliases
    // must not create a consent identity different from the server's URL parser.
    const auto labelsHost=out.host.back()=='.'?out.host.substr(0,out.host.size()-1):out.host;
    const auto last = labelsHost.substr(labelsHost.rfind('.') == std::string::npos ? 0 : labelsHost.rfind('.') + 1);
    const bool numeric = !last.empty() && (std::all_of(last.begin(), last.end(), digit) || last.rfind("0x", 0) == 0);
    size_t at = 0, labels = 0;
    for (;;) {
      const auto end = labelsHost.find('.', at); const auto label = labelsHost.substr(at, end == std::string::npos ? end : end - at);
      if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') invalid();
      for (const auto c : label) if (!digit(c) && (c < 'a' || c > 'z') && c != '-') invalid();
      if (numeric) {
        if (label.size() > 3 || (label.size() > 1 && label.front() == '0')) invalid();
        unsigned value = 0; for (const auto c : label) { if (!digit(c)) invalid(); value = value * 10 + unsigned(c - '0'); }
        if (value > 255) invalid();
      }
      ++labels; if (end == std::string::npos) break; at = end + 1;
    }
    if (numeric) { if(labels != 4)invalid();out.host=labelsHost; }
  }
  unsigned value = out.secure() ? 443u : 80u;
  if (!port.empty()) { value = 0; for (const auto c : port) { if (!digit(c)) invalid(); value = value * 10 + unsigned(c - '0'); if (value > 65535) invalid(); } if (!value) invalid(); }
  out.port = uint16_t(value);
  out.text = out.scheme + "://" + (out.host.find(':') == std::string::npos ? out.host : '[' + out.host + ']');
  if (out.port != (out.secure() ? 443 : 80)) out.text += ':' + std::to_string(out.port);
  return out;
}
inline Origin authorize(const std::string& input, const std::string& consent = {}) {
  auto result = parse(input);
  if (!result.secure() && (consent.empty() || parse(consent).text != result.text)) throw std::runtime_error("http_consent_required");
  return result;
}
inline bool relayMatches(const Origin& origin, const std::string& url, const std::string& path) {
  try {
    if(path.empty()||path.front()!='/')return false;
    for(const auto c:path)if(!digit(c)&&(c<'a'||c>'z')&&(c<'A'||c>'Z')&&c!='/'&&c!='-'&&c!='_')return false;
    const auto separator = url.find("://"); if (separator == std::string::npos) return false;
    const auto scheme = lower(url.substr(0, separator));
    if (scheme != (origin.secure() ? "wss" : "ws")) return false;
    const auto at = url.find('/', separator + 3);
    if (at == std::string::npos || url.substr(at) != path) return false;
    return parse((origin.secure() ? "https" : "http") + url.substr(separator, at - separator)).text == origin.text;
  } catch (...) { return false; }
}
inline bool relaySyntax(const std::string& url,const std::string& path) {
  try {
    const auto separator=url.find("://");if(separator==std::string::npos)return false;
    const auto scheme=lower(url.substr(0,separator));if(scheme!="ws"&&scheme!="wss")return false;
    const auto at=url.find('/',separator+3);if(at==std::string::npos)return false;
    const auto origin=parse((scheme=="wss"?"https":"http")+url.substr(separator,at-separator));
    return relayMatches(origin,url,path);
  }catch(...){return false;}
}
struct HealthProfile {
  bool exactTypes = false;
  std::string protocol, publicOrigin, scheme;
  bool allowInsecureHttp = false, directTls = false;
};
inline std::string healthFailure(const Origin& origin, const HealthProfile& value, bool present,
    bool modernAccess = false, bool bootstrapOnly = false) {
  if (origin.secure()) return {}; // existing HTTPS optional metadata compatibility
  if (modernAccess && bootstrapOnly) return "broker_setup_only";
  if (!present || value.protocol != "broker-transport-v1")
    return modernAccess ? "broker_transport_profile_mismatch" : "broker_http_unsupported";
  if (!value.exactTypes) return "broker_transport_profile_mismatch";
  if (value.publicOrigin != origin.text || value.scheme != origin.scheme) return "broker_origin_not_allowed";
  if (!value.allowInsecureHttp) return "broker_http_disabled";
  if (value.directTls) return "broker_transport_profile_mismatch";
  return {};
}
inline bool healthMatches(const Origin& origin, const HealthProfile& value, bool present) {
  return healthFailure(origin, value, present).empty();
}
}
