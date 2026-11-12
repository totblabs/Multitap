#pragma once
#include "product_versions.h"
#include "media.hpp"
#include "windows_error.hpp"
#include "broker_origin.hpp"
#include <winhttp.h>
#ifdef _MSC_VER
// Standalone Host fixtures also instantiate the inline IDN adapter.
#pragma comment(lib,"normaliz.lib")
#endif
#include <wincrypt.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <variant>
#include <intrin.h>

namespace host {
// How many leading bytes of data[0..size) need no JSON escape (no quote, backslash or control character), 16 at a time: a
// clipboard chunk is one 61440-character string, parsed and quoted on every Host hop (stage_bench: the byte loop was ~1/3 of a chunk).
inline size_t jsonPlainRun(const char* data, size_t size) {
  size_t at = 0;
#if defined(_M_X64)
  const __m128i quote = _mm_set1_epi8('"'), backslash = _mm_set1_epi8('\\'), control = _mm_set1_epi8(31);
  for (; at + 16 <= size; at += 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at));
    const __m128i special = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, backslash)), _mm_cmpeq_epi8(_mm_min_epu8(v, control), v));
    if (const unsigned mask = unsigned(_mm_movemask_epi8(special))) { unsigned long first = 0; _BitScanForward(&first, mask); return at + first; }
  }
