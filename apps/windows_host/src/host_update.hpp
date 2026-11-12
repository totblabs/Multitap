#pragma once
#include "operational_log.hpp"
#include "../../windows_client/windows/runner/product_update.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <thread>

// Administrator-initiated Host program update (host-update-v1).
//
// The broker sends only catalog metadata (version, file name, size, SHA-256,
// /downloads/ path, and the release signature when this Host announced
// release-signature support). This service refuses a release that is not
// signed with the project's pinned release key, downloads from ITS configured
// broker origin, verifies exact size and SHA-256 inside a protected stage,
// records the request, and starts the installer's unattended update mode. The
// installer stops/replaces this service; the restarted service reports the
// result. Nothing here reboots Windows, prompts, or accepts a host, URL or command.
namespace host {
inline constexpr const char* hostUpdateProtocol = "host-update-v1";
// Announced in host.update.hello; a broker that understands it answers with
// the same value in host.update.hello.ack and sends each release signature.
inline constexpr const char* hostUpdateReleaseSignature = multitap::release_signature::kSignatureDomain;
inline constexpr uint64_t hostUpdateMaximumSize = 128ULL * 1024 * 1024;

struct HostUpdateFailure : std::runtime_error { explicit HostUpdateFailure(const char* code) : std::runtime_error(code) {} };

inline bool hostUpdateVersion(const std::string& value) {
  static const std::regex pattern("[0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6}");
  return value.size() <= 20 && std::regex_match(value, pattern);
}
// Numeric x.y.z order; both inputs must already satisfy hostUpdateVersion.
inline int compareHostVersions(const std::string& left, const std::string& right) {
  const auto parts = [](const std::string& text) {
    std::array<unsigned long, 3> out{}; size_t at = 0;
    for (auto& part : out) { const auto end = text.find('.', at); part = std::stoul(text.substr(at, end == std::string::npos ? std::string::npos : end - at)); at = end == std::string::npos ? text.size() : end + 1; }
    return out;
  };
  const auto a = parts(left), b = parts(right);
  for (size_t i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
inline bool hostUpdateRequestId(const std::string& value) {
  static const std::regex pattern("[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}");
  return value.size() == 36 && std::regex_match(value, pattern);
}
// Fixed vocabulary shared with server/src/host-update.mjs hostUpdateCodes.
inline bool hostUpdateCode(const std::string& code) {
  static const std::set<std::string> codes{"", "invalid_request", "busy", "not_newer", "same_version", "session_active", "download_failed",
    "download_size_mismatch", "hash_mismatch", "not_executable", "stage_failed", "launch_failed", "install_failed", "install_result_missing",
    "certificate_trust_required", "driver_install_required", "existing_install_required", "display_attention", "restart_required", "result_unreported",
    "signature_invalid"};
  return codes.count(code) != 0;
}

struct HostUpdateRequest {
  // signature: empty when the broker sent none (an unsigned or older catalog).
  std::string requestId, version, filename, sha256, path, signature;
  uint64_t size = 0; bool allowDowngrade = false, disconnectSessions = false;
};
// Strict: exactly the broker's catalog metadata, nothing else. The download
// URL is always the configured origin plus /downloads/<exact installer name>.
inline std::optional<HostUpdateRequest> parseHostUpdateRequest(const Json& value) {
  const auto* fields = std::get_if<Json::Object>(&value.value);
  if (!fields || value.get("type").string() != "host.update.start") return std::nullopt;
  static const std::set<std::string> allowed{"type", "requestId", "version", "filename", "size", "sha256", "path", "allowDowngrade", "disconnectSessions", "signature"};
  const bool signedRequest = fields->count("signature") != 0;
  if (fields->size() != allowed.size() - (signedRequest ? 0 : 1)) return std::nullopt;
  for (const auto& [key, item] : *fields) { (void)item; if (!allowed.count(key)) return std::nullopt; }
  HostUpdateRequest request;
  if (signedRequest) {
    const auto* signature = std::get_if<std::string>(&value.get("signature").value);
    if (!signature || signature->size() > 128) return std::nullopt;
    request.signature = *signature;
  }
  const std::pair<const char*, std::string*> textFields[]{{"requestId", &request.requestId}, {"version", &request.version}, {"filename", &request.filename}, {"sha256", &request.sha256}, {"path", &request.path}};
  for (const auto& [key, target] : textFields) {
    const auto* text = std::get_if<std::string>(&value.get(key).value); if (!text) return std::nullopt; *target = *text;
  }
  const auto* size = std::get_if<double>(&value.get("size").value);
  const auto* downgrade = std::get_if<bool>(&value.get("allowDowngrade").value);
  const auto* disconnect = std::get_if<bool>(&value.get("disconnectSessions").value);
  if (!size || !downgrade || !disconnect || !std::isfinite(*size) || *size < 1 || *size > double(hostUpdateMaximumSize) || std::floor(*size) != *size) return std::nullopt;
  request.size = uint64_t(*size); request.allowDowngrade = *downgrade; request.disconnectSessions = *disconnect;
  static const std::regex digest("[a-f0-9]{64}");
  if (!hostUpdateRequestId(request.requestId) || !hostUpdateVersion(request.version) || !std::regex_match(request.sha256, digest) ||
      request.filename != "Multitap-Host-" + request.version + "-windows-x64.exe" || request.path != "/downloads/" + request.filename) return std::nullopt;
  return request;
}

struct HostUpdateDecision { std::string state, code; };
// One update at a time; never the running version; never older unless the
// administrator flagged it; a live session defers unless it was disconnected.
inline HostUpdateDecision decideHostUpdate(const std::optional<HostUpdateRequest>& request, const std::string& running, bool busy, bool sessionActive) {
  if (!request || !hostUpdateVersion(running)) return {"rejected", "invalid_request"};
  if (busy) return {"rejected", "busy"};
  const int order = compareHostVersions(request->version, running);
  if (order == 0) return {"rejected", "same_version"};
  if (order < 0 && !request->allowDowngrade) return {"rejected", "not_newer"};
  if (sessionActive && !request->disconnectSessions) return {"deferred", "session_active"};
  return {"accepted", ""};
}
// The project's own release signature over exactly this catalog metadata
// (product host, version, file name, size, SHA-256); the downloaded bytes are
// bound to it by the exact size and SHA-256 checks. Checked before any download.
inline bool hostUpdateSignatureValid(const HostUpdateRequest& request, const multitap::release_signature::Key& key) {
  return multitap::release_signature::VerifyRelease("host", request.version, request.filename, request.size, request.sha256, request.signature, key);
}

// Exact declared size (never more), SHA-256 and a Windows executable header.
class HostUpdateVerifier {
 public:
  HostUpdateVerifier(uint64_t size, std::string digest) : size_(size), digest_(std::move(digest)) {}
  void add(const uint8_t* data, size_t count) {
    if (!count) return;
    if (count > size_ - received_ || count > MAXDWORD) throw HostUpdateFailure("download_size_mismatch");
    for (size_t i = 0; i < count && header_ < 2; ++i) magic_[header_++] = data[i];
    hash_.Add(const_cast<uint8_t*>(data), DWORD(count)); received_ += count;
  }
  void finish() {
    if (received_ != size_) throw HostUpdateFailure("download_size_mismatch");
    if (hash_.Finish() != digest_) throw HostUpdateFailure("hash_mismatch");
    if (header_ != 2 || magic_[0] != 'M' || magic_[1] != 'Z') throw HostUpdateFailure("not_executable");
  }
  uint64_t received() const { return received_; }
 private:
  uint64_t size_, received_ = 0; std::string digest_; multitap::update_detail::Hash hash_; std::array<uint8_t, 2> magic_{}; size_t header_ = 0;
};

struct HostUpdateDownload {
  virtual ~HostUpdateDownload() = default;
  virtual DWORD status() = 0;
  virtual int64_t contentLength() = 0;  // -1 when not declared
  virtual size_t read(uint8_t* buffer, size_t size) = 0;  // 0 at the end
};
// GET of one exact release path on the configured origin. HTTPS keeps normal
// certificate validation; HTTP requires protected exact consent and health.
// Publisher/signature, size, SHA, version and no-redirect checks stay separate.
class WinHttpHostUpdateDownload final : public HostUpdateDownload {
 public:
  WinHttpHostUpdateDownload(const std::wstring& origin, const std::wstring& path,const std::wstring& consent={})
      : origin_(verifiedBrokerOrigin(origin,consent)), session_(httpSession()), connection_(WinHttpConnect(session_.handle, origin_.hostname.c_str(), origin_.port, 0)),
        request_(WinHttpOpenRequest(connection_.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, origin_.flags())) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    winCheck(WinHttpSetOption(request_.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)), "Disable Host update redirects");
    WinHttpSetTimeouts(request_.handle, 15000, 15000, 30000, 30000);
    winCheck(WinHttpSendRequest(request_.handle, L"Accept: application/vnd.microsoft.portable-executable\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "Request Host update");
    winCheck(WinHttpReceiveResponse(request_.handle, nullptr), "Receive Host update");
    DWORD length = sizeof(status_);
    winCheck(WinHttpQueryHeaders(request_.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_, &length, WINHTTP_NO_HEADER_INDEX), "Read Host update status");
    DWORD declared = 0; length = sizeof(declared);
    if (WinHttpQueryHeaders(request_.handle, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &declared, &length, WINHTTP_NO_HEADER_INDEX)) length_ = declared;
  }
  DWORD status() override { return status_; }
  int64_t contentLength() override { return length_; }
  size_t read(uint8_t* buffer, size_t size) override { DWORD actual = 0; winCheck(WinHttpReadData(request_.handle, buffer, DWORD(std::min<size_t>(size, 1 << 20)), &actual), "Read Host update"); return actual; }
 private:
  Origin origin_; Internet session_, connection_, request_; DWORD status_ = 0; int64_t length_ = -1;
};

// ProgramData\Multitap-Updates\{GUID}: owner Administrators, only SYSTEM and
// Administrators (the installer's verified-parent check requires exactly this).
inline std::filesystem::path hostUpdateStageRoot() { return programDataFolder() / L"Multitap-Updates"; }
inline std::filesystem::path createProtectedHostUpdateStage(const std::filesystem::path& root) {
  using namespace multitap::update_detail;
  Descriptor rootSecurity, stageSecurity;
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &rootSecurity.value, nullptr));
  // Files written into the stage (installer, result) inherit SYSTEM/Administrators only.
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &stageSecurity.value, nullptr));
  SECURITY_ATTRIBUTES rootAttributes{sizeof(rootAttributes), rootSecurity.value, FALSE}, stageAttributes{sizeof(stageAttributes), stageSecurity.value, FALSE};
  // A root a standard user created first is replaced, not trusted.
  SecureUpdateRoot(root.wstring(), rootAttributes);
  { Handle handle{DirectoryHandle(root.wstring())}; TrustedDirectory(handle.value); }
  GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid))); wchar_t suffix[40]{}; Require(StringFromGUID2(guid, suffix, 40) > 0);
  const auto stage = root / suffix; Require(CreateDirectoryW(stage.c_str(), &stageAttributes));
  { Handle handle{DirectoryHandle(stage.wstring())}; TrustedDirectory(handle.value); }
  return stage;
}
inline bool hostUpdateStageName(const std::filesystem::path& stage, const std::filesystem::path& root) {
  static const std::wregex guid(L"\\{[A-F0-9]{8}-[A-F0-9]{4}-[A-F0-9]{4}-[A-F0-9]{4}-[A-F0-9]{12}\\}");
  return !root.empty() && _wcsicmp(stage.parent_path().c_str(), root.c_str()) == 0 && std::regex_match(stage.filename().wstring(), guid);
}
// The installer keeps its msiexec log (and, without the package cache, the
// package) under %TEMP%. For SYSTEM that is C:\Windows\Temp, where every user may
// create files inside new subfolders (BUILTIN\Users: CI write/append), so the
// installer gets TEMP/TMP inside its protected stage instead: owner
// Administrators, only SYSTEM and Administrators, inherited by what it unpacks.
inline std::filesystem::path hostUpdateTemporaryDirectory(const std::filesystem::path& stage) { return stage / L"temp"; }
inline bool createHostUpdateTemporaryDirectory(const std::filesystem::path& directory) {
  using namespace multitap::update_detail;
  Descriptor security;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &security.value, nullptr)) return false;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.value, FALSE};
  if (!CreateDirectoryW(directory.c_str(), &attributes)) return false;
  if (TrustedDirectoryPath(directory.wstring())) return true;
  RemoveDirectoryW(directory.c_str()); return false;
}
// Child-only receiver capability plus protected TEMP/TMP; no global process
// environment mutation. Replace inherited names case-insensitively so a stale
// value cannot win. Pure entry transformation is also used by the fixture.
inline std::wstring hostUpdateEnvironmentFromEntries(const std::filesystem::path& temporary,const std::vector<std::wstring>& inherited) {
  static constexpr wchar_t protocolName[]=L"MULTITAP_HOST_UPDATE_RESULT_PROTOCOL=";
  std::vector<std::wstring> entries;
  for(const auto& entry:inherited){
    if(_wcsnicmp(entry.c_str(),L"TEMP=",5)==0||_wcsnicmp(entry.c_str(),L"TMP=",4)==0||
       _wcsnicmp(entry.c_str(),protocolName,std::size(protocolName)-1)==0)continue;
    entries.push_back(entry);
  }
  entries.push_back(L"TEMP="+temporary.wstring());entries.push_back(L"TMP="+temporary.wstring());
  entries.push_back(std::wstring(protocolName)+L"2");
  const auto name=[](const std::wstring& entry){const auto end=entry.find(L'=',1);return entry.substr(0,end==std::wstring::npos?entry.size():end);};
  std::stable_sort(entries.begin(),entries.end(),[&](const std::wstring& a,const std::wstring& b){
    const auto left=name(a),right=name(b);
    return CompareStringOrdinal(left.c_str(),int(left.size()),right.c_str(),int(right.size()),TRUE)==CSTR_LESS_THAN;
  });
  std::wstring block;
  for(const auto& entry:entries){block+=entry;block.push_back(L'\0');}
  block.push_back(L'\0');return block;
}
inline std::wstring hostUpdateEnvironmentBlock(const std::filesystem::path& temporary) {
  std::vector<std::wstring> inherited;
  if(LPWCH current=GetEnvironmentStringsW()){
    for(const wchar_t* at=current;*at;at+=wcslen(at)+1)inherited.emplace_back(at);
    FreeEnvironmentStringsW(current);
  }
  return hostUpdateEnvironmentFromEntries(temporary,inherited);
}
// Starts the downloaded installer's unattended service mode outside this
// service's lifetime: it stops and replaces this very process.
inline bool launchHostUpdateInstaller(const std::filesystem::path& installer, const std::filesystem::path& stage, bool allowDowngrade) {
  const auto temporary = hostUpdateTemporaryDirectory(stage);
  if (!createHostUpdateTemporaryDirectory(temporary)) return false;
  auto environment = hostUpdateEnvironmentBlock(temporary);
  // /MultitapDowngrade: the administrator explicitly asked for an older release
  // (the broker request carries allowDowngrade); otherwise the setup refuses one.
  const std::wstring flags_text = std::wstring(L" /MultitapUpdate /MultitapService") + (allowDowngrade ? L" /MultitapDowngrade" : L"");
  std::wstring command = L"\"" + installer.wstring() + L"\"" + flags_text;
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  BOOL created = CreateProcessW(installer.c_str(), command.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, environment.data(), stage.c_str(), &startup, &process);
  if (!created && GetLastError() == ERROR_ACCESS_DENIED) { command = L"\"" + installer.wstring() + L"\"" + flags_text; created = CreateProcessW(installer.c_str(), command.data(), nullptr, nullptr, FALSE, flags, environment.data(), stage.c_str(), &startup, &process); }
  if (!created) { RemoveDirectoryW(temporary.c_str()); return false; }
  CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}
