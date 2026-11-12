// Remote Host update (host-update-v1): request parsing, decisions, verified
// download into a stage, pending/result records and the reporting state
// machine. Fixture only: a fake transport and launcher, a private temporary
// directory, no ProgramData, no network, no installer and no Windows change.
#include "host_update.hpp"
#include <iostream>

namespace {
int failures = 0, checks = 0;
void require(bool value, const char* name) { ++checks; if (!value) { ++failures; std::cout << "FAILED: " << name << '\n'; } }

std::string digestOf(const std::vector<uint8_t>& bytes) {
  multitap::update_detail::Hash hash; std::vector<uint8_t> copy(bytes);
  for (size_t at = 0; at < copy.size(); at += 65536) hash.Add(copy.data() + at, DWORD(std::min<size_t>(65536, copy.size() - at)));
  return hash.Finish();
}
// A throwaway fixture key (never the project release key) and signatures made
// by tools/windows-release-signature.mjs (Node's Ed25519) with its private half.
const char* const fixtureKeyHex = "8affa5a3bf4efe125f5ff67c54f5c76012e0b06b42018b7f1f06f07a539d4006";
// host, 0.2.60, Multitap-Host-0.2.60-windows-x64.exe, 200003 bytes, the fixture payload's SHA-256.
const std::string fixturePayloadSha = "a4f8db94fd962beb20bb6f1f63a4b69c4acbd4f589fe4be5109b221ff0894903";
const std::string fixtureSignature = "Oa7cwEsm4w4c7xcS7aY9eJdZ9/25NFY2P8fWUmzn20mmHQ8VtKI16tZzB+sk7HfRYCyNdEiYN7zkHupTwpteBw==";
multitap::release_signature::Key fixtureKey() {
  multitap::release_signature::Key key{};
  for (size_t i = 0; i < key.size(); ++i) key[i] = uint8_t(std::stoul(std::string(fixtureKeyHex + 2 * i, 2), nullptr, 16));
  return key;
}
std::string requestJson(const std::string& id, const std::string& version, uint64_t size, const std::string& sha, bool downgrade = false, bool disconnect = false, const std::string& extra = "") {
  const auto file = "Multitap-Host-" + version + "-windows-x64.exe";
  return "{\"type\":\"host.update.start\",\"requestId\":\"" + id + "\",\"version\":\"" + version + "\",\"filename\":\"" + file + "\",\"size\":" + std::to_string(size) +
    ",\"sha256\":\"" + sha + "\",\"path\":\"/downloads/" + file + "\",\"allowDowngrade\":" + (downgrade ? "true" : "false") + ",\"disconnectSessions\":" + (disconnect ? "true" : "false") + extra + '}';
}
// The fixture release as a signing broker sends it.
std::string signedRequest(const std::string& id, bool disconnect = false, const std::string& signature = fixtureSignature) {
  return requestJson(id, "0.2.60", 200003, fixturePayloadSha, false, disconnect, ",\"signature\":\"" + signature + "\"");
}
host::Json parse(const std::string& text) { return host::JsonParser::parse(text); }

struct FakeDownload final : host::HostUpdateDownload {
  DWORD code = 200; int64_t length = -1; std::vector<uint8_t> bytes; size_t at = 0;
  DWORD status() override { return code; }
  int64_t contentLength() override { return length; }
  size_t read(uint8_t* buffer, size_t size) override { const size_t count = std::min<size_t>({size, 7001, bytes.size() - at}); if (count) std::memcpy(buffer, bytes.data() + at, count); at += count; return count; }
};

struct Fixture {
  std::filesystem::path root, stageRoot, pendingPath;
  std::mutex mutex; std::vector<std::string> sent;
  std::vector<std::pair<std::wstring, std::wstring>> opened; std::vector<std::filesystem::path> launched; std::vector<bool> launchedDowngrade;
  std::vector<uint8_t> payload; std::string sha; int64_t clock = 1700000000000;
  bool session = false, launchResult = true; DWORD httpStatus = 200; int64_t declared = -2; std::vector<uint8_t> served;
  explicit Fixture(const std::filesystem::path& base) {
    GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id, text, 40);
    root = base / (std::wstring(L"host-update-qa-") + std::wstring(text + 1, text + 37)); stageRoot = root / L"Multitap-Updates"; pendingPath = root / L"host-update.json";
    std::filesystem::create_directories(stageRoot);
    payload.resize(200003); payload[0] = 'M'; payload[1] = 'Z'; for (size_t i = 2; i < payload.size(); ++i) payload[i] = uint8_t((i * 2654435761u) >> 13);
    sha = digestOf(payload); served = payload;
  }
  ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
  host::HostUpdateEnvironment environment(const std::string& running = "0.2.59") {
    host::HostUpdateEnvironment value;
    value.runningVersion = running; value.origin = L"https://broker.fixture.invalid"; value.stageRoot = stageRoot; value.pendingPath = pendingPath;
    value.sessionActive = [this] { return session; };
    value.open = [this](const std::wstring& origin, const std::wstring& path) -> std::unique_ptr<host::HostUpdateDownload> {
      std::lock_guard lock(mutex); opened.emplace_back(origin, path);
      auto download = std::make_unique<FakeDownload>(); download->code = httpStatus; download->bytes = served; download->length = declared == -2 ? int64_t(served.size()) : declared; return download;
    };
    value.createStage = [](const std::filesystem::path& base) { GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id, text, 40); const auto stage = base / text; std::filesystem::create_directory(stage); return stage; };
    value.launch = [this](const std::filesystem::path& installer, const std::filesystem::path&, bool allowDowngrade) { std::lock_guard lock(mutex); launched.push_back(installer); launchedDowngrade.push_back(allowDowngrade); return launchResult; };
    value.trusted = [](const std::filesystem::path&) { return true; };
    value.now = [this] { return clock; };
    value.pollMs = 0; value.releaseKey = fixtureKey();
    return value;
  }
  host::HostUpdater::Send sink() { return [this](const std::string& message) { std::lock_guard lock(mutex); sent.push_back(message); }; }
  std::vector<host::Json> messages() { std::lock_guard lock(mutex); std::vector<host::Json> out; for (const auto& text : sent) out.push_back(parse(text)); return out; }
  std::vector<std::string> states(const std::string& id) { std::vector<std::string> out; for (const auto& value : messages()) if (value.get("requestId").string() == id) out.push_back(value.get("state").string()); return out; }
  bool waitFor(const std::string& id, std::initializer_list<const char*> terminal) {
    // Generous ceiling (60 s) for a loaded release machine; returns on the state.
    for (int i = 0; i < 6000; ++i) { for (const auto& state : states(id)) for (const char* wanted : terminal) if (state == wanted) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return false;
  }
  void clear() { std::lock_guard lock(mutex); sent.clear(); opened.clear(); launched.clear(); launchedDowngrade.clear(); }
  size_t stageCount() { size_t count = 0; for (const auto& entry : std::filesystem::directory_iterator(stageRoot)) { (void)entry; ++count; } return count; }
};
const std::string id1 = "a7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d10", id2 = "b7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d11", id3 = "c7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d12";