#endif
  while (at < size) { const unsigned char c = static_cast<unsigned char>(data[at]); if (c == '"' || c == '\\' || c < 32) break; ++at; }
  return at;
}
inline std::string utf8(const std::wstring& value) { if (value.empty()) return {}; const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr); if (!size) throw std::runtime_error("Invalid Unicode text"); std::string out(size, 0); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), out.data(), size, nullptr, nullptr); return out; }
inline std::wstring wide(const std::string& value) { if (value.empty()) return {}; const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0); if (!size) throw std::runtime_error("Invalid UTF8 text"); std::wstring out(size, 0); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), out.data(), size); return out; }
// Runs that need no escape are copied whole (a clipboard chunk is 61440 base64 characters per message, twice per hop).
inline std::string jsonQuote(const std::string& text) {
  std::string out; out.reserve(text.size() + 2); out += '"';
  for (size_t at = 0;;) {
    const size_t run = jsonPlainRun(text.data() + at, text.size() - at);
    out.append(text, at, run); at += run;
    if (at >= text.size()) break;
    const unsigned char c = static_cast<unsigned char>(text[at++]);
    if (c == '\\' || c == '"') { out += '\\'; out += char(c); } else { const char hex[] = "0123456789abcdef"; out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  out += '"'; return out;
}
struct Json {
  using Object = std::map<std::string, Json>; using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Object, Array> value = nullptr;
  const Json& get(const std::string& key) const { static const Json empty; const auto* object = std::get_if<Object>(&value); if (!object) return empty; const auto item = object->find(key); return item == object->end() ? empty : item->second; }
  std::string string() const { const auto* text = std::get_if<std::string>(&value); return text ? *text : ""; }
  double number(double fallback = 0) const { const auto* number = std::get_if<double>(&value); return number ? *number : fallback; }
  bool boolean(bool fallback = false) const { const auto* boolean = std::get_if<bool>(&value); return boolean ? *boolean : fallback; }
};
class JsonParser {
 public:
  static Json parse(const std::string& text) { if (text.size() > 65536) throw std::runtime_error("Control JSON exceeds bounds"); JsonParser parser(text); Json value = parser.item(0); parser.space(); if (parser.at_ != text.size()) throw std::runtime_error("Invalid control JSON"); return value; }
 private:
  explicit JsonParser(const std::string& text) : text_(text) {}
  const std::string& text_; size_t at_ = 0;
  void space() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\r' || text_[at_] == '\n' || text_[at_] == '\t')) at_++; }
  char take() { if (at_ >= text_.size()) throw std::runtime_error("Truncated control JSON"); return text_[at_++]; }
  bool eat(char ch) { space(); if (at_ < text_.size() && text_[at_] == ch) { at_++; return true; } return false; }
  unsigned hex4() { unsigned n = 0; for (int i = 0; i < 4; i++) { char c = take(); n <<= 4; if (c >= '0' && c <= '9') n |= c - '0'; else if (c >= 'a' && c <= 'f') n |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') n |= c - 'A' + 10; else throw std::runtime_error("Invalid JSON escape"); } return n; }
  std::string string() {
    if (take() != '"') throw std::runtime_error("Expected JSON string"); std::string out;
    for (;;) {
      // A plain run (no quote, backslash or control character) is copied whole.
      const size_t run = at_; at_ += jsonPlainRun(text_.data() + at_, text_.size() - at_);
      out.append(text_, run, at_ - run);
      const char c = take(); if (c == '"') return out; if (static_cast<unsigned char>(c) < 32) throw std::runtime_error("Invalid JSON text");
      const char escaped = take();
      switch (escaped) { case '"': case '\\': case '/': out += escaped; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
        case 'u': { unsigned cp = hex4(); std::wstring chars; if (cp >= 0xd800 && cp <= 0xdbff) { if (take() != '\\' || take() != 'u') throw std::runtime_error("Invalid JSON surrogate"); const unsigned second = hex4(); if (second < 0xdc00 || second > 0xdfff) throw std::runtime_error("Invalid JSON surrogate"); chars.push_back(wchar_t(cp)); chars.push_back(wchar_t(second)); } else { if (cp >= 0xdc00 && cp <= 0xdfff) throw std::runtime_error("Invalid JSON surrogate"); chars.push_back(wchar_t(cp)); } out += utf8(chars); break; }
        default: throw std::runtime_error("Invalid JSON escape");
      }
    }
  }
  Json item(int depth) {
    if (depth > 16) throw std::runtime_error("Control JSON nesting exceeds bounds"); space(); if (at_ == text_.size()) throw std::runtime_error("Missing control JSON"); Json value;
    if (text_[at_] == '"') { value.value = string(); return value; }
    if (eat('{')) { Json::Object object; if (!eat('}')) do { space(); auto key = string(); if (!eat(':')) throw std::runtime_error("Invalid JSON object"); if (object.size() >= 512 || !object.emplace(key, item(depth + 1)).second) throw std::runtime_error("Invalid JSON object keys"); } while (eat(',')); else { value.value = object; return value; } if (!eat('}')) throw std::runtime_error("Invalid JSON object end"); value.value = std::move(object); return value; }
    if (eat('[')) { Json::Array array; if (!eat(']')) do { if (array.size() >= 512) throw std::runtime_error("JSON array exceeds bounds"); array.push_back(item(depth + 1)); } while (eat(',')); else { value.value = array; return value; } if (!eat(']')) throw std::runtime_error("Invalid JSON array end"); value.value = std::move(array); return value; }
    for (const auto& word : {std::string("true"), std::string("false"), std::string("null")}) if (text_.compare(at_, word.size(), word) == 0) { at_ += word.size(); if (word != "null") value.value = word == "true"; return value; }
    const size_t start = at_; if (text_[at_] == '-') at_++; if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9') throw std::runtime_error("Invalid JSON value");
    if (text_[at_] == '0') at_++; else while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') at_++;
    if (at_ < text_.size() && text_[at_] == '.') { at_++; const size_t digit = at_; while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') at_++; if (digit == at_) throw std::runtime_error("Invalid JSON fraction"); }
    if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) { at_++; if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) at_++; const size_t digit = at_; while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') at_++; if (digit == at_) throw std::runtime_error("Invalid JSON exponent"); }
    const double n = std::stod(text_.substr(start, at_ - start)); if (!std::isfinite(n)) throw std::runtime_error("Invalid JSON number"); value.value = n; return value;
  }
};
struct Internet { HINTERNET handle = nullptr; Internet() = default; explicit Internet(HINTERNET value) : handle(value) { if (!value) winCheck(FALSE, "Open HTTPS handle"); } ~Internet() { if (handle) WinHttpCloseHandle(handle); } Internet(const Internet&) = delete; Internet& operator=(const Internet&) = delete; };
struct Origin {
  transport::Origin policy;
  std::wstring text, hostname, httpConsentOrigin; INTERNET_PORT port = 443;
  static std::string asciiAddress(const std::wstring& input) {
    if (input.size() > 512) transport::invalid();
    if (std::all_of(input.begin(), input.end(), [](wchar_t c){return static_cast<uint32_t>(c) < 128u;})) return utf8(input);
    // Preserve IDN support while sharing an ASCII canonical identity with URL
    // implementations. Only the hostname is mapped; syntax is still checked.
    URL_COMPONENTS parts{}; parts.dwStructSize=sizeof(parts); parts.dwHostNameLength=DWORD(-1);
    if(!WinHttpCrackUrl(input.c_str(),DWORD(input.size()),0,&parts)||!parts.dwHostNameLength)transport::invalid();
    const int length=IdnToAscii(IDN_USE_STD3_ASCII_RULES,parts.lpszHostName,int(parts.dwHostNameLength),nullptr,0);
    if(length<=0)transport::invalid();std::wstring host(size_t(length),L'\0');
    if(IdnToAscii(IDN_USE_STD3_ASCII_RULES,parts.lpszHostName,int(parts.dwHostNameLength),host.data(),length)!=length)transport::invalid();
    auto mapped=input; mapped.replace(size_t(parts.lpszHostName-input.data()),parts.dwHostNameLength,host);return utf8(mapped);
  }
  explicit Origin(const std::wstring& input,const std::wstring& consent={}) {
    policy=transport::authorize(asciiAddress(input),consent.empty()?std::string():asciiAddress(consent));
    text=wide(policy.text);hostname=wide(policy.host);port=policy.port;
    if(!policy.secure())httpConsentOrigin=text;
  }
  DWORD flags()const{return policy.secure()?WINHTTP_FLAG_SECURE:0;}
  bool relayMatches(const std::string& url,const std::string& path)const{return transport::relayMatches(policy,url,path);}
};
inline HINTERNET httpSession() { HINTERNET value = WinHttpOpen(L"MultitapHost/" MULTITAP_HOST_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0); if (!value) winCheck(FALSE, "Create HTTPS session"); WinHttpSetTimeouts(value, 8000, 8000, 8000, 15000); return value; }
inline void validateBrokerHealth(const transport::Origin& origin,const Json& value) {
  if(value.get("service").string()!="multitap-broker"||value.get("status").string()!="ok"||value.get("authentication").string()!="id-password"||value.get("media").string()!="relay-v1")throw std::runtime_error("broker_incompatible");
  const auto& raw=value.get("transport");const auto* object=std::get_if<Json::Object>(&raw.value);
  transport::HealthProfile profile;profile.protocol=raw.get("protocol").string();profile.publicOrigin=raw.get("publicOrigin").string();profile.scheme=raw.get("scheme").string();
  profile.allowInsecureHttp=raw.get("allowInsecureHttp").boolean();profile.directTls=raw.get("directTls").boolean();
  profile.exactTypes=object&&object->size()==5&&std::holds_alternative<std::string>(raw.get("protocol").value)&&std::holds_alternative<std::string>(raw.get("publicOrigin").value)&&std::holds_alternative<std::string>(raw.get("scheme").value)&&std::holds_alternative<bool>(raw.get("allowInsecureHttp").value)&&std::holds_alternative<bool>(raw.get("directTls").value);
  const auto* envelope=std::get_if<Json::Object>(&value.value);
  const bool present=envelope&&envelope->find("transport")!=envelope->end();
  const auto& access=value.get("access");
  const bool modernAccess=access.get("protocol").string()=="server-access-v1";
  const auto reason=transport::healthFailure(origin,profile,present,modernAccess,access.get("bootstrapOnly").boolean());
  if(!reason.empty())throw std::runtime_error(reason);
}
inline const char* brokerNetworkFailure(DWORD error) {
  switch(error){
    case ERROR_WINHTTP_SECURE_FAILURE:return "broker_tls_failed";
    case ERROR_WINHTTP_CANNOT_CONNECT:return "broker_connection_failed";
    case ERROR_WINHTTP_TIMEOUT:return "broker_timeout";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:return "broker_dns_failed";
    default:return "broker_network_failed";
  }
}
inline void brokerNetworkCheck(BOOL ok) {
  if(!ok)throw std::runtime_error(brokerNetworkFailure(GetLastError()));
}
inline std::string brokerAccessFailure(const Json& value) {
  const auto& error=value.get("error");
  const auto code=std::holds_alternative<std::string>(error.value)?error.string():error.get("code").string();
  if(code=="http_disabled"||code=="http_not_allowed")return "broker_http_disabled";
  if(code=="origin_not_allowed"||code=="unregistered_origin")return "broker_origin_not_allowed";
  return {};
}
inline Json brokerHealth(const Origin& origin) {
  Internet session(httpSession()),connection(WinHttpConnect(session.handle,origin.hostname.c_str(),origin.port,0));
  Internet request(WinHttpOpenRequest(connection.handle,L"GET",L"/healthz",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,origin.flags()));
  DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  winCheck(WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy)),"Disable broker health redirects");
  brokerNetworkCheck(WinHttpSendRequest(request.handle,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0));
  brokerNetworkCheck(WinHttpReceiveResponse(request.handle,nullptr));
  DWORD status=0,length=sizeof(status);winCheck(WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX),"Broker health status");
  std::string bytes;for(;;){std::array<char,2048> chunk{};DWORD size=0;brokerNetworkCheck(WinHttpReadData(request.handle,chunk.data(),DWORD(chunk.size()),&size));if(!size)break;if(bytes.size()+size>8192)throw std::runtime_error("broker_health_too_large");bytes.append(chunk.data(),size);}
  if(status!=200){
    std::string reason;
    try{reason=brokerAccessFailure(JsonParser::parse(bytes));}catch(...){ }
    throw std::runtime_error(reason.empty()?"broker_health_failed":reason);
  }
  const auto value=JsonParser::parse(bytes);
  validateBrokerHealth(origin.policy,value);
  return value;
}
inline Origin verifiedBrokerOrigin(const std::wstring& address,const std::wstring& consent={}) {
  Origin origin(address,consent);if(!origin.policy.secure())(void)brokerHealth(origin);return origin;
}
struct BrokerHttpError : std::runtime_error {
  DWORD status; std::string code;
  BrokerHttpError(DWORD value, std::string reason) : std::runtime_error("Enrollment HTTP " + std::to_string(value) + (reason.empty() ? "" : ": " + reason)), status(value), code(std::move(reason)) {}
};
struct ProtectedTextScrubber { std::string& text; ~ProtectedTextScrubber() { SecureZeroMemory(text.data(), text.size()); } };
inline Json post(const Origin& origin, const std::wstring& path, const std::string& body, const wchar_t* headers = L"Content-Type: application/json\r\n") {
  if(!origin.policy.secure())(void)brokerHealth(origin);
  Internet session(httpSession()), connection(WinHttpConnect(session.handle, origin.hostname.c_str(), origin.port, 0));
  Internet request(WinHttpOpenRequest(connection.handle, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, origin.flags()));
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)), "Disable HTTPS redirects");
  winCheck(WinHttpSendRequest(request.handle, headers, DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0), "Send HTTPS enrollment");
  winCheck(WinHttpReceiveResponse(request.handle, nullptr), "Receive HTTPS enrollment");
  DWORD status = 0, size = sizeof(status); winCheck(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX), "Read HTTPS status");
  std::string result; ProtectedTextScrubber responseScrubber{result};
  for (;;) { DWORD available = 0; winCheck(WinHttpQueryDataAvailable(request.handle, &available), "Read HTTPS body size"); if (!available) break; if (result.size() + available > 65536) throw std::runtime_error("Enrollment response exceeds bounds"); const size_t offset = result.size(); result.resize(offset + available); DWORD actual = 0; winCheck(WinHttpReadData(request.handle, result.data() + offset, available, &actual), "Read HTTPS response"); result.resize(offset + actual); }
  if (status < 200 || status >= 300) { std::string code; try { code = JsonParser::parse(result).get("error").get("code").string(); } catch (...) {} if (code.size() > 80 || code.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) code.clear(); throw BrokerHttpError(status, std::move(code)); }
  return JsonParser::parse(result);
}
inline std::string machineUuid() {
  constexpr DWORD rawSmbios = 0x52534d42;
  const DWORD size = GetSystemFirmwareTable(rawSmbios, 0, nullptr, 0); if (size < 8 || size > 1024 * 1024) throw std::runtime_error("Cannot read SMBIOS UUID");
  std::vector<uint8_t> data(size); if (GetSystemFirmwareTable(rawSmbios, 0, data.data(), size) != size) throw std::runtime_error("Cannot read SMBIOS UUID");
  DWORD length = 0; std::memcpy(&length, data.data() + 4, 4); const size_t end = std::min(size_t(size), size_t(length) + 8);
  for (size_t at = 8; at + 4 <= end;) {
    const uint8_t type = data[at], count = data[at + 1]; if (count < 4 || at + count > end) break;
    if (type == 1 && count >= 24) {
      const auto* bytes = data.data() + at + 8;
      if (std::all_of(bytes, bytes + 16, [](uint8_t b) { return b == 0; }) || std::all_of(bytes, bytes + 16, [](uint8_t b) { return b == 255; })) break;
      GUID guid{}; std::memcpy(&guid, bytes, 16);
      if (data[1] < 2 || (data[1] == 2 && data[2] < 6)) { guid.Data1 = (DWORD(bytes[0]) << 24) | (DWORD(bytes[1]) << 16) | (DWORD(bytes[2]) << 8) | bytes[3]; guid.Data2 = WORD((WORD(bytes[4]) << 8) | bytes[5]); guid.Data3 = WORD((WORD(bytes[6]) << 8) | bytes[7]); }
      wchar_t text[40]{}; StringFromGUID2(guid, text, 40); std::wstring value(text + 1, text + 37); for (auto& ch : value) ch = wchar_t(towlower(ch)); return utf8(value);
    }
    at += count; while (at + 1 < end && (data[at] != 0 || data[at + 1] != 0)) at++; at += 2;
  }
  throw std::runtime_error("SMBIOS UUID is unavailable; VM enrollment cannot be verified");
}
// host-machine-v1: machineUuid is the SMBIOS UUID of the Windows the
// registration was approved on (lowercase). Empty in a registration saved
// without it; credential_binding.hpp records it on the first start.
struct Credentials { std::wstring origin; std::string hostId, token, desktopId; std::wstring httpConsentOrigin; std::string machineUuid; };
inline bool credentialMachineUuid(const std::string& text) {
  if (text.size() != 36) return false;
  for (size_t i = 0; i < text.size(); i++) if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; } else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
  return true;
}
inline std::string lowerMachineUuid(std::string text) { for (auto& ch : text) if (ch >= 'A' && ch <= 'F') ch = char(ch - 'A' + 'a'); return text; }
// The protected file exists but this Windows cannot unseal it (machine DPAPI
// key differs, e.g. a copied disk after sysprep). Distinct from a missing file.
struct CredentialUnreadable : std::runtime_error { CredentialUnreadable() : std::runtime_error("Read protected host credential") {} };
inline Origin credentialOrigin(const Credentials& value) { return Origin(value.origin,value.httpConsentOrigin); }
inline Origin verifiedCredentialOrigin(const Credentials& value) { return verifiedBrokerOrigin(value.origin,value.httpConsentOrigin); }
inline std::filesystem::path knownFolder(REFKNOWNFOLDERID id) { PWSTR directory = nullptr; check(SHGetKnownFolderPath(id, 0, nullptr, &directory), "Windows data directory"); std::filesystem::path path(directory); CoTaskMemFree(directory); return path; }
// Machine-wide folders are resolved once per process. Every shell folder query
// (SHGetKnownFolderPath, SHGetFolderPath) leaves a 24-byte block on the process
// heap for the calling thread that the thread's exit never frees (Windows 11
// 26100: +1 live block per exited thread that made one). machineDirectory()
// runs on short-lived threads, e.g. each streaming session's capture thread
// checks encoder.json for the x264/MFT switch, which leaked one block per
// session (track-agent soak). Per-user folders (LocalAppData) depend on the
// caller's token and stay uncached.
inline const std::filesystem::path& programDataFolder() { static const std::filesystem::path path = knownFolder(FOLDERID_ProgramData); return path; }
inline const std::filesystem::path& programFilesFolder() { static const std::filesystem::path path = knownFolder(FOLDERID_ProgramFiles); return path; }
inline std::filesystem::path machineDirectory() { return programDataFolder() / L"Multitap" / L"Host"; }
// Who may use the Host's data folders. `usersRead` (ProgramData\Multitap and
// ...\Host): SYSTEM and Administrators everything, Users read only. Otherwise
// (...\Host\private): SYSTEM and Administrators only. A worker of a standard
// Windows user therefore reads the first and has nothing in the second
// (standard_user_access_test.cpp checks both with a real access check).
inline const wchar_t* hostDataSecurityText(bool usersRead) {
  return usersRead ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)" : L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
}
// A private file of the Host (credentials, requests, journals): SYSTEM and
// Administrators only, set when the file is created instead of inherited from
// a folder whose list may have been changed in between.
inline constexpr const wchar_t* hostPrivateFileSecurityText = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
inline bool hostAdministrativeSid(PSID sid) { return sid && IsValidSid(sid) && (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)); }
// No allow entry names anyone but SYSTEM and Administrators, with any right
// (reading a machine-DPAPI file is as good as reading its secret).
inline bool privateOnlyAcl(PACL dacl) {
  if (!dacl) return false;
  for (DWORD i = 0; i < dacl->AceCount; i++) {
    void* raw = nullptr; if (!GetAce(dacl, i, &raw)) return false; const auto* header = static_cast<ACE_HEADER*>(raw); if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) { const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw); PSID sid = const_cast<DWORD*>(&ace->SidStart); if (ace->Mask && !hostAdministrativeSid(sid)) return false; }
    else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE || header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE || header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) return false;
  }
  return true;
}
// The Host's private folder as prepareMachineDirectory makes it:
// owned by SYSTEM or Administrators, a protected list, and
// every entry, inheritable or not, an allow entry for SYSTEM or Administrators.
// Anything else was not made by the Host (ProgramData lets every user create
// folders) and is not used.
inline bool exactPrivateDescriptor(PSECURITY_DESCRIPTOR descriptor) {
  if (!descriptor || !IsValidSecurityDescriptor(descriptor)) return false;
  PSID owner = nullptr; BOOL ownerDefaulted = FALSE, present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
  if (!GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) || !hostAdministrativeSid(owner)) return false;
  if (!GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED)) return false;
  if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present || !dacl) return false;
  for (DWORD i = 0; i < dacl->AceCount; i++) {
    void* raw = nullptr; if (!GetAce(dacl, i, &raw)) return false;
    const auto* header = static_cast<ACE_HEADER*>(raw);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
    if (!hostAdministrativeSid(const_cast<DWORD*>(&static_cast<ACCESS_ALLOWED_ACE*>(raw)->SidStart))) return false;
  }
  return true;
}
inline bool exactPrivateDirectory(const std::filesystem::path& path) {
  const HANDLE folder = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (folder == INVALID_HANDLE_VALUE) return false;
  FILE_ATTRIBUTE_TAG_INFO tag{}; bool safe = GetFileInformationByHandleEx(folder, FileAttributeTagInfo, &tag, sizeof(tag)) && (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (safe) safe = GetSecurityInfo(folder, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS && exactPrivateDescriptor(descriptor);
  if (descriptor) LocalFree(descriptor); CloseHandle(folder); return safe;
}
// A private folder that is there but not exactly the Host's is never adopted
// (its list reset while files someone else owns, links or open handles stay
// inside): an empty one or a link is removed, anything else is moved aside to
// "<name>.untrusted-{GUID}" (never deleted), and a new one is created. A
// correctly protected folder is kept as it is, with everything in it.
// [exact] and [sddl] are replaced only by the fixture (windows_account_record_test.cpp).
inline void securePrivateDirectory(const std::filesystem::path& path, const std::function<bool(const std::filesystem::path&)>& exact = exactPrivateDirectory,
                                   const wchar_t* sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)") {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  winCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr), "Create host data ACL");
  struct Free { PSECURITY_DESCRIPTOR value; ~Free() { LocalFree(value); } } free{descriptor};
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (CreateDirectoryW(path.c_str(), &security)) {
      // A new folder: its owner becomes Administrators, as before.
      PSID owner = nullptr; if (ConvertStringSidToSidW(L"S-1-5-32-544", &owner)) { (void)SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, owner, nullptr, nullptr, nullptr); LocalFree(owner); }
      if (!exact(path)) throw std::runtime_error("Cannot restrict host data permissions");
      return;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) winCheck(FALSE, "Create protected host data directory");
    if (exact(path)) return;
    // Busy is not untrusted: a folder that cannot be looked at right now stays where it is.
    { const HANDLE probe = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe); else if (GetLastError() == ERROR_SHARING_VIOLATION) throw std::runtime_error("Host data directory is busy"); }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) && ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str()))) continue;
    GUID guid{}; check(CoCreateGuid(&guid), "Untrusted host data directory name"); wchar_t suffix[40]{}; StringFromGUID2(guid, suffix, 40);
    if (!MoveFileExW(path.c_str(), (path.wstring() + L".untrusted-" + suffix).c_str(), 0)) winCheck(FALSE, "Move aside an untrusted host data directory");
  }
  throw std::runtime_error("Refusing an unsafe host data directory");
}
inline void secureDirectory(const std::filesystem::path& path, bool usersRead) {
  if (!usersRead) { securePrivateDirectory(path); return; }
  const DWORD attributes = GetFileAttributesW(path.c_str()); if (attributes != INVALID_FILE_ATTRIBUTES && (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))) throw std::runtime_error("Refusing an unsafe host data directory");
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  winCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(hostDataSecurityText(usersRead), SDDL_REVISION_1, &descriptor, nullptr), "Create host data ACL");
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  if (!CreateDirectoryW(path.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS) { LocalFree(descriptor); winCheck(FALSE, "Create protected host data directory"); }
  PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE; GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
  PSID owner=nullptr;ConvertStringSidToSidW(L"S-1-5-32-544",&owner);const DWORD result = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, owner, nullptr, dacl, nullptr);if(owner)LocalFree(owner); LocalFree(descriptor);
  if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot restrict host data permissions");
}
inline void prepareMachineDirectory() { const auto path = machineDirectory(); secureDirectory(path.parent_path(), true); secureDirectory(path, true); secureDirectory(path / L"private", false); }
inline std::wstring temporarySuffix(){GUID id{};check(CoCreateGuid(&id),"Secure temporary file name");wchar_t text[40]{};StringFromGUID2(id,text,40);return std::wstring(L".")+text+L".new";}
// Who may change a machine-wide entry: SYSTEM and Administrators. `installer`
// also accepts TrustedInstaller, the Windows servicing identity that every
// folder created under Program Files inherits full control for.
inline bool machineWriter(PSID sid,bool installer){
  if(IsWellKnownSid(sid,WinLocalSystemSid)||IsWellKnownSid(sid,WinBuiltinAdministratorsSid))return true;
  if(!installer)return false;
  PSID servicing=nullptr;if(!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",&servicing))return false;
  const bool same=EqualSid(sid,servicing)!=FALSE;LocalFree(servicing);return same;
}
// The owner is such a writer and no allow entry gives anyone else a write right.
inline bool trustedSecurity(PSID owner,PACL dacl,bool installer){
  bool safe=owner&&machineWriter(owner,installer)&&dacl;
  const DWORD writes=GENERIC_WRITE|GENERIC_ALL|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER;
  if(safe)for(DWORD i=0;i<dacl->AceCount;i++){void* raw=nullptr;if(!GetAce(dacl,i,&raw)){safe=false;break;}const auto* header=static_cast<ACE_HEADER*>(raw);if(header->AceFlags&INHERIT_ONLY_ACE)continue;if(header->AceType==ACCESS_ALLOWED_ACE_TYPE){const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);PSID sid=const_cast<DWORD*>(&ace->SidStart);if((ace->Mask&writes)&&!machineWriter(sid,installer)){safe=false;break;}}else if(header->AceType==ACCESS_ALLOWED_OBJECT_ACE_TYPE){safe=false;break;}}
  return safe;
}
inline bool trustedMachineEntry(const std::filesystem::path& path,bool installer){
  const DWORD attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
  PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;if(GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
  const bool safe=trustedSecurity(owner,dacl,installer);
  LocalFree(descriptor);return safe;
}
inline bool trustedMachineFile(const std::filesystem::path& path){return trustedMachineEntry(path,false);}
// A private file (or the private folder): trustedMachineFile, and no entry for
// anyone else at all, reading included.
inline bool trustedPrivateFile(const std::filesystem::path& path){
  if(!trustedMachineFile(path))return false;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
  const DWORD result=GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&descriptor);
  const bool safe=result==ERROR_SUCCESS&&privateOnlyAcl(dacl);if(descriptor)LocalFree(descriptor);return safe;
}
// Writes [bytes] to [path] in the private folder: a new temporary file with
// hostPrivateFileSecurityText from the start, written and flushed, checked to
// be private, then moved over [path]. False on any failure (nothing is left).
inline bool writePrivateFile(const std::filesystem::path& path,const void* bytes,DWORD size){
  PSECURITY_DESCRIPTOR descriptor=nullptr;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(hostPrivateFileSecurityText,SDDL_REVISION_1,&descriptor,nullptr))return false;
  SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
  const auto temporary=std::filesystem::path(path.wstring()+temporarySuffix());
  const HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  LocalFree(descriptor);if(file==INVALID_HANDLE_VALUE)return false;
  DWORD written=0;const bool saved=WriteFile(file,bytes,size,&written,nullptr)&&written==size&&FlushFileBuffers(file);CloseHandle(file);
  if(!saved||!trustedPrivateFile(temporary)||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());return false;}
  return true;
}
// An installed program path under Program Files. The Host is a Windows
// Installer package, so its folders are created by Windows Installer and
// inherit Program Files' entries, TrustedInstaller's full control among them.
// trustedMachineFile refuses such a folder, which would make every check of
// the service's installation fail on a Host first installed from the MSI:
// Host settings would show "서버 연결 확인 필요" for a connected Host and
// could not restart the service. TrustedInstaller can
// already replace any Windows file; accepting it gives no one a new way in.
inline bool trustedInstalledPath(const std::filesystem::path& path){return trustedMachineEntry(path,true);}
inline std::filesystem::path credentialPath() { return machineDirectory() / L"private" / L"credentials.dpapi"; }
inline bool validToken(const std::string& token) { return token.size() == 43 && token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos; }
// The sealed text of a registration, and back (credential_binding_test.cpp).
inline std::string credentialJson(const Credentials& value) {
  const auto machine = lowerMachineUuid(value.machineUuid);
  if (!validToken(value.token) || (!machine.empty() && !credentialMachineUuid(machine))) throw std::runtime_error("Invalid host credential"); Origin origin(value.origin,value.httpConsentOrigin);
  return "{\"origin\":" + jsonQuote(utf8(origin.text)) + ",\"hostId\":" + jsonQuote(value.hostId) + ",\"token\":" + jsonQuote(value.token) + ",\"desktopId\":" + jsonQuote(value.desktopId) + (origin.policy.secure()?"":",\"httpConsentOrigin\":"+jsonQuote(utf8(origin.httpConsentOrigin))) + (machine.empty()?"":",\"machineUuid\":"+jsonQuote(machine)) + '}';
}
inline void readCredentialJson(const Json& value, Credentials& out) {
  const Origin origin(wide(value.get("origin").string()),wide(value.get("httpConsentOrigin").string()));
  out.origin=origin.text;out.httpConsentOrigin=origin.httpConsentOrigin;out.hostId = value.get("hostId").string(); out.token = value.get("token").string(); out.desktopId = value.get("desktopId").string(); out.machineUuid = lowerMachineUuid(value.get("machineUuid").string());
  if (!validToken(out.token) || (!out.machineUuid.empty() && !credentialMachineUuid(out.machineUuid))) throw std::runtime_error("Protected host credential is invalid");
}
inline void saveCredentials(const Credentials& value) {
  std::string json = credentialJson(value);
  DATA_BLOB input{DWORD(json.size()), reinterpret_cast<BYTE*>(json.data())}, encrypted{};
  winCheck(CryptProtectData(&input, L"Multitap host credential", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN | CRYPTPROTECT_LOCAL_MACHINE, &encrypted), "Protect host credential");
  const auto path = credentialPath(); prepareMachineDirectory();
  // Machine DPAPI: any process on this PC can unseal the file, so who can read it is decided by its own list.
  const bool saved = writePrivateFile(path, encrypted.pbData, encrypted.cbData); LocalFree(encrypted.pbData); SecureZeroMemory(json.data(), json.size());
  if (!saved) throw std::runtime_error("Cannot save protected host credential");
}
inline bool loadCredentials(Credentials& out) {
  const auto path = credentialPath(); if (!std::filesystem::exists(path)) return false;
  if(!trustedPrivateFile(path))throw std::runtime_error("Machine registration permissions must allow only System and Administrators");
  std::ifstream file(path, std::ios::binary); std::vector<BYTE> data((std::istreambuf_iterator<char>(file)), {});
  if (data.empty() || data.size() > 16384) throw std::runtime_error("Protected host credential is invalid");
  DATA_BLOB input{DWORD(data.size()), data.data()}, plain{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) throw CredentialUnreadable();
  std::string json(reinterpret_cast<const char*>(plain.pbData), plain.cbData); SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData);
  const auto value = JsonParser::parse(json); SecureZeroMemory(json.data(), json.size());
  readCredentialJson(value, out); return true;
}
// Where a private file that is not this Windows's goes (host-machine-v1): next
// to the live file, in the same private folder, never deleted (the move keeps
// the file's own access list). credentials.dpapi -> credentials-copied-<time>.dpapi
inline std::filesystem::path heldPrivatePath(const std::filesystem::path& live, const wchar_t* kind) {
  FILETIME time{}; GetSystemTimeAsFileTime(&time); ULARGE_INTEGER stamp{}; stamp.LowPart = time.dwLowDateTime; stamp.HighPart = time.dwHighDateTime;
  return live.parent_path() / (live.stem().wstring() + L"-" + kind + L"-" + std::to_wstring(stamp.QuadPart) + live.extension().wstring());
}
inline void keepPrivateFileAside(const std::filesystem::path& live, const wchar_t* kind) {
  winCheck(MoveFileExW(live.c_str(), heldPrivatePath(live, kind).c_str(), MOVEFILE_WRITE_THROUGH), "Keep host registration aside");
}
inline void keepCredentialFileAside(const wchar_t* kind) { keepPrivateFileAside(credentialPath(), kind); }
inline Credentials enroll(const std::wstring& address, const std::string& code,const std::wstring& consent={}) {
  const Origin origin(address,consent); if (code.empty() || code.size() > 128 || code.find_first_of("\r\n\t ") != std::string::npos) throw std::runtime_error("Enter the single-use VM enrollment code");
  wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD size = MAX_COMPUTERNAME_LENGTH + 1; winCheck(GetComputerNameW(hostname, &size), "Read host name");
  const auto machine = machineUuid();
  const auto result = post(origin, L"/api/v1/hosts/enroll", "{\"code\":" + jsonQuote(code) + ",\"hostname\":" + jsonQuote(utf8(hostname)) + ",\"machineUuid\":" + jsonQuote(machine) + ",\"version\":\"" MULTITAP_HOST_VERSION_UTF8 "\"}");
  Credentials credentials{origin.text, result.get("hostId").string(), result.get("hostToken").string(), result.get("desktopId").string(),origin.httpConsentOrigin,machine};
  if (!validToken(credentials.token) || credentials.hostId.empty() || credentials.desktopId.empty()) throw std::runtime_error("Enrollment response is invalid");
  saveCredentials(credentials); return credentials;
}
}