inline int64_t hostUpdateClock() { FILETIME now{}; GetSystemTimeAsFileTime(&now); return int64_t(((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime) / 10000ULL - 11644473600000ULL); }

struct HostUpdatePending { std::string requestId, fromVersion, targetVersion; std::filesystem::path stage; int64_t startedAt = 0; };
inline std::string hostUpdatePendingJson(const HostUpdatePending& pending) {
  return "{\"schema\":1,\"requestId\":" + jsonQuote(pending.requestId) + ",\"fromVersion\":" + jsonQuote(pending.fromVersion) + ",\"targetVersion\":" + jsonQuote(pending.targetVersion) +
    ",\"stage\":" + jsonQuote(utf8(pending.stage.wstring())) + ",\"startedAt\":" + std::to_string(pending.startedAt) + '}';
}
inline std::optional<HostUpdatePending> parseHostUpdatePending(const std::string& bytes, const std::filesystem::path& root) {
  try {
    const auto value = JsonParser::parse(bytes); const auto* fields = std::get_if<Json::Object>(&value.value);
    if (!fields || fields->size() != 6 || value.get("schema").number() != 1) return std::nullopt;
    HostUpdatePending pending{value.get("requestId").string(), value.get("fromVersion").string(), value.get("targetVersion").string(), wide(value.get("stage").string()), 0};
    const auto* started = std::get_if<double>(&value.get("startedAt").value);
    if (!started || !std::isfinite(*started) || *started < 0 || *started > 9e15 || std::floor(*started) != *started) return std::nullopt;
    pending.startedAt = int64_t(*started);
    if (!hostUpdateRequestId(pending.requestId) || !hostUpdateVersion(pending.fromVersion) || !hostUpdateVersion(pending.targetVersion) || !hostUpdateStageName(pending.stage, root)) return std::nullopt;
    return pending;
  } catch (...) { return std::nullopt; }
}
// Written by the installer's service mode into the stage directory (host_installer.cpp writeServiceResult).
struct HostUpdateResult { bool completed = false, rebootRequired = false, attention = false; std::string version, code; };
inline std::optional<HostUpdateResult> parseHostUpdateResult(const std::string& bytes) {
  try {
    const auto value = JsonParser::parse(bytes); const auto* fields = std::get_if<Json::Object>(&value.value);
    if (!fields || fields->size() != 8 || value.get("schema").number() != 1 || value.get("product").string() != "host") return std::nullopt;
    const auto outcome = value.get("outcome").string(); const auto completed = value.get("completed").number(-1);
    const auto* reboot = std::get_if<bool>(&value.get("rebootRequired").value); const auto* attention = std::get_if<bool>(&value.get("attention").value);
    const auto* code = std::get_if<std::string>(&value.get("code").value);
    if ((outcome != "completed" && outcome != "failed") || !std::holds_alternative<double>(value.get("completed").value) || completed < 0 || completed > 6 || std::floor(completed) != completed || !reboot || !attention || !code) return std::nullopt;
    HostUpdateResult result{outcome == "completed", *reboot, *attention, value.get("version").string(), *code};
    static const std::set<std::string> completedCodes{"", "display_attention"}, failedCodes{"install_failed", "existing_install_required", "certificate_trust_required", "driver_install_required", "not_newer", "stage_failed", "launch_failed"};
    if (!hostUpdateVersion(result.version) || !(result.completed ? completedCodes.count(result.code) : failedCodes.count(result.code))) return std::nullopt;
    return result;
  } catch (...) { return std::nullopt; }
}
struct HostUpdateReport { std::string requestId, state, code, version; bool rebootRequired = false; };
// Completion means THIS running service is the requested version.
inline HostUpdateReport hostUpdateFinalReport(const HostUpdatePending& pending, const std::optional<HostUpdateResult>& result, const std::string& running, bool timedOut) {
  if (result) {
    if (result->completed && result->version == pending.targetVersion && running == pending.targetVersion) return {pending.requestId, "completed", result->code, running, result->rebootRequired};
    return {pending.requestId, "failed", result->completed ? "install_failed" : result->code, running, result->rebootRequired};
  }
  if (timedOut) return running == pending.targetVersion ? HostUpdateReport{pending.requestId, "completed", "result_unreported", running, false} : HostUpdateReport{pending.requestId, "failed", "install_result_missing", running, false};
  return {pending.requestId, "installing", "", running, false};
}
// An unattended update never trusts a certificate or installs a driver. When it
// finished but this Host has no virtual display at all (a first MSI install
// of an earlier Host never installed one), completion carries the existing
// `display_attention` code, so the administrator is told to open Host settings
// on the VM, where the install is finished after one confirmation
// (host_prerequisites.hpp). Any other code a result already carries stays.
inline std::string hostUpdateCompletionCode(const std::string& state, const std::string& code, bool displayMissing) {
  return state == "completed" && code.empty() && displayMissing ? "display_attention" : code;
}
// No native provider selection whose node Windows still lists, and no legacy
// adapter on record (legacy_display_identity.hpp). Unknown (a read failed) is "not missing".
inline bool hostVirtualDisplayMissing() noexcept {
  try {
    bool present = false; const auto config = nativeDisplayConfig(&present);
    if (nativeProviderSelected(config, present)) {
      auto instance = wide(config.get("deviceInstanceId").string()); DEVINST node = 0;
      return nativeDisplayInstanceId(config.get("deviceInstanceId").string()) && CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_PHANTOM) == CR_NO_SUCH_DEVNODE;
    }
    wchar_t legacy[128]{}; DWORD bytes = sizeof(legacy) - sizeof(wchar_t);
    const auto read = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Multitap\\Display", L"DeviceInstance", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, legacy, &bytes);
    return read == ERROR_FILE_NOT_FOUND || (read == ERROR_SUCCESS && !legacy[0]);
  } catch (...) { return false; }
}
inline std::string hostUpdateReportFields(const HostUpdateReport& report) {
  return "\"requestId\":" + jsonQuote(report.requestId) + ",\"state\":" + jsonQuote(report.state) + ",\"code\":" + jsonQuote(report.code) + ",\"version\":" + jsonQuote(report.version) + ",\"rebootRequired\":" + (report.rebootRequired ? "true" : "false");
}