void parsingAndDecisions() {
  const auto sha = std::string(64, 'a');
  const auto valid = host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.60", 17000000, sha)));
  require(valid && valid->size == 17000000 && valid->path == "/downloads/Multitap-Host-0.2.60-windows-x64.exe" && !valid->allowDowngrade, "valid request");
  for (const auto& bad : {
         requestJson(id1, "0.2.60", 17000000, sha, false, false, ",\"url\":\"https://evil.invalid/x.exe\""),
         requestJson(id1, "0.2.60", 17000000, sha, false, false, ",\"origin\":\"https://evil.invalid\""),
         requestJson("not-a-uuid", "0.2.60", 17000000, sha), requestJson(id1, "0.2", 17000000, sha), requestJson(id1, "0.2.60", 0, sha),
         requestJson(id1, "0.2.60", host::hostUpdateMaximumSize + 1, sha), requestJson(id1, "0.2.60", 17000000, std::string(64, 'A')), requestJson(id1, "0.2.60", 17000000, std::string(63, 'a')),
         std::string("{\"type\":\"host.update.start\",\"requestId\":\"") + id1 + "\",\"version\":\"0.2.60\",\"filename\":\"Multitap-Host-0.2.60-windows-x64.exe\",\"size\":17000000.5,\"sha256\":\"" + sha + "\",\"path\":\"/downloads/Multitap-Host-0.2.60-windows-x64.exe\",\"allowDowngrade\":false,\"disconnectSessions\":false}",
         std::string("{\"type\":\"host.update.start\",\"requestId\":\"") + id1 + "\",\"version\":\"0.2.60\",\"filename\":\"Multitap-Host-0.2.61-windows-x64.exe\",\"size\":1,\"sha256\":\"" + sha + "\",\"path\":\"/downloads/Multitap-Host-0.2.61-windows-x64.exe\",\"allowDowngrade\":false,\"disconnectSessions\":false}",
         std::string("{\"type\":\"host.update.start\",\"requestId\":\"") + id1 + "\",\"version\":\"0.2.60\",\"filename\":\"Multitap-Host-0.2.60-windows-x64.exe\",\"size\":1,\"sha256\":\"" + sha + "\",\"path\":\"//evil.invalid/downloads/Multitap-Host-0.2.60-windows-x64.exe\",\"allowDowngrade\":false,\"disconnectSessions\":false}",
         std::string("{\"type\":\"host.update.start\",\"requestId\":\"") + id1 + "\",\"version\":\"0.2.60\",\"filename\":\"Multitap-Host-0.2.60-windows-x64.exe\",\"size\":1,\"sha256\":\"" + sha + "\",\"path\":\"/downloads/../Multitap-Host-0.2.60-windows-x64.exe\",\"allowDowngrade\":false,\"disconnectSessions\":false}",
         std::string("{\"type\":\"host.update.start\",\"requestId\":\"") + id1 + "\",\"version\":\"0.2.60\",\"filename\":\"Multitap-Host-0.2.60-windows-x64.exe\",\"size\":1,\"sha256\":\"" + sha + "\",\"path\":\"/downloads/Multitap-Host-0.2.60-windows-x64.exe\",\"allowDowngrade\":\"no\",\"disconnectSessions\":false}"})
    require(!host::parseHostUpdateRequest(parse(bad)), "malformed or redirected request rejected");
  require(host::compareHostVersions("0.2.60", "0.2.59") > 0 && host::compareHostVersions("0.2.9", "0.2.10") < 0 && host::compareHostVersions("1.0.0", "0.99.99") > 0 && host::compareHostVersions("0.2.59", "0.2.59") == 0, "numeric version order");
  const auto older = host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.58", 10, sha))), olderAllowed = host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.58", 10, sha, true)));
  const auto same = host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.59", 10, sha, true))), disconnect = host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.60", 10, sha, false, true)));
  const auto decision = [](const std::optional<host::HostUpdateRequest>& request, bool busy, bool session) { const auto value = host::decideHostUpdate(request, "0.2.59", busy, session); return value.state + ':' + value.code; };
  require(decision(valid, false, false) == "accepted:", "newer accepted");
  require(decision(older, false, false) == "rejected:not_newer", "older refused without flag");
  require(decision(olderAllowed, false, false) == "accepted:", "older only with explicit flag");
  require(decision(same, false, false) == "rejected:same_version", "same version never reinstalled");
  require(decision(valid, true, false) == "rejected:busy", "one update at a time");
  require(decision(valid, false, true) == "deferred:session_active", "session defers");
  require(decision(disconnect, false, true) == "accepted:", "explicit disconnect proceeds");
  require(decision(std::nullopt, false, false) == "rejected:invalid_request", "invalid request");
}

