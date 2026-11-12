#pragma once
#include "windows_autologon.hpp"
#include "pending_connection.hpp"
#include <ntsecapi.h>
#include <lm.h>
#include <ctime>

namespace host::autologon {
// The adapter is never used by the memory-only QA executable.
struct Bytes {
  static constexpr size_t capacity = 196608;
  std::string data;
  Bytes() { data.reserve(capacity); }
  ~Bytes() { SecureZeroMemory(data.data(), data.size()); }
  void room(size_t size) const { if (size > capacity - data.size()) throw Failure("recovery_invalid"); }
  void number(uint32_t n) { room(4); for (int i = 0; i < 4; ++i) data += char(n >> (i * 8)); }
  void text(const std::string& value) { if (value.size() > 8192) throw Failure("recovery_invalid"); room(value.size() + 4); number(uint32_t(value.size())); data += value; }
  void raw(const std::vector<uint8_t>& value) { room(value.size()); for (const auto byte : value) data += char(byte); }
  void wideText(const std::wstring& value) { std::string textValue = utf8(value); ProtectedTextScrubber scrub{textValue}; text(textValue); }
};
inline void encodeSnapshot(Bytes& out, const Snapshot& value) {
  for (size_t i = 0; i < value.values.size(); ++i) {
    const auto& item = value.values[i];
    if (i == 5 && !item.bytes.empty()) throw Failure("recovery_invalid");
    out.number(item.exists ? 1u : 0u); out.number(item.type);
    if (item.bytes.size() > 4096) throw Failure("recovery_invalid");
    out.number(uint32_t(item.bytes.size()));
    out.raw(item.bytes);
  }
  out.number(value.secretExists ? 1u : 0u); out.wideText(value.secret.text);
  out.number(value.policyBlocked ? 1u : 0u);
}
inline void encodeRecord(Bytes& out, const Record& record) {
  out.number(record.schema); out.text(record.operationId); out.text(record.action); out.text(record.phase);
  out.wideText(record.account.name); out.wideText(record.account.domain); out.wideText(record.account.sid);
  out.number(record.account.usable ? 1u : 0u);
  encodeSnapshot(out, record.baseline); encodeSnapshot(out, record.expected); encodeSnapshot(out, record.next);
  out.number(record.intent ? 1u : 0u);
  // Schema1 bytes remain unchanged. Schema2 adds presence-only plaintext size;
  // it never serializes the external registry password contents.
  if (record.schema == 2) {
    out.number(record.baseline.plainPasswordBytes);
    out.number(record.expected.plainPasswordBytes);
    out.number(record.next.plainPasswordBytes);
  }
  if (out.data.size() > 131072) throw Failure("recovery_invalid");
}
struct Decoder {
  const std::string& data; size_t at = 0;
  uint32_t number() {
    if (at + 4 > data.size()) throw Failure("recovery_invalid");
    uint32_t n = 0; for (int i = 0; i < 4; ++i) n |= uint32_t(uint8_t(data[at++])) << (i * 8); return n;
  }
  bool boolean() { const auto n = number(); if (n > 1) throw Failure("recovery_invalid"); return n != 0; }
  std::string text() { const auto n = number(); if (n > 8192 || n > data.size() - at) throw Failure("recovery_invalid"); auto out = data.substr(at, n); at += n; return out; }
  std::wstring wideText() { auto value = text(); ProtectedTextScrubber scrub{value}; return wide(value); }
  Snapshot snapshot() {
    Snapshot out;
    for (auto& item : out.values) {
      item.exists = boolean(); item.type = number(); const auto n = number();
      if (n > 4096 || n > data.size() - at || (!item.exists && (n || item.type))) throw Failure("recovery_invalid");
      item.bytes.assign(data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + n)); at += n;
    }
    out.secretExists = boolean(); out.secret.text = wideText(); out.policyBlocked = boolean();
    if (!out.secretExists && !out.secret.text.empty()) throw Failure("recovery_invalid");
    return out;
  }
  Record record() {
    Record r; r.schema = number(); r.operationId = text(); r.action = text(); r.phase = text();
    r.account.name = wideText(); r.account.domain = wideText(); r.account.sid = wideText(); r.account.usable = boolean();
    r.baseline = snapshot(); r.expected = snapshot(); r.next = snapshot(); r.intent = boolean();
    if (r.schema == 2) {
      r.baseline.plainPasswordBytes = number(); r.expected.plainPasswordBytes = number(); r.next.plainPasswordBytes = number();
    } else if (r.schema != 1) throw Failure("recovery_invalid");
    if (at != data.size()) throw Failure("recovery_invalid"); return r;
  }
};
struct Registry {
  HKEY key = nullptr;
  Registry(const wchar_t* path, REGSAM rights, bool optional = false) {
    const auto error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, rights | KEY_WOW64_64KEY, &key);
    if (optional && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return;
    if (error != ERROR_SUCCESS) throw Failure("state_unavailable");
  }
  ~Registry() { if (key) RegCloseKey(key); }
  Registry(const Registry&) = delete;
};
inline Value readValue(HKEY key, const wchar_t* name) {
  Value out; if (!key) return out;
  DWORD size = 0, type = 0;
  auto error = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
  if (error == ERROR_FILE_NOT_FOUND) return out;
  if (error != ERROR_SUCCESS || size > 4096) throw Failure("state_unavailable");
  out.exists = true; out.type = type; out.bytes.resize(size);
  error = RegQueryValueExW(key, name, nullptr, &type, out.bytes.data(), &size);
  if (error != ERROR_SUCCESS || size != out.bytes.size() || type != out.type) throw Failure("state_unavailable");
  return out;
}
struct Policy {
  LSA_HANDLE handle = nullptr;
  explicit Policy(ACCESS_MASK rights) {
    LSA_OBJECT_ATTRIBUTES attributes{}; attributes.Length = sizeof(attributes);
    if (LsaOpenPolicy(nullptr, &attributes, rights, &handle) != 0) throw Failure("state_unavailable");
  }
  ~Policy() { if (handle) LsaClose(handle); }
};
inline LSA_UNICODE_STRING secretName() {
  static wchar_t name[] = L"DefaultPassword";
  return {USHORT(15 * sizeof(wchar_t)), USHORT(16 * sizeof(wchar_t)), name};
}
inline void readSecret(Snapshot& out) {
  Policy policy(POLICY_GET_PRIVATE_INFORMATION); auto name = secretName(); PLSA_UNICODE_STRING secret = nullptr;
  const auto status = LsaRetrievePrivateData(policy.handle, &name, &secret);
  if (LsaNtStatusToWinError(status) == ERROR_FILE_NOT_FOUND) return;
  if (status != 0 || !secret) throw Failure("state_unavailable");
  struct Cleanup { PLSA_UNICODE_STRING value; ~Cleanup() { if (value->Buffer) SecureZeroMemory(value->Buffer, value->MaximumLength); LsaFreeMemory(value); } } cleanup{secret};
  if (secret->Length > 4096 || (secret->Length % sizeof(wchar_t)) != 0 || (secret->Length && !secret->Buffer)) throw Failure("state_unavailable");
  out.secretExists = true; if (secret->Length) out.secret.text.assign(secret->Buffer, secret->Length / sizeof(wchar_t));
}
inline constexpr const wchar_t* winlogonPath = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
inline bool policyRestricted() {
  Registry policy(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", KEY_QUERY_VALUE, true);
  const auto caption = readValue(policy.key, L"LegalNoticeCaption"), text = readValue(policy.key, L"LegalNoticeText");
  const auto nonempty = [](const Value& v) { return v.exists && std::any_of(v.bytes.begin(), v.bytes.end(), [](uint8_t b) { return b != 0; }); };
  if (nonempty(caption) || nonempty(text) || nonempty(readValue(policy.key, L"scforceoption"))) return true;
  // An existing EAS policy is not weakened or interpreted as permission.
  Registry eas(L"SYSTEM\\CurrentControlSet\\Control\\EAS\\Policies", KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, true);
  if (eas.key) {
    DWORD subkeys = 0, values = 0;
    if (RegQueryInfoKeyW(eas.key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) throw Failure("state_unavailable");
    if (subkeys || values) return true;
  }
  return false;
}
inline std::wstring computerName() {
  wchar_t value[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD size = DWORD(std::size(value));
  if (!GetComputerNameW(value, &size)) throw Failure("account_unavailable"); return {value, size};
}
// The NetUser/LSA reads behind localAccount; QA passes fakes (settings_rpc_test.cpp).
struct AccountLookup {
  decltype(&::NetUserGetInfo) getInfo = &::NetUserGetInfo;
  decltype(&::NetUserModalsGet) modals = &::NetUserModalsGet;
  decltype(&::NetApiBufferFree) free = &::NetApiBufferFree;
  decltype(&::LookupAccountSidW) lookupSid = &::LookupAccountSidW;
  std::wstring (*machine)() = computerName;
};
inline Account localAccount(const std::wstring& entered, const AccountLookup& api = {}) {
  auto name = entered; const auto slash = name.find(L'\\'); const auto machine = api.machine();
  if (slash != std::wstring::npos) {
    const auto domain = name.substr(0, slash);
    if (domain != L"." && _wcsicmp(domain.c_str(), machine.c_str()) != 0) throw Failure("local_account_required");
    name = name.substr(slash + 1);
  }
  if (name.empty() || name.size() > 256 || name.find_first_of(L"\\/@") != std::wstring::npos || std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; })) throw Failure("local_account_required");
  LPBYTE raw = nullptr;
  const auto error = api.getInfo(nullptr, name.c_str(), 4, &raw);
  if (error == NERR_UserNotFound) throw Failure("account_invalid");
  if (error != NERR_Success || !raw) throw Failure("account_unavailable");
  struct Cleanup { const AccountLookup& api; LPBYTE p; ~Cleanup() { api.free(p); } } cleanup{api, raw};
  const auto* info = reinterpret_cast<USER_INFO_4*>(raw);
  if (!info->usri4_user_sid) throw Failure("account_invalid");
  DWORD userLength = 0, domainLength = 0; SID_NAME_USE sidUse{};
  api.lookupSid(nullptr, info->usri4_user_sid, nullptr, &userLength, nullptr, &domainLength, &sidUse);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !userLength || !domainLength || userLength > 257 || domainLength > 257) throw Failure("account_unavailable");
  std::vector<wchar_t> sidUser(userLength), sidDomain(domainLength);
  if (!api.lookupSid(nullptr, info->usri4_user_sid, sidUser.data(), &userLength, sidDomain.data(), &domainLength, &sidUse)) throw Failure("account_unavailable");
  if (sidUse != SidTypeUser || _wcsicmp(sidDomain.data(), machine.c_str()) != 0) throw Failure("local_account_required");
  LPWSTR sid = nullptr;
  if (!info->usri4_user_sid || !ConvertSidToStringSidW(info->usri4_user_sid, &sid)) throw Failure("account_invalid");
  Account out; out.name = info->usri4_name; out.domain = machine; out.sid = sid; LocalFree(sid);
  const auto flags = info->usri4_flags;
  out.usable = (flags & (UF_ACCOUNTDISABLE | UF_LOCKOUT | UF_PASSWORD_EXPIRED | UF_SMARTCARD_REQUIRED)) == 0 &&
    (info->usri4_acct_expires == TIMEQ_FOREVER || info->usri4_acct_expires > uint64_t(std::time(nullptr))) && info->usri4_password_expired == 0;
  if ((flags & UF_DONT_EXPIRE_PASSWD) == 0) {
    LPBYTE modal = nullptr;
    if (api.modals(nullptr, 0, &modal) != NERR_Success || !modal) throw Failure("account_unavailable");
    const auto maximum = reinterpret_cast<USER_MODALS_INFO_0*>(modal)->usrmod0_max_passwd_age;
    api.free(modal);
    if (maximum != TIMEQ_FOREVER && info->usri4_password_age >= maximum) out.usable = false;
  }
  LPBYTE internet = nullptr;
  const auto internetResult = api.getInfo(nullptr, name.c_str(), 24, &internet);
  if (internetResult != NERR_Success || !internet) throw Failure("account_unavailable");
  const bool linked = reinterpret_cast<USER_INFO_24*>(internet)->usri24_internet_identity != FALSE;
  api.free(internet); if (linked) throw Failure("local_account_required");
  return out;
}
inline std::wstring tokenSid(HANDLE token) {
  DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size < sizeof(TOKEN_USER) || size > 65536) throw Failure("account_changed");
  std::vector<BYTE> data(size);
  if (!GetTokenInformation(token, TokenUser, data.data(), size, &size)) throw Failure("account_changed");
  LPWSTR value = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &value)) throw Failure("account_changed");
  std::wstring out = value; LocalFree(value); return out;
}
class NativeAccess final : public Access {
 public:
  bool administrator() override { return elevated(); }
  bool lock() override { guard_ = nativeConnectionLock(L"Global\\MultitapHost.WindowsAutologon", 0); return bool(guard_); }
  void unlock() noexcept override { guard_.reset(); }
  Snapshot read() override {
    Snapshot out; Registry key(winlogonPath, KEY_QUERY_VALUE);
    for (size_t i = 0; i < names.size(); ++i) {
      if (i == 5) {
        // Presence/type/size only, never the plaintext payload. Explicit
        // takeover may delete this fixed value after disabling autologon.
        DWORD type = 0, size = 0;
        const auto result = RegQueryValueExW(key.key, names[i], nullptr, &type, nullptr, &size);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) throw Failure("state_unavailable");
        if (result == ERROR_SUCCESS) { out.values[i] = Value{true, type, {}}; out.plainPasswordBytes = size; }
      } else out.values[i] = readValue(key.key, names[i]);
    }
    readSecret(out); out.policyBlocked = policyRestricted(); return out;
  }
  std::optional<Record> load() override {
    const auto file = path(); const auto attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) { if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return {}; throw Failure("storage_unavailable"); }
    if (!trustedPrivateConnectionFile(file) || !trustedPrivateConnectionFile(file.parent_path())) throw Failure("storage_unavailable");
    std::ifstream input(file, std::ios::binary); std::vector<BYTE> data(196609); input.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())); data.resize(size_t(input.gcount()));
    if (data.empty() || data.size() > 196608) throw Failure("recovery_invalid");
    DATA_BLOB sealed{DWORD(data.size()), data.data()}, plain{};
    if (!CryptUnprotectData(&sealed, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) throw Failure("storage_unavailable");
    if (plain.cbData > 131072) { SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData); throw Failure("recovery_invalid"); }
    Bytes text; text.data.assign(reinterpret_cast<const char*>(plain.pbData), plain.cbData); SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData);
    Decoder decoder{text.data}; return decoder.record();
  }
  void save(const Record& record) override {
    prepareMachineDirectory(); const auto file = path();
    if (!trustedPrivateConnectionFile(file.parent_path())) throw Failure("storage_unavailable");
    if (std::filesystem::exists(file) && !trustedPrivateConnectionFile(file)) throw Failure("storage_unavailable");
    Bytes plain; encodeRecord(plain, record); DATA_BLOB source{DWORD(plain.data.size()), reinterpret_cast<BYTE*>(plain.data.data())}, sealed{};
    if (!CryptProtectData(&source, L"Multitap Windows automatic login recovery v1", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &sealed)) throw Failure("storage_unavailable");
    struct Cleanup { DATA_BLOB& blob; ~Cleanup() { if (blob.pbData) LocalFree(blob.pbData); } } cleanup{sealed};
    if (!writePrivateFile(file, sealed.pbData, sealed.cbData)) throw Failure("storage_unavailable");
  }
  void remove() override {
    const auto file = path(); if (std::filesystem::exists(file) && (!trustedPrivateConnectionFile(file) || !DeleteFileW(file.c_str()))) throw Failure("storage_unavailable");
  }
  std::string revision(const Snapshot& snapshot, const std::optional<Record>& record) override {
    Bytes bytes; bytes.text("multitap-windows-autologon-revision-v1"); encodeSnapshot(bytes, snapshot);
    bytes.number(snapshot.plainPasswordBytes);
    bytes.number(record ? 1u : 0u); if (record) encodeRecord(bytes, *record);
    const auto digest = connectionSha256(bytes.data); const char hex[] = "0123456789abcdef"; std::string out;
    for (const auto byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; } return out;
  }
  Account validateInteractive(const std::wstring& username, const Secret& password) override {
    auto account = localAccount(username); if (!account.usable) throw Failure("account_restricted");
    HANDLE token = nullptr;
    if (!LogonUserW(account.name.c_str(), account.domain.c_str(), password.text.c_str(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &token)) {
      const auto code = GetLastError();
      if (code == ERROR_LOGON_FAILURE || code == ERROR_WRONG_PASSWORD) throw Failure("credentials_rejected");
      throw Failure("account_restricted");
    }
    struct Cleanup { HANDLE token; ~Cleanup() { CloseHandle(token); } } cleanup{token};
    const bool same = account.sid == tokenSid(token);
    if (!same || localAccount(account.name).sid != account.sid) throw Failure("account_changed");
    return account;
  }
  Account inspectAccount(const Account& account) override {
    try { return localAccount(account.name); }
    catch (const Failure& failure) {
      const std::string code = failure.what();
      if (code == "account_invalid" || code == "local_account_required") return {};
      throw;
    }
  }
  Observation observeLogin(const Account& account) override {
    // Observation only: this does not prove AutoAdminLogon caused the login.
    const DWORD session = WTSGetActiveConsoleSessionId(); if (session == 0xffffffff) return {};
    LPWSTR name = nullptr, domain = nullptr; DWORD nameBytes = 0, domainBytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &name, &nameBytes)) return {};
    const bool domainOk = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSDomainName, &domain, &domainBytes) != FALSE;
    std::wstring selected = name ? name : L"", selectedDomain = domainOk && domain ? domain : L"";
    if (name) WTSFreeMemory(name); if (domain) WTSFreeMemory(domain);
    if (selected.empty() || _wcsicmp(selectedDomain.c_str(), account.domain.c_str()) != 0) return {};
    if (_wcsicmp(selected.c_str(), account.name.c_str()) != 0) return {};
    HANDLE ownToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownToken)) return {};
    std::wstring ownSid;
    try { ownSid = tokenSid(ownToken); } catch (...) { CloseHandle(ownToken); return {}; }
    CloseHandle(ownToken);
    if (ownSid == L"S-1-5-18") {
      HANDLE actualToken = nullptr;
      if (!WTSQueryUserToken(session, &actualToken)) return {};
      bool same = false; try { same = tokenSid(actualToken) == account.sid; } catch (...) {}
      CloseHandle(actualToken); if (!same) return {};
    } else {
      DWORD ownSession = 0xffffffff;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) || ownSession != session || ownSid != account.sid) return {};
    }
    SYSTEMTIME now{}; GetSystemTime(&now); char stamp[32]{};
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ", unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay), unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond));
    return {true, stamp};
  }
  void writeValue(size_t index, const Value& value) override {
    if (index >= names.size() || (index == 5 && (value.exists || value.type || !value.bytes.empty()))) throw Failure("invalid_request");
    // Index5 is delete-only; no plaintext RegSetValueEx path exists.
    Registry key(winlogonPath, KEY_SET_VALUE);
    const auto result = value.exists ? RegSetValueExW(key.key, names[index], 0, value.type, value.bytes.data(), DWORD(value.bytes.size())) : RegDeleteValueW(key.key, names[index]);
    if (result != ERROR_SUCCESS && !(result == ERROR_FILE_NOT_FOUND && !value.exists)) throw Failure("write_failed");
    if (RegFlushKey(key.key) != ERROR_SUCCESS) throw Failure("write_failed");
  }
  void writeSecret(bool exists, const Secret& secret) override {
    Policy policy(POLICY_CREATE_SECRET); auto name = secretName();
    if (secret.text.size() > 256) throw Failure("invalid_credentials");
    LSA_UNICODE_STRING value{USHORT(secret.text.size() * sizeof(wchar_t)), USHORT((secret.text.size() + 1) * sizeof(wchar_t)), const_cast<PWSTR>(secret.text.c_str())};
    if (LsaStorePrivateData(policy.handle, &name, exists ? &value : nullptr) != 0) throw Failure("write_failed");
  }
 private:
  ConnectionGuardPtr guard_;
  static std::filesystem::path path() { return machineDirectory() / L"private" / L"windows-autologon.dpapi"; }
};
inline std::string statusJson(const Status& status) {
  return "{\"schema\":1,\"supported\":" + std::string(status.supported ? "true" : "false") +
    ",\"administrator\":" + (status.administrator ? "true" : "false") +
    ",\"supportsTakeOwnership\":" + (status.supportsTakeOwnership ? "true" : "false") +
    ",\"enabled\":" + (status.enabled ? (*status.enabled ? "true" : "false") : "null") +
    ",\"state\":" + jsonQuote(status.state) + ",\"reason\":" + jsonQuote(status.reason) +
    ",\"revision\":" + (status.revision.empty() ? "null" : jsonQuote(status.revision)) +
    ",\"accountLabel\":" + jsonQuote(utf8(status.account)) +
    ",\"loggedIn\":" + (status.loggedIn ? "true" : "false") + ",\"observedAt\":" + (status.observedAt.empty() ? "null" : jsonQuote(status.observedAt)) +
    ",\"operation\":" + (status.operationId.empty() ? "null" : "{\"id\":" + jsonQuote(status.operationId) + ",\"state\":" + jsonQuote(status.operationState) + "}") + "}";
}
inline std::string windowsAutologonStatus(const std::string& operation = {}) {
  NativeAccess access; Controller controller(access); return statusJson(controller.status(operation));
}
inline std::string windowsAutologonChange(const std::string& action, const Json& value) {
  if (action != "windowsAutologonApply" && action != "windowsAutologonDisable" && action != "windowsAutologonRepair") throw Failure("invalid_request");
  const auto* object = std::get_if<Json::Object>(&value.value);
  if (!object) throw Failure("invalid_request");
  for (const auto& field : *object) {
    const bool common = field.first == "action" || field.first == "operationId" || field.first == "expectedRevision" || field.first == "confirmed" ||
      (action != "windowsAutologonRepair" && field.first == "takeOwnership");
    const bool credential = action == "windowsAutologonApply" && (field.first == "username" || field.first == "password" || field.first == "passwordConfirmation");
    if (!common && !credential) throw Failure("invalid_request");
  }
  NativeAccess access; Controller controller(access); Request request;
  request.operationId = value.get("operationId").string(); request.expectedRevision = value.get("expectedRevision").string();
  request.confirmed = value.get("confirmed").boolean(); request.username = wide(value.get("username").string());
  if (const auto found = object->find("takeOwnership"); found != object->end()) {
    const auto* consent = std::get_if<bool>(&found->second.value);
    if (!consent) throw Failure("invalid_request");
    request.takeOwnership = *consent;
  }
  if (const auto* password = std::get_if<std::string>(&value.get("password").value)) request.password.text = wide(*password);
  if (const auto* confirmation = std::get_if<std::string>(&value.get("passwordConfirmation").value)) request.confirmation.text = wide(*confirmation);
  try {
    const auto status = action == "windowsAutologonApply" ? controller.apply(request) : action == "windowsAutologonDisable" ? controller.disable(request) : controller.repair(request);
    return "{\"ok\":true,\"autologon\":" + statusJson(status) + '}';
  } catch (const Failure& failure) {
    // Structured failure preserves fresh current state without exposing secrets or OS text.
    return "{\"ok\":true,\"rejected\":true,\"code\":" + jsonQuote(failure.what()) + ",\"autologon\":" + statusJson(controller.status(request.operationId)) + '}';
  }
}
inline std::string lifecycleOperationId() {
  const auto random = utf8(randomName());
  return random.substr(0, 8) + '-' + random.substr(8, 4) + '-' + random.substr(12, 4) + '-' + random.substr(16, 4) + '-' + random.substr(20);
}
// Root integration hooks. Never run these on the service startup critical path.
// Observe a bounded deadline externally without terminating an admitted owner.
// A failed/slow repair must not gate service reachability or manual streaming.
inline const char* windowsAutologonStartupRecovery() noexcept {
  try {
    NativeAccess access;
    { Guard guard(access); const auto record = access.load(); if (!record || record->phase != "changing") return "unchanged"; }
    Controller controller(access); const auto before = controller.status();
    if (before.reason != "repair_required" || !hex64(before.revision)) return "repair_required";
    Request request; request.operationId = lifecycleOperationId(); request.expectedRevision = before.revision; request.confirmed = true;
    const auto after = controller.repair(request);
    return after.operationId == request.operationId && after.operationState == "disabled" ? "recovered" : "repair_required";
  } catch (...) { return "repair_required"; }
}
// Call only after the installer's explicit Uninstall confirmation, before
// removing HostTool/private state. Failure must preserve the repair tool/record.
inline const char* windowsAutologonUninstallCleanup(bool confirmed) noexcept {
  if (!confirmed) return "confirmation_required";
  try {
    NativeAccess access;
    { Guard guard(access); if (!access.load()) return "unchanged"; }
    Controller controller(access); auto status = controller.status();
    if (!status.administrator || !hex64(status.revision)) return "repair_required";
    if (status.state == "failed" || status.state == "configured" || status.state == "observed" || status.state == "needs_update") {
      Request request; request.operationId = lifecycleOperationId(); request.expectedRevision = status.revision; request.confirmed = true;
      status = status.state == "failed" ? controller.repair(request) : controller.disable(request);
      if (status.operationId != request.operationId || status.operationState != "disabled") return "repair_required";
    }
    Guard guard(access); const auto record = access.load();
    if (!record) return "unchanged";
    if (record->phase != "disabled" || record->intent || !sameSettings(access.read(), record->expected)) return "repair_required";
    access.remove(); return "cleaned";
  } catch (...) { return "repair_required"; }
}
} // namespace host::autologon