struct HostUpdateEnvironment {
  std::string runningVersion;
  std::wstring origin;  // the configured broker origin; never from a message
  std::function<bool()> sessionActive = [] { return false; };
  std::function<std::unique_ptr<HostUpdateDownload>(const std::wstring& origin, const std::wstring& path)> open;
  std::filesystem::path stageRoot;
  std::function<std::filesystem::path(const std::filesystem::path& root)> createStage;
  std::function<bool(const std::filesystem::path& installer, const std::filesystem::path& stage, bool allowDowngrade)> launch;
  std::filesystem::path pendingPath;
  std::function<bool(const std::filesystem::path&)> trusted;
  std::function<int64_t()> now = hostUpdateClock;
  int64_t resultTimeoutMs = 15 * 60 * 1000, pollMs = 2000;
  // True when this Host has no virtual display (hostVirtualDisplayMissing).
  std::function<bool()> displayMissing;
  // The pinned project release key; fixtures substitute their own.
  multitap::release_signature::Key releaseKey = multitap::release_signature::PinnedKey();
};
inline HostUpdateEnvironment serviceHostUpdateEnvironment(const std::wstring& origin, std::function<bool()> sessionActive) {
  HostUpdateEnvironment environment;
  environment.runningVersion = MULTITAP_HOST_VERSION_UTF8; environment.origin = origin; environment.sessionActive = std::move(sessionActive);
  environment.open = [](const std::wstring& base, const std::wstring& path) -> std::unique_ptr<HostUpdateDownload> {
    const auto requested=transport::parse(Origin::asciiAddress(base));
    if(requested.secure())return std::make_unique<WinHttpHostUpdateDownload>(base,path);
    Credentials current;ProtectedTextScrubber scrubber{current.token};
    if(!loadCredentials(current)||credentialOrigin(current).policy.text!=requested.text)throw std::runtime_error("http_consent_required");
    return std::make_unique<WinHttpHostUpdateDownload>(base,path,current.httpConsentOrigin);
  };
  environment.stageRoot = hostUpdateStageRoot(); environment.createStage = createProtectedHostUpdateStage; environment.launch = launchHostUpdateInstaller;
  environment.pendingPath = machineDirectory() / L"private" / L"host-update.json"; environment.trusted = trustedMachineFile;
  environment.displayMissing = hostVirtualDisplayMissing;
  return environment;
}
// Fixed diagnostics (hostAction 230): stage = state number, error = code index.
inline void recordHostUpdateEvent(const std::string& state, const std::string& code) noexcept {
  static const std::array<const char*, 8> states{"accepted", "downloading", "verifying", "installing", "completed", "failed", "deferred", "rejected"};
  // Append only: the index is the recorded error number (server/admin/js/labels.js hostUpdateEventCodes).
  static const std::array<const char*, 21> codes{"", "invalid_request", "busy", "not_newer", "same_version", "session_active", "download_failed", "download_size_mismatch", "hash_mismatch", "not_executable",
    "stage_failed", "launch_failed", "install_failed", "install_result_missing", "certificate_trust_required", "driver_install_required", "existing_install_required", "display_attention", "restart_required", "result_unreported",
    "signature_invalid"};
  uint32_t stage = 0, error = 0;
  for (size_t i = 0; i < states.size(); ++i) if (state == states[i]) stage = uint32_t(i + 1);
  for (size_t i = 0; i < codes.size(); ++i) if (code == codes[i]) error = uint32_t(i);
  recordHostEvent("hostAction", {{"action", uint32_t(230)}, {"stage", stage}, {"error", error}, {"ok", state != "failed" && state != "rejected"}});
}