void verifier() {
  std::vector<uint8_t> bytes{'M', 'Z', 1, 2, 3};
  const auto sha = digestOf(bytes);
  const auto run = [&](const std::vector<uint8_t>& data, uint64_t size, const std::string& digest) -> std::string {
    try { host::HostUpdateVerifier verify(size, digest); for (size_t i = 0; i < data.size(); i += 2) verify.add(data.data() + i, std::min<size_t>(2, data.size() - i)); verify.finish(); return "ok"; }
    catch (const host::HostUpdateFailure& failure) { return failure.what(); }
  };
  require(run(bytes, 5, sha) == "ok", "exact bytes verify");
  require(run(bytes, 4, sha) == "download_size_mismatch", "oversized stream stops before writing more");
  require(run(bytes, 6, sha) == "download_size_mismatch", "short stream fails");
  require(run(bytes, 5, std::string(64, '0')) == "hash_mismatch", "hash mismatch");
  std::vector<uint8_t> text{'P', 'K', 1, 2, 3};
  require(run(text, 5, digestOf(text)) == "not_executable", "non-executable rejected even with matching hash");
}

// multitap-windows-release-signature-v1: canonical message, strict encoding,
// RFC 8032 vectors, the fixture signature and every tampered field.
void signatures() {
  namespace rs = multitap::release_signature;
  const auto hex = [](const std::string& text) { std::vector<uint8_t> out; for (size_t i = 0; i < text.size(); i += 2) out.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16))); return out; };
  const auto key = [&](const std::string& text) { rs::Key out{}; const auto bytes = hex(text); std::copy(bytes.begin(), bytes.end(), out.begin()); return out; };
  const auto signature = [&](const std::string& text) { rs::Signature out{}; const auto bytes = hex(text); std::copy(bytes.begin(), bytes.end(), out.begin()); return out; };
  // RFC 8032 section 7.1, TEST 1 (empty message) and TEST 2 (0x72).
  const auto rfc1 = key("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  const auto sig1 = signature("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  const auto rfc2 = key("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
  const auto sig2 = signature("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
  require(rs::VerifyEd25519("", sig1, rfc1) && rs::VerifyEd25519("\x72", sig2, rfc2), "RFC 8032 vectors verify");
  require(!rs::VerifyEd25519("\x73", sig2, rfc2) && !rs::VerifyEd25519("", sig1, rfc2), "RFC 8032 message/key changes fail");
  auto flipped = sig1; flipped[5] ^= 1; auto highS = sig1; highS[63] |= 0xf0;
  require(!rs::VerifyEd25519("", flipped, rfc1) && !rs::VerifyEd25519("", highS, rfc1), "altered R or S >= L fails");
  // The exact signed bytes.
  const auto message = rs::Message("host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 200003, fixturePayloadSha);
  require(message && *message == "multitap-windows-release-signature-v1\nproduct=host\nversion=0.2.60\nfilename=Multitap-Host-0.2.60-windows-x64.exe\nsize=200003\nsha256=" + fixturePayloadSha + "\n", "canonical message");
  for (const auto& [product, version, filename, size, sha] : std::vector<std::tuple<std::string, std::string, std::string, uint64_t, std::string>>{
         {"server", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 1, fixturePayloadSha}, {"host", "0.2", "Multitap-Host-0.2-windows-x64.exe", 1, fixturePayloadSha},
         {"host", "0.2.60\nsize=1", "Multitap-Host-0.2.60\nsize=1-windows-x64.exe", 1, fixturePayloadSha}, {"host", "0.2.60", "Multitap-0.2.60-windows-x64.exe", 1, fixturePayloadSha},
         {"host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 0, fixturePayloadSha}, {"host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 128ULL * 1024 * 1024 + 1, fixturePayloadSha},
         {"host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 1, std::string(64, 'A')}, {"host", "1234567.2.60", "Multitap-Host-1234567.2.60-windows-x64.exe", 1, fixturePayloadSha}})
    require(!rs::Message(product, version, filename, size, sha), "fields outside the catalog rules are never signed");
  // Strict base64: 86 symbols + "==", zero padding bits.
  rs::Signature decoded{};
  require(rs::DecodeSignature(fixtureSignature, decoded), "canonical signature decodes");
  std::string padded = fixtureSignature; padded[85] = 'B';
  for (const auto& bad : {std::string(), fixtureSignature.substr(0, 87), fixtureSignature + "=", std::string(86, 'A') + "A=", padded, std::string(85, 'A') + "*==", " " + fixtureSignature.substr(1)})
    require(!rs::DecodeSignature(bad, decoded), "non-canonical or malformed signature text rejected");
  const auto fixture = fixtureKey();
  require(rs::VerifyRelease("host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 200003, fixturePayloadSha, fixtureSignature, fixture), "fixture release verifies");
  require(!rs::VerifyRelease("host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 200004, fixturePayloadSha, fixtureSignature, fixture), "tampered size fails");
  require(!rs::VerifyRelease("host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 200003, std::string(64, 'a'), fixtureSignature, fixture), "tampered SHA-256 fails");
  require(!rs::VerifyRelease("host", "0.2.61", "Multitap-Host-0.2.61-windows-x64.exe", 200003, fixturePayloadSha, fixtureSignature, fixture), "tampered version/filename fails");
  require(!rs::VerifyRelease("client", "0.2.60", "Multitap-0.2.60-windows-x64.exe", 200003, fixturePayloadSha, fixtureSignature, fixture), "other product fails");
  require(!rs::VerifyRelease("host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 200003, fixturePayloadSha, "", fixture), "missing signature fails");
  require(!rs::VerifyRelease("host", "0.2.60", "Multitap-Host-0.2.60-windows-x64.exe", 200003, fixturePayloadSha, fixtureSignature), "fixture key is not the pinned release key");
  std::string pinned; for (const auto byte : rs::PinnedKey()) { constexpr char digits[] = "0123456789abcdef"; pinned += digits[byte >> 4]; pinned += digits[byte & 15]; }
  require(pinned == rs::kReleasePublicKeyHex && pinned.size() == 64, "pinned key parses");
  // Requests: an optional string signature, nothing else added.
  const auto withSignature = host::parseHostUpdateRequest(parse(signedRequest(id1)));
  require(withSignature && withSignature->signature == fixtureSignature && host::hostUpdateSignatureValid(*withSignature, fixture), "signed request parses and verifies");
  const auto unsignedRequest = host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.60", 200003, fixturePayloadSha)));
  require(unsignedRequest && unsignedRequest->signature.empty() && !host::hostUpdateSignatureValid(*unsignedRequest, fixture), "unsigned request parses but never verifies");
  require(!host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.60", 200003, fixturePayloadSha, false, false, ",\"signature\":7"))) &&
      !host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.60", 200003, fixturePayloadSha, false, false, ",\"signature\":\"" + fixtureSignature + "\",\"url\":\"x\""))) &&
      !host::parseHostUpdateRequest(parse(requestJson(id1, "0.2.60", 200003, fixturePayloadSha, false, false, ",\"signature\":\"" + std::string(200, 'A') + "\""))), "malformed signature fields rejected");
}

// The installer's child-only environment: TEMP/TMP and explicit result-v2
// receiver capability, sorted/double-NUL ended. No global env mutation.
void installerEnvironment(const std::filesystem::path& base) {
  const auto block = host::hostUpdateEnvironmentBlock(base / L"stage-temp");
  require(block.size() > 2 && block[block.size() - 1] == L'\0' && block[block.size() - 2] == L'\0', "environment block ends with two NULs");
  std::vector<std::wstring> entries; for (size_t at = 0; at < block.size() && block[at]; at += entries.back().size() + 1) entries.emplace_back(block.c_str() + at);
  size_t temp = 0, tmp = 0, resultProtocol = 0; bool sorted = true, path = false;
  const std::wstring protocolPrefix=L"MULTITAP_HOST_UPDATE_RESULT_PROTOCOL=";
  for (size_t i = 0; i < entries.size(); ++i) {
    if (_wcsnicmp(entries[i].c_str(), L"TEMP=", 5) == 0) { ++temp; require(entries[i] == L"TEMP=" + (base / L"stage-temp").wstring(), "TEMP is the stage folder"); }
    if (_wcsnicmp(entries[i].c_str(), L"TMP=", 4) == 0) { ++tmp; require(entries[i] == L"TMP=" + (base / L"stage-temp").wstring(), "TMP is the stage folder"); }
    if (_wcsnicmp(entries[i].c_str(), L"PATH=", 5) == 0) path = true;
    if (_wcsnicmp(entries[i].c_str(),protocolPrefix.c_str(),protocolPrefix.size())==0) { ++resultProtocol;require(entries[i]==protocolPrefix+L"2","child capability is exact result protocol2"); }
    if (i) { const auto a = entries[i - 1].substr(0, entries[i - 1].find(L'=', 1)), b = entries[i].substr(0, entries[i].find(L'=', 1)); if (CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_GREATER_THAN) sorted = false; }
  }
  require(temp == 1 && tmp == 1 && resultProtocol == 1 && path && sorted, "one TEMP/TMP/fixed receiver capability; rest kept and sorted");
  const auto replaced=host::hostUpdateEnvironmentFromEntries(base/L"stage-temp",{
    L"TEMP=old",L"tmp=old",L"MULTITAP_HOST_UPDATE_RESULT_PROTOCOL=1",
    L"multitap_host_update_result_protocol=garbage",L"PATH=synthetic",L"UNCHANGED=synthetic"});
  size_t capabilities=0;bool keptPath=false,keptOther=false;
  for(size_t at=0;at<replaced.size()&&replaced[at];){
    const std::wstring entry(replaced.c_str()+at);at+=entry.size()+1;
    if(_wcsnicmp(entry.c_str(),protocolPrefix.c_str(),protocolPrefix.size())==0){++capabilities;require(entry==protocolPrefix+L"2","stale/case-duplicate opt-in replaced");}
    keptPath|=entry==L"PATH=synthetic";keptOther|=entry==L"UNCHANGED=synthetic";
  }
  require(capabilities==1&&keptPath&&keptOther,"exactly one explicit child opt-in, unrelated environment retained");
  require(replaced.size()>2&&replaced[replaced.size()-1]==L'\0'&&replaced[replaced.size()-2]==L'\0',"synthetic child environment double-NUL terminated");
}

// A root another account created is never trusted: an empty one is removed,
// a non-empty one is moved aside. (Creating the real Administrators-owned
// root needs elevation, so the final creation fails here by design.)
void foreignUpdateRoot(const std::filesystem::path& base) {
  using namespace multitap::update_detail;
  GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id, text, 40);
  const auto parent = base / (std::wstring(L"host-update-root-qa-") + std::wstring(text + 1, text + 37));
  std::filesystem::create_directories(parent);
  const auto root = (parent / L"Multitap-Updates").wstring();
  Descriptor security; ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &security.value, nullptr);
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.value, FALSE};
  const auto attempt = [&] { try { SecureUpdateRoot(root, attributes); return true; } catch (const std::runtime_error&) { return false; } };
  std::filesystem::create_directory(root);
  require(!TrustedDirectoryPath(root), "a user-made root is untrusted");
  const bool emptyResult = attempt();
  require(emptyResult == TrustedDirectoryPath(root) && (emptyResult || !std::filesystem::exists(root)), "an untrusted empty root is removed");
  std::filesystem::remove(root);
  std::filesystem::create_directory(root); { std::ofstream planted(std::filesystem::path(root) / L"planted.exe", std::ios::binary); planted << "MZ"; }
  const bool fullResult = attempt();
  size_t aside = 0; for (const auto& entry : std::filesystem::directory_iterator(parent)) if (entry.path().filename().wstring().rfind(L"Multitap-Updates.untrusted-{", 0) == 0 && std::filesystem::exists(entry.path() / L"planted.exe")) ++aside;
  require(aside == 1 && (fullResult ? TrustedDirectoryPath(root) : !std::filesystem::exists(root)), "an untrusted non-empty root is moved aside, never used");
  std::ofstream(std::filesystem::path(root), std::ios::binary) << "file";
  const bool fileResult = attempt();
  require(fileResult ? TrustedDirectoryPath(root) : !std::filesystem::exists(root), "a file in place of the root is removed");
  std::error_code error; std::filesystem::remove_all(parent, error);
}

void signedUpdates(const std::filesystem::path& base) {
  Fixture f(base);
  require(f.sha == fixturePayloadSha, "fixture payload matches the signed SHA-256");
  {
    host::HostUpdater updater(f.environment(), f.sink());
    const auto hello = parse(updater.hello());
    require(hello.get("releaseSignature").string() == "multitap-windows-release-signature-v1", "hello announces release-signature support");
    // A broker without signature support: refused with a code it knows.
    updater.acknowledged(parse("{\"type\":\"host.update.hello.ack\",\"protocol\":\"host-update-v1\"}"));
    updater.start(parse(requestJson(id1, "0.2.60", 200003, fixturePayloadSha)));
    require(f.states(id1) == std::vector<std::string>{"rejected"} && f.messages().back().get("code").string() == "invalid_request", "unsigned release refused for an older broker");
    // A signing broker: the fixed code, before any download or stage.
    updater.hello(); updater.acknowledged(parse("{\"type\":\"host.update.hello.ack\",\"protocol\":\"host-update-v1\",\"releaseSignature\":\"multitap-windows-release-signature-v1\"}"));
    updater.start(parse(requestJson(id2, "0.2.60", 200003, fixturePayloadSha)));
    require(f.states(id2) == std::vector<std::string>{"rejected"} && f.messages().back().get("code").string() == "signature_invalid", "unsigned release refused with signature_invalid");
    // Signed with another key, or over other metadata: refused the same way.
    std::string other = fixtureSignature; other[10] = other[10] == 'A' ? 'B' : 'A';
    updater.start(parse(signedRequest(id3, false, other)));
    require(f.states(id3) == std::vector<std::string>{"rejected"} && f.messages().back().get("code").string() == "signature_invalid", "mismatching signature refused");
    f.clear();
    updater.start(parse(requestJson(id1, "0.2.60", 200004, fixturePayloadSha, false, false, ",\"signature\":\"" + fixtureSignature + "\"")));
    require(f.states(id1) == std::vector<std::string>{"rejected"} && f.messages().back().get("code").string() == "signature_invalid", "signature over another size refused");
    // An active session never defers an unsigned release for a later retry.
    f.session = true; f.clear();
    updater.start(parse(requestJson(id2, "0.2.60", 200003, fixturePayloadSha)));
    require(f.states(id2) == std::vector<std::string>{"rejected"} && f.messages().back().get("code").string() == "signature_invalid", "unsigned release not deferred");
    require(f.opened.empty() && f.launched.empty() && f.stageCount() == 0 && !std::filesystem::exists(f.pendingPath), "nothing downloaded, staged or launched");
  }
  {
    // The pinned release key never accepts the fixture signature.
    auto environment = f.environment(); environment.releaseKey = multitap::release_signature::PinnedKey();
    f.session = false; f.clear();
    host::HostUpdater updater(std::move(environment), f.sink());
    updater.start(parse(signedRequest(id3)));
    require(f.states(id3) == std::vector<std::string>{"rejected"} && f.opened.empty(), "fixture signature refused under the release key");
  }
}

void endToEnd(const std::filesystem::path& base) {
  Fixture f(base);
  std::filesystem::path stage;
  {
    host::HostUpdater updater(f.environment(), f.sink());
    const auto hello = parse(updater.hello());
    require(hello.get("type").string() == "host.update.hello" && hello.get("protocol").string() == "host-update-v1" && hello.get("version").string() == "0.2.59" && std::holds_alternative<std::nullptr_t>(hello.get("result").value), "hello without pending result");
    updater.acknowledged(parse("{\"type\":\"host.update.hello.ack\",\"protocol\":\"host-update-v1\"}"));
    updater.start(parse(signedRequest(id1)));
    require(f.waitFor(id1, {"installing", "failed"}), "update reached the installer");
    updater.stop();
    const auto states = f.states(id1);
    require(states.size() >= 5 && states.front() == "accepted" && states[1] == "downloading" && states[states.size() - 2] == "verifying" && states.back() == "installing", "accepted, downloading, verifying, installing in order");
    require(f.opened.size() == 1 && f.opened[0].first == L"https://broker.fixture.invalid" && f.opened[0].second == L"/downloads/Multitap-Host-0.2.60-windows-x64.exe", "download only from the configured origin and catalog path");
    require(f.launched.size() == 1 && f.launched[0].filename() == L"Multitap-Host-0.2.60-windows-x64.exe" && std::filesystem::file_size(f.launched[0]) == f.payload.size(), "verified installer launched from its stage");
    require(std::filesystem::exists(f.pendingPath) && updater.pending() && updater.pending()->targetVersion == "0.2.60" && updater.pending()->fromVersion == "0.2.59", "pending record survives the service restart");
    bool progressBounded = true; for (const auto& value : f.messages()) if (value.get("state").string() == "downloading" && (value.get("total").number() != double(f.payload.size()) || value.get("received").number() > double(f.payload.size()))) progressBounded = false;
    require(progressBounded, "progress bounded by the catalog size");
    if (!f.launched.empty()) stage = f.launched[0].parent_path();
    f.clear();
    updater.start(parse(signedRequest(id2)));
    require(f.states(id2) == std::vector<std::string>{"rejected"} && f.messages().back().get("code").string() == "busy", "second request while installing is busy");
  }
  // The installer finished; the restarted (new) service reports the result.
  require(!stage.empty() && stage.parent_path() == f.stageRoot, "stage inside the private fixture root");
  if (stage.empty()) return;
  { std::ofstream result(stage / L"result.json", std::ios::binary); result << "{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"completed\",\"completed\":6,\"rebootRequired\":false,\"attention\":false,\"code\":\"\"}"; }
  {
    f.clear();
    host::HostUpdater restarted(f.environment("0.2.60"), f.sink());
    const auto hello = parse(restarted.hello());
    require(hello.get("result").get("requestId").string() == id1 && hello.get("result").get("state").string() == "completed" && hello.get("result").get("version").string() == "0.2.60", "restarted service reports completion in hello");
    restarted.acknowledged(parse("{\"type\":\"host.update.hello.ack\",\"protocol\":\"host-update-v1\",\"result\":\"" + id1 + "\"}"));
    require(!restarted.pending() && !std::filesystem::exists(f.pendingPath), "acknowledged result clears the pending record");
    require(!std::filesystem::exists(stage), "only the exact staged files and directory were removed");
    require(f.stageCount() == 0, "no stage left behind");
  }
}

void failures_(const std::filesystem::path& base) {
  Fixture f(base);
  const auto attempt = [&](const std::string& id, auto configure, const char* code, const char* name) {
    f.clear(); f.httpStatus = 200; f.declared = -2; f.served = f.payload; f.session = false; f.launchResult = true; configure();
    host::HostUpdater updater(f.environment(), f.sink());
    updater.start(parse(signedRequest(id)));
    require(f.waitFor(id, {"failed", "installing"}), name);
    updater.stop();
    const auto messages = f.messages();
    require(!messages.empty() && messages.back().get("state").string() == "failed" && messages.back().get("code").string() == code, name);
    // The fake launcher records the failed start attempt; nothing else ran.
    require(f.launched.size() == (f.launchResult ? 0u : 1u) && !std::filesystem::exists(f.pendingPath) && f.stageCount() == 0 && !updater.pending(), name);
  };
  attempt(id1, [&] { f.served[100] = uint8_t(f.served[100] ^ 0x5a); }, "hash_mismatch", "tampered bytes never launch");
  attempt(id1, [&] { f.declared = int64_t(f.payload.size()) + 1; }, "download_size_mismatch", "declared size mismatch");
  attempt(id1, [&] { f.declared = -1; f.served.push_back(0); }, "download_size_mismatch", "undeclared oversized stream");
  attempt(id1, [&] { f.declared = -1; f.served.pop_back(); }, "download_size_mismatch", "truncated stream");
  attempt(id1, [&] { f.httpStatus = 404; }, "download_failed", "missing release");
  attempt(id1, [&] { f.launchResult = false; }, "launch_failed", "installer start failure clears the pending record");
  {
    f.clear(); f.served = f.payload; f.session = true; f.launchResult = true; f.httpStatus = 200; f.declared = -2;
    host::HostUpdater updater(f.environment(), f.sink());
    updater.start(parse(signedRequest(id2)));
    require(f.states(id2) == std::vector<std::string>{"deferred"} && f.messages().back().get("code").string() == "session_active" && f.opened.empty(), "active session defers without downloading");
    updater.start(parse(signedRequest(id3, true)));
    require(f.waitFor(id3, {"installing", "failed"}) && f.states(id3).back() == "installing", "explicit disconnect proceeds");
    require(!f.launchedDowngrade.empty() && !f.launchedDowngrade.back(), "an ordinary update never carries the downgrade permission");
    updater.stop();
  }
  {
    Fixture g(base);
    host::HostUpdater updater(g.environment(), g.sink());
    updater.start(parse(requestJson(id1, "0.2.58", g.payload.size(), g.sha)));
    updater.start(parse(requestJson(id2, "0.2.59", g.payload.size(), g.sha, true)));
    updater.start(parse(requestJson(id3, "0.2.60", g.payload.size(), g.sha, false, false, ",\"url\":\"https://evil.invalid\"")));
    updater.start(parse("{\"type\":\"host.update.start\",\"requestId\":\"nope\"}"));
    const auto messages = g.messages();
    require(messages.size() == 3 && messages[0].get("code").string() == "not_newer" && messages[1].get("code").string() == "same_version" && messages[2].get("code").string() == "invalid_request" && g.opened.empty(), "refusals are reported, uncorrelatable input is ignored");
    bool clean = true; for (const auto& text : g.sent) if (text.find("evil") != std::string::npos) clean = false;
    require(clean, "nothing from a request is echoed");
  }
}

void reporting(const std::filesystem::path& base) {
  Fixture f(base);
  std::filesystem::path stage;
  {
    host::HostUpdater updater(f.environment(), f.sink());
    updater.start(parse(signedRequest(id1)));
    require(f.waitFor(id1, {"installing"}), "installing");
    updater.stop(); stage = f.launched.at(0).parent_path();
  }
  // Preflight refusal: the OLD service is still running and sees the result.
  { std::ofstream result(stage / L"result.json", std::ios::binary); result << "{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"failed\",\"completed\":0,\"rebootRequired\":false,\"attention\":false,\"code\":\"driver_install_required\"}"; }
  {
    f.clear();
    host::HostUpdater updater(f.environment(), f.sink());
    updater.poll();
    require(f.sent.empty(), "no status before the broker announced support");
    const auto hello = parse(updater.hello());
    require(hello.get("result").get("state").string() == "failed" && hello.get("result").get("code").string() == "driver_install_required", "manual decision reported as a fixed code");
    updater.acknowledged(parse("{\"type\":\"host.update.hello.ack\",\"protocol\":\"host-update-v1\"}"));
    require(updater.pending().has_value(), "a hello ack without the result id keeps the record");
    updater.poll();
    const auto messages = f.messages();
    require(!messages.empty() && messages.back().get("type").string() == "host.update.status" && messages.back().get("state").string() == "failed" && messages.back().get("requestId").string() == id1, "final status sent once the broker supports the protocol");
    updater.acknowledged(parse("{\"type\":\"host.update.status.ack\",\"requestId\":\"" + id1 + "\"}"));
    require(!updater.pending() && !std::filesystem::exists(f.pendingPath), "status ack clears the record");
  }
  // No result at all: only the observed version decides, after the deadline.
  {
    f.clear();
    host::HostUpdater updater(f.environment(), f.sink());
    updater.start(parse(signedRequest(id2)));
    require(f.waitFor(id2, {"installing"}), "second installing");
    updater.stop();
  }
  for (const auto& [running, state, code] : {std::tuple{std::string("0.2.60"), std::string("completed"), std::string("result_unreported")}, std::tuple{std::string("0.2.59"), std::string("failed"), std::string("install_result_missing")}}) {
    f.clear();
    host::HostUpdater updater(f.environment(running), f.sink());
    require(parse(updater.hello()).get("result").get("state").string() == "installing", "pending without result is still installing");
    updater.acknowledged(parse("{\"type\":\"host.update.hello.ack\",\"protocol\":\"host-update-v1\"}"));
    updater.poll(); require(f.sent.empty(), "no verdict before the deadline");
    f.clock += 16 * 60 * 1000; updater.poll(); f.clock -= 16 * 60 * 1000;
    const auto messages = f.messages();
    require(!messages.empty() && messages.back().get("state").string() == state && messages.back().get("code").string() == code, "timeout verdict from the running version");
  }
  // A finished unattended update on a Host that has no virtual display says so
  // with the existing display_attention code; it never installs or trusts one.
  require(host::hostUpdateCompletionCode("completed", "", true) == "display_attention" && host::hostUpdateCompletionCode("completed", "", false).empty() &&
          host::hostUpdateCompletionCode("completed", "result_unreported", true) == "result_unreported" && host::hostUpdateCompletionCode("failed", "", true).empty() &&
          host::hostUpdateCode("display_attention"), "only a clean completion gains the display notice");
  for (const bool missing : {true, false}) {
    f.clear();
    auto environment = f.environment("0.2.60"); int reads = 0; environment.displayMissing = [&reads, missing] { ++reads; return missing; };
    host::HostUpdater updater(std::move(environment), f.sink());
    const auto waiting = updater.pending();
    // (The second pass starts with the first pass's result already in the stage.)
    require(waiting.has_value() && (!missing || reads == 0), "an update that is still installing does not read the display");
    if (waiting) { std::ofstream result(waiting->stage / L"result.json", std::ios::binary); result << "{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"completed\",\"completed\":6,\"rebootRequired\":false,\"attention\":false,\"code\":\"\"}"; }
    const auto hello = parse(updater.hello());
    require(hello.get("result").get("state").string() == "completed" && hello.get("result").get("code").string() == (missing ? "display_attention" : "") && reads > 0, "a Host without a virtual display reports it after an unattended update");
  }
  // A completed result read by a service that is not the target version is not success.
  host::HostUpdatePending pending{id1, "0.2.59", "0.2.60", f.stageRoot / L"{00000000-0000-0000-0000-000000000000}", 0};
  const auto completed = host::parseHostUpdateResult("{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"completed\",\"completed\":6,\"rebootRequired\":true,\"attention\":false,\"code\":\"\"}");
  require(completed && completed->rebootRequired, "result parse");
  require(host::hostUpdateFinalReport(pending, completed, "0.2.59", false).state == "failed", "completion requires the running target version");
  const auto restart = host::hostUpdateFinalReport(pending, completed, "0.2.60", false);
  require(restart.state == "completed" && restart.rebootRequired, "restart requirement travels with completion");
  for (const auto* bad : {"{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"completed\",\"completed\":6,\"rebootRequired\":false,\"attention\":false,\"code\":\"driver_install_required\"}",
                          "{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"failed\",\"completed\":2,\"rebootRequired\":false,\"attention\":false,\"code\":\"\"}",
                          "{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"failed\",\"completed\":2,\"rebootRequired\":false,\"attention\":false,\"code\":\"C:\\\\secret\"}",
                          "{\"schema\":1,\"product\":\"client\",\"version\":\"0.2.60\",\"outcome\":\"completed\",\"completed\":6,\"rebootRequired\":false,\"attention\":false,\"code\":\"\"}",
                          "{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"completed\",\"completed\":6,\"rebootRequired\":false,\"attention\":false,\"code\":\"\",\"extra\":1}"})
    require(!host::parseHostUpdateResult(bad), "untrusted result shape rejected");
  // These are the existing broker/service diagnostic vocabulary, not local
  // installer handoff strings. A failed preflight stays failed and keeps its
  // fixed cause even if a different running version reports it.
  for(const auto* code:{"not_newer","stage_failed","launch_failed"}){
    const std::string bytes=std::string("{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"failed\",\"completed\":0,\"rebootRequired\":false,\"attention\":false,\"code\":\"")+code+"\"}";
    const auto failure=host::parseHostUpdateResult(bytes);
    require(failure&&failure->code==code&&!failure->completed,"fixed preflight cause parsed without promotion");
    const auto report=host::hostUpdateFinalReport(pending,failure,"0.2.59",false);
    require(report.state=="failed"&&report.code==code&&report.requestId==pending.requestId,"fixed cause reaches the same operation report");
    require(host::hostUpdateCode(code),"fixed cause already accepted by broker vocabulary");
  }
  for(const auto* code:{"installer_missing","progress_channel_failed","installer_start_failed","update_not_newer"}){
    const std::string bytes=std::string("{\"schema\":1,\"product\":\"host\",\"version\":\"0.2.60\",\"outcome\":\"failed\",\"completed\":0,\"rebootRequired\":false,\"attention\":false,\"code\":\"")+code+"\"}";
    require(!host::parseHostUpdateResult(bytes),"local handoff strings require service vocabulary normalization");
  }
  // A tampered pending record pointing outside the stage root is discarded.
  { std::ofstream record(f.pendingPath, std::ios::binary); record << host::hostUpdatePendingJson({id3, "0.2.59", "0.2.60", L"C:\\Windows\\{00000000-0000-0000-0000-000000000000}", 0}); }
  { host::HostUpdater updater(f.environment(), f.sink()); require(!updater.pending() && !std::filesystem::exists(f.pendingPath), "foreign stage path rejected"); }
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t temporary[MAX_PATH + 1]{}; GetTempPathW(MAX_PATH, temporary);
    const std::filesystem::path base = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(temporary);
    std::filesystem::create_directories(base);
    parsingAndDecisions(); verifier(); signatures(); installerEnvironment(base); foreignUpdateRoot(base); signedUpdates(base); endToEnd(base); failures_(base); reporting(base);
    if (SUCCEEDED(initialized)) CoUninitialize();
  } catch (const std::exception& error) { std::cout << "FAILED: exception " << error.what() << '\n'; ++failures; }
  std::cout << "{\"kind\":\"host-remote-update\",\"checks\":" << checks << ",\"failures\":" << failures << ",\"releaseSignature\":true,\"network\":false,\"installerExecuted\":false,\"programDataChanged\":false}\n";
  return failures ? 1 : 0;
}
