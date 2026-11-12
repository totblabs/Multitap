#pragma once
// Streaming drive redirection (drive-v1; docs/drive-redirection.md): the rules
// the Host shares with the broker (server/src/drive.mjs) and the client:
// plain Windows file-name segments, share ids, fixed error codes, and the
// WebDAV status each code becomes.
#include "machine.hpp"
#include <set>

namespace host::drive {
inline constexpr const char* protocol = "drive-v1";
inline constexpr size_t chunkBytes = 262144, maxInFlight = 8, maxShares = 8, maxPathUnits = 1024, maxNameUnits = 255, maxDepth = 64, maxLabelUnits = 64;
// Every token the Host serves: "Multitap-" + 43 base64url characters (256 bits).
inline constexpr const char* sharePrefix = "Multitap-";

// One plain Windows file name: 1..max UTF-16 units, well formed, no control
// characters or <>:"/\|?*, not ending in a dot or space, not a DOS device
// name (with or without an extension, superscript COM/LPT digits included).
inline bool safeName(const std::wstring& name, size_t max = maxNameUnits) {
  if (name.empty() || name.size() > max) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const wchar_t c = name[i];
    if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) return false;
    if (c >= 0xd800 && c <= 0xdbff) { if (++i == name.size() || name[i] < 0xdc00 || name[i] > 0xdfff) return false; }
    else if (c >= 0xdc00 && c <= 0xdfff) return false;
  }
  if (name.back() == L'.' || name.back() == L' ') return false;
  std::wstring stem = name.substr(0, name.find(L'.'));
  while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
  for (auto& c : stem) if (c >= L'a' && c <= L'z') c = wchar_t(c - 32);
  static const std::set<std::wstring> devices{L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
  if (devices.count(stem)) return false;
  if (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0)) {
    const wchar_t digit = stem[3];
    if ((digit >= L'1' && digit <= L'9') || digit == 0xb9 || digit == 0xb2 || digit == 0xb3) return false;
  }
  return true;
}
inline bool safeName(const std::string& name, size_t max = maxNameUnits) {
  if (name.empty() || name.size() > max * 3) return false;
  try { return safeName(wide(name), max); } catch (...) { return false; }
}
// '' (the share root) or 1..64 safe segments joined with '/', <= 1024 units.
inline std::vector<std::wstring> pathSegments(const std::wstring& path) {
  std::vector<std::wstring> segments;
  if (path.empty()) return segments;
  if (path.size() > maxPathUnits) throw std::runtime_error("drive_invalid_path");
  size_t start = 0;
  for (;;) {
    const auto end = path.find(L'/', start);
    segments.push_back(path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
    if (!safeName(segments.back()) || segments.size() > maxDepth) throw std::runtime_error("drive_invalid_path");
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return segments;
}
inline bool safePath(const std::string& path) {
  if (path.empty()) return true;
  if (path.size() > maxPathUnits * 3) return false;
  try { (void)pathSegments(wide(path)); return true; } catch (...) { return false; }
}
inline bool shareId(const std::string& id) {
  return !id.empty() && id.size() <= 32 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
inline std::string randomToken() {
  std::array<BYTE, 32> bytes{};
  check(BCryptGenRandom(nullptr, bytes.data(), DWORD(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "Drive share token");
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out; uint32_t buffer = 0; int bits = 0;
  for (const BYTE value : bytes) { buffer = (buffer << 8) | value; bits += 8; while (bits >= 6) { bits -= 6; out += alphabet[(buffer >> bits) & 63]; } }
  if (bits) out += alphabet[(buffer << (6 - bits)) & 63];
  SecureZeroMemory(bytes.data(), bytes.size());
  return out; // 43 characters
}
inline bool shareToken(const std::string& value) {
  const size_t prefix = strlen(sharePrefix);
  return value.size() == prefix + 43 && value.compare(0, prefix, sharePrefix) == 0 && validToken(value.substr(prefix));
}
// File request outcomes the client (and the relay) may report.
inline bool errorCode(const std::string& code) {
  static const std::set<std::string> codes{"drive_not_found", "drive_exists", "drive_not_empty", "drive_read_only", "drive_denied", "drive_link_refused", "drive_invalid_path", "drive_share_unknown", "drive_busy", "drive_no_space", "drive_is_directory", "drive_not_directory", "drive_io_failed"};
  return codes.count(code) != 0;
}
// The WebDAV status of a failed request. Local reasons: drive_unavailable
// (no channel) 503, drive_timeout 504.
inline int httpStatus(const std::string& code) {
  if (code == "drive_not_found") return 404;
  if (code == "drive_exists") return 405;
  if (code == "drive_read_only" || code == "drive_denied" || code == "drive_link_refused" || code == "drive_share_unknown" || code == "drive_not_empty") return 403;
  if (code == "drive_invalid_path") return 400;
  if (code == "drive_busy") return 423;
  if (code == "drive_no_space") return 507;
  if (code == "drive_is_directory" || code == "drive_not_directory") return 409;
  if (code == "drive_unavailable") return 503;
  if (code == "drive_timeout") return 504;
  return 502;
}
// Host diagnostics (hostAction action 247): fixed numeric codes only.
inline uint32_t diagnosticCode(const std::string& code) {
  static const std::map<std::string, uint32_t> codes{{"drive_webclient_unavailable", 1}, {"drive_letter_unavailable", 2}, {"drive_map_failed", 3}, {"no_interactive_session", 4}, {"drive_policy_changed", 5}, {"session_ended", 6}, {"drive_transport_closed", 7}, {"drive_timeout", 8}, {"authorization_revoked", 9}, {"drive_invalid_message", 10}, {"drive_server_failed", 11}, {"drive_helper_failed", 12}};
  const auto found = codes.find(code);
  return found == codes.end() ? 99 : found->second;
}
inline constexpr uint32_t diagnosticAction = 247;
}