class HostUpdater {
 public:
  using Send = std::function<void(const std::string&)>;
  HostUpdater(HostUpdateEnvironment environment, Send send) : environment_(std::move(environment)), send_(std::move(send)) { loadPending(); }
  ~HostUpdater() { stop(); }
  HostUpdater(const HostUpdater&) = delete;
  void stop() { stopping_ = true; if (worker_.joinable()) worker_.join(); }
  // First message of every channel. A finished result travels here too, so a
  // restarted service reports even if it missed the broker's progress view.
  std::string hello() {
    std::lock_guard lock(mutex_);
    brokerSupports_ = false; brokerSignatures_ = false; finalSent_ = false; refreshPending(true);
    std::string message = "{\"type\":\"host.update.hello\",\"protocol\":\"host-update-v1\",\"version\":" + jsonQuote(environment_.runningVersion) +
        ",\"releaseSignature\":" + jsonQuote(hostUpdateReleaseSignature);
    if (pending_) { message += ",\"result\":{" + hostUpdateReportFields(report_) + '}'; helloCarriedFinal_ = report_.state != "installing"; }
    return message + '}';
  }
  void acknowledged(const Json& message) {
    std::lock_guard lock(mutex_);
    const auto type = message.get("type").string();
    if (type == "host.update.hello.ack") {
      if (message.get("protocol").string() != hostUpdateProtocol) return;
      brokerSupports_ = true;
      // This broker knows the signature_invalid code (older ones would ignore it).
      brokerSignatures_ = message.get("releaseSignature").string() == hostUpdateReleaseSignature;
      if (pending_ && helloCarriedFinal_ && message.get("result").string() == pending_->requestId) completePending();
    } else if (type == "host.update.status.ack" && pending_ && finalSent_ && message.get("requestId").string() == pending_->requestId) completePending();
  }
  void start(const Json& message) {
    const auto request = parseHostUpdateRequest(message);
    const auto replyId = request ? request->requestId : message.get("requestId").string();
    if (!hostUpdateRequestId(replyId)) return;  // Nothing correlatable to answer.
    HostUpdateDecision decision;
    {
      std::lock_guard lock(mutex_);
      if (worker_.joinable() && !running_) worker_.join();
      const bool busy = running_ || pending_.has_value();
      decision = decideHostUpdate(request, environment_.runningVersion, busy, !busy && request && environment_.sessionActive());
      // An unsigned or mismatching release is refused before anything is
      // downloaded, and is never deferred to be retried. A broker without
      // release-signature support hears a code it already knows.
      if ((decision.state == "accepted" || decision.state == "deferred") && !hostUpdateSignatureValid(*request, environment_.releaseKey))
        decision = {"rejected", brokerSignatures_ ? "signature_invalid" : "invalid_request"};
      if (decision.state == "accepted") { running_ = true; stopping_ = false; }  // Reserved: a second request is "busy".
    }
    // "accepted" is sent before the worker can report any progress.
    status(replyId, decision.state, decision.code);
    if (decision.state != "accepted") return;
    bool started = false;
    {
      std::lock_guard lock(mutex_);
      try { worker_ = std::thread([this, value = *request] { run(value); running_ = false; }); started = true; }
      catch (...) { running_ = false; }
    }
    if (!started) status(replyId, "failed", "launch_failed");
  }
  // Called about once a second from the sending thread.
  void poll() {
    std::lock_guard lock(mutex_);
    if (!pending_ || finalSent_) { retryCleanup(); return; }
    const auto now = environment_.now();
    if (now - lastPoll_ < environment_.pollMs) return;
    lastPoll_ = now; refreshPending(false);
    if (report_.state != "installing" && brokerSupports_) { send(hostUpdateReportFields(report_)); finalSent_ = true; recordHostUpdateEvent(report_.state, report_.code); }
  }
  bool busy() const { std::lock_guard lock(mutex_); return running_ || pending_.has_value(); }
  std::optional<HostUpdatePending> pending() const { std::lock_guard lock(mutex_); return pending_; }

