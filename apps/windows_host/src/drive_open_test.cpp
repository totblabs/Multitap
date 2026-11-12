// Reuse the existing fake SDK/pipe types; NEVER invoke its legacy main/sections.
// This entry calls only pure parsing and an injected WinFsp memory/helper path.
#define main driveOpenLegacyFixtureMainNotInvoked
#include "drive_winfsp_test.cpp"
#undef main

namespace {
decltype(fs::Api::create) driveOpenOriginalCreate = nullptr;
void openRulesOnly() {
  uint32_t id = 0; std::string share;
  CHECK(openRequest(JsonParser::parse(R"({"type":"drive.open","id":1,"share":"docs"})"), id, share));
  CHECK(id == 1 && share == "docs");
  for (const auto* text : {
      R"({"type":"drive.open","id":0,"share":"docs"})",
      R"({"type":"drive.open","id":1.5,"share":"docs"})",
      R"({"type":"drive.open","id":4294967296,"share":"docs"})",
      R"({"type":"drive.open","id":1,"share":"../docs"})",
      R"({"type":"drive.open","id":1,"share":"docs","path":"C:\\Windows"})",
      R"({"type":"drive.open","id":1,"share":"docs","command":"explorer.exe"})"}) {
    CHECK(!openRequest(JsonParser::parse(text), id, share));
  }
  CHECK(ownedTargetMatches(L"\\Device\\Owned", L"\\Device\\Owned"));
  CHECK(!ownedTargetMatches(L"\\Device\\Owned", L"\\Device\\Foreign"));
  CHECK(!ownedTargetMatches(L"", L""));
  CHECK(JsonParser::parse(openResponse(3, "docs", "")).get("ok").boolean());
  CHECK(JsonParser::parse(openResponse(3, "docs", "arbitrary text")).get("error").string() == "drive_open_failed");
}
void openMemoryOnly() {
  Mem pc; FakeFsp fsp; auto api = fsp.api(); Link2 link;
  driveOpenOriginalCreate = api.create;
  api.create = [](PWSTR d, const FSP_FSCTL_VOLUME_PARAMS* p, const FSP_FILE_SYSTEM_INTERFACE* t, FSP_FILE_SYSTEM** out) -> NTSTATUS {
    const auto result = driveOpenOriginalCreate(d, p, t, out);
    if (NT_SUCCESS(result)) wcscpy_s((*out)->VolumeName, L"\\Device\\SyntheticDriveOpen");
    return result;
  };
  std::atomic<unsigned> opens{0}; std::atomic<bool> current{true}, held{false};
  std::promise<void> entered, release; auto released = release.get_future().share();
  fs::Helper helper(api, {
    [&](uint8_t* b, size_t n) { return link.toHelper.read(b, n); },
    [&](const std::vector<uint8_t>& bytes) { link.toService.write(bytes.data(), bytes.size()); return true; }
  }, L"S-1-5-21-1-2-3-1001", {},
    [&](const std::wstring& root) {
      CHECK(root == L"Z:\\"); ++opens;
      if (held) { entered.set_value(); released.wait(); }
      return std::string(); // ONLY this injected callback; never COM/Explorer.
    }, [&](const std::wstring& root, const std::wstring& volume) {
      return current && root == L"Z:\\" && volume == L"\\Device\\SyntheticDriveOpen";
    });
  std::thread helperThread([&] { helper.run(); link.toService.close(); });
  fs::FsBridge bridge(pc,
    [&](uint8_t* b, size_t n) { return link.toService.read(b, n); },
    [&](const std::vector<uint8_t>& bytes) { link.toHelper.write(bytes.data(), bytes.size()); return true; },
    [&] { link.toHelper.close(); });
  CHECK(bridge.waitReady(std::chrono::seconds(2)).empty());
  const std::string token = std::string(sharePrefix) + std::string(43, 'a');
  CHECK(bridge.mount({token, "docs", "Synthetic", false}, std::chrono::seconds(2)).letter == "Z");
  CHECK(bridge.openRoot(token).empty()); CHECK(opens == 1);
  current = false;
  CHECK(bridge.openRoot(token) == "drive_open_unavailable"); CHECK(opens == 1);
  current = true;
  CHECK(bridge.unmount(token, std::chrono::seconds(2)));
  CHECK(bridge.openRoot(token) == "drive_share_unknown"); CHECK(opens == 1);
  CHECK(bridge.mount({token, "docs", "Synthetic", false}, std::chrono::seconds(2)).letter == "Z");
  held = true;
  auto result = std::async(std::launch::async, [&] { return bridge.openRoot(token); });
  CHECK(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(bridge.openRoot(token) == "drive_busy"); // actual callback still owns slot
  bridge.cancelOpen();
  CHECK(result.get() == "drive_open_unknown");
  CHECK(bridge.openRoot(token) == "drive_share_unknown"); // ended observer cannot re-admit
  release.set_value();
  bridge.signalStop(); helperThread.join(); bridge.stop();
  CHECK(opens == 2); CHECK(fsp.deleted == fsp.created);
}
}
int main() {
  openRulesOnly(); openMemoryOnly();
  std::cout << "drive open memory checks: " << checks << ", failures: " << failures << "\n";
  return failures ? 1 : 0;
}