 private:
  HostUpdateEnvironment environment_; Send send_;
  mutable std::mutex mutex_; std::thread worker_; std::atomic<bool> running_{false}, stopping_{false};
  std::optional<HostUpdatePending> pending_; HostUpdateReport report_; bool brokerSupports_ = false, brokerSignatures_ = false, finalSent_ = false, helloCarriedFinal_ = false;
  int64_t lastPoll_ = 0; std::vector<std::filesystem::path> cleanup_; int cleanupAttempts_ = 0;

  void send(const std::string& fields) { try { send_("{\"type\":\"host.update.status\"," + fields + '}'); } catch (...) { } }
  void status(const std::string& requestId, const std::string& state, const std::string& code, std::optional<std::pair<uint64_t, uint64_t>> progress = {}) {
    std::string fields = "\"requestId\":" + jsonQuote(requestId) + ",\"state\":" + jsonQuote(state) + ",\"code\":" + jsonQuote(code) + ",\"version\":" + jsonQuote(environment_.runningVersion);
    if (progress) fields += ",\"received\":" + std::to_string(progress->first) + ",\"total\":" + std::to_string(progress->second);
    send(fields);
    if (state != "downloading" || !progress || progress->first == 0) recordHostUpdateEvent(state, code);
  }
  bool readSmall(const std::filesystem::path& path, size_t limit, std::string& out) const {
    try {
      std::error_code error; if (!std::filesystem::is_regular_file(path, error) || !environment_.trusted || !environment_.trusted(path)) return false;
      const auto size = std::filesystem::file_size(path, error); if (error || size == 0 || size > limit) return false;
      std::ifstream file(path, std::ios::binary); out.assign(std::istreambuf_iterator<char>(file), {}); return out.size() == size;
    } catch (...) { return false; }
  }
  void loadPending() {
    std::string bytes;
    if (!readSmall(environment_.pendingPath, 4096, bytes)) return;
    pending_ = parseHostUpdatePending(bytes, environment_.stageRoot);
    // An unreadable record cannot be matched to a request; drop only that file.
    if (!pending_) { std::error_code error; std::filesystem::remove(environment_.pendingPath, error); return; }
    refreshPending(true);
  }
  void refreshPending(bool force) {
    if (!pending_) return;
    if (!force && report_.requestId == pending_->requestId && report_.state != "installing") return;
    std::string bytes; std::optional<HostUpdateResult> result;
    if (readSmall(pending_->stage / L"result.json", 1024, bytes)) result = parseHostUpdateResult(bytes);
    report_ = hostUpdateFinalReport(*pending_, result, environment_.runningVersion, environment_.now() - pending_->startedAt > environment_.resultTimeoutMs);
    // Read only for a finished update (a few times per update, never per poll of a running one).
    if (report_.state == "completed" && report_.code.empty() && environment_.displayMissing) {
      bool missing = false; try { missing = environment_.displayMissing(); } catch (...) { }
      report_.code = hostUpdateCompletionCode(report_.state, report_.code, missing);
    }
  }
  void completePending() {
    if (!pending_) return;
    std::error_code error; std::filesystem::remove(environment_.pendingPath, error);
    // Only the exact files this request produced; never a recursive removal.
    // The protected TEMP folder goes only when the installer left it empty.
    cleanup_ ={pending_->stage / L"result.json", pending_->stage / std::filesystem::path(L"Multitap-Host-" + wide(pending_->targetVersion) + L"-windows-x64.exe"),
      hostUpdateTemporaryDirectory(pending_->stage), pending_->stage};
    cleanupAttempts_ = 0; pending_.reset(); report_ = {}; finalSent_ = false; helloCarriedFinal_ = false;
    retryCleanup();
  }
  void retryCleanup() {
    if (cleanup_.empty() || cleanupAttempts_ >= 30) return;
    ++cleanupAttempts_;
    for (auto at = cleanup_.begin(); at != cleanup_.end();) {
      std::error_code error; const bool directory = std::filesystem::is_directory(*at, error);
      const bool gone = !std::filesystem::exists(*at, error) || (directory ? RemoveDirectoryW(at->c_str()) != FALSE : DeleteFileW(at->c_str()) != FALSE);
      at = gone ? cleanup_.erase(at) : at + 1;
    }
  }
  bool writePending(const HostUpdatePending& pending) {
    try {
      const auto temporary = environment_.pendingPath.wstring() + L".new";
      const auto bytes = hostUpdatePendingJson(pending);
      { HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0; const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
        CloseHandle(file); if (!ok) { DeleteFileW(temporary.c_str()); return false; } }
      return MoveFileExW(temporary.c_str(), environment_.pendingPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) { return false; }
  }
  void run(const HostUpdateRequest& request) {
    std::filesystem::path stage, installer; bool keepStage = false;
    // Declared before every file handle below, so it runs after they close.
    struct Discard {
      std::filesystem::path& stage; std::filesystem::path& installer; bool& keep;
      ~Discard() { if (keep) return; if (!installer.empty()) DeleteFileW(installer.c_str()); if (!stage.empty()) RemoveDirectoryW(stage.c_str()); }
    };
    try {
      Discard discard{stage, installer, keepStage};
      try { stage = environment_.createStage(environment_.stageRoot); } catch (...) { throw HostUpdateFailure("stage_failed"); }
      if (!hostUpdateStageName(stage, environment_.stageRoot)) throw HostUpdateFailure("stage_failed");
      installer = stage / wide(request.filename);
      HANDLE file = CreateFileW(installer.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file == INVALID_HANDLE_VALUE) { installer.clear(); throw HostUpdateFailure("stage_failed"); }
      struct Closer { HANDLE& handle; ~Closer() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } closer{file};
      HostUpdateVerifier verifier(request.size, request.sha256);
      {
        std::unique_ptr<HostUpdateDownload> download;
        try { download = environment_.open(environment_.origin, wide(request.path)); } catch (...) { throw HostUpdateFailure("download_failed"); }
        if (!download || download->status() != 200) throw HostUpdateFailure("download_failed");
        if (download->contentLength() >= 0 && uint64_t(download->contentLength()) != request.size) throw HostUpdateFailure("download_size_mismatch");
        status(request.requestId, "downloading", "", std::pair{uint64_t(0), request.size});
        std::vector<uint8_t> buffer(64 * 1024); uint64_t reported = 0; auto last = GetTickCount64();
        for (;;) {
          if (stopping_) return;  // Service stopping: discard; the broker reports silence.
          size_t count = 0;
          try { count = download->read(buffer.data(), buffer.size()); } catch (...) { throw HostUpdateFailure("download_failed"); }
          if (!count) break;
          verifier.add(buffer.data(), count);
          DWORD written = 0;
          if (!WriteFile(file, buffer.data(), DWORD(count), &written, nullptr) || written != count) throw HostUpdateFailure("stage_failed");
          const auto received = verifier.received();
          if (received == request.size || (GetTickCount64() - last >= 1000 && received - reported >= request.size / 50)) { status(request.requestId, "downloading", "", std::pair{received, request.size}); reported = received; last = GetTickCount64(); }
        }
      }
      status(request.requestId, "verifying", "");
      verifier.finish();
      if (!FlushFileBuffers(file)) throw HostUpdateFailure("stage_failed");
      // Reopen read-only and keep denying writers until the installer starts.
      CloseHandle(file); file = INVALID_HANDLE_VALUE;
      HANDLE held = CreateFileW(installer.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (held == INVALID_HANDLE_VALUE) throw HostUpdateFailure("stage_failed");
      struct Holder { HANDLE handle; ~Holder() { CloseHandle(handle); } } holder{held};
      LARGE_INTEGER length{}; if (!GetFileSizeEx(held, &length) || uint64_t(length.QuadPart) != request.size) throw HostUpdateFailure("download_size_mismatch");
      if (stopping_) return;
      HostUpdatePending pending{request.requestId, environment_.runningVersion, request.version, stage, environment_.now()};
      if (!writePending(pending)) throw HostUpdateFailure("stage_failed");
      { std::lock_guard lock(mutex_); pending_ = pending; report_ = hostUpdateFinalReport(pending, std::nullopt, environment_.runningVersion, false); finalSent_ = false; lastPoll_ = environment_.now(); }
      status(request.requestId, "installing", "");
      if (!environment_.launch(installer, stage, request.allowDowngrade)) {
        { std::lock_guard lock(mutex_); pending_.reset(); report_ = {}; }
        std::error_code error; std::filesystem::remove(environment_.pendingPath, error);
        throw HostUpdateFailure("launch_failed");
      }
      keepStage = true;
    } catch (const HostUpdateFailure& failure) {
      status(request.requestId, "failed", hostUpdateCode(failure.what()) ? failure.what() : "install_failed");
    } catch (...) {
      status(request.requestId, "failed", "stage_failed");
    }
  }
};
}
