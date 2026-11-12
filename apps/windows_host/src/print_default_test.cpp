// print-v1 default printer (print_default.hpp): save / set / restore, skip when
// the user changed the default, crash recovery, and the link-hardened record
// file, all with a substituted printer API. No printer or setting of this
// machine is read or changed.
#include <winsock2.h>
#include "print_redirection.hpp"
#include <winioctl.h>
#include <cstring>
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::wstring ours = printQueueName;

struct FakeUser {
  std::wstring def = L"HP LaserJet"; std::set<std::wstring> printers{L"HP LaserJet", L"Office", ours}; std::string record; bool setFails = false, loadFails = false, saveFails = false; unsigned sets = 0, error = 0;
  std::map<std::wstring, PrintQueueBinding> bindings{{ours, {L"Multitap_Print_50000", printDriverName}}, {printLegacyQueueName, {L"Multitap_Print_50001", printDriverName}}};
  DefaultPrinterAccess access() {
    DefaultPrinterAccess a;
    a.current = [this] { return def; };
    a.set = [this](const std::wstring& name) { ++sets; if (setFails || !printers.count(name)) return false; def = name; return true; };
    a.exists = [this](const std::wstring& name) { return printers.count(name) != 0; };
    a.binding = [this](const std::wstring& name) -> std::optional<PrintQueueBinding> {
      const auto found = bindings.find(name);
      return printers.count(name) && found != bindings.end() ? std::optional<PrintQueueBinding>(found->second) : std::nullopt;
    };
    a.lastError = [this] { return error; };
    a.load = [this]() -> std::optional<std::string> { if (loadFails) return std::nullopt; return record; };
    a.save = [this](const std::string& text) { if (saveFails) return false; record = text; return true; };
    return a;
  }
};
using R = DefaultPrinterResult;

void saveSetRestore() {
  FakeUser user; DefaultPrinterKeeper keeper(user.access());
  require(keeper.claim() == R::claimed && user.def == ours && !user.record.empty(), "The queue did not become the default with a record");
  require(user.record.find("HP LaserJet") != std::string::npos, "The previous default was not saved");
  const auto saved = JsonParser::parse(user.record);
  require(saved.get("v").number() == 2 && saved.get("queue").string() == utf8(ours) && saved.get("port").string() == "Multitap_Print_50000", "The new record did not bind the exact claimed queue and port");
  require(keeper.release() == R::restored && user.def == L"HP LaserJet" && user.record.empty(), "The saved default did not return, or the record stayed");
  require(keeper.release() == R::none && user.def == L"HP LaserJet", "A second release changed something");
  // No previous default: the queue is default, and release removes the record without inventing one.
  FakeUser none; none.def.clear(); DefaultPrinterKeeper second(none.access());
  require(second.claim() == R::claimed && none.def == ours, "Claim without a previous default failed");
  require(second.release() == R::kept && none.record.empty(), "Release without a previous default did not clear the record");
  // Korean names round-trip through the record.
  FakeUser korean; korean.def = L"사무실 프린터"; korean.printers.insert(korean.def); DefaultPrinterKeeper third(korean.access());
  require(third.claim() == R::claimed && third.release() == R::restored && korean.def == L"사무실 프린터", "A Korean printer name did not round-trip");
}

void userChoice() {
  FakeUser user; DefaultPrinterKeeper keeper(user.access());
  require(keeper.claim() == R::claimed, "Claim");
  user.def = L"Office";  // the user picks another default during the session
  const auto sets = user.sets;
  require(keeper.release() == R::kept && user.def == L"Office" && user.sets == sets && user.record.empty(), "The user's own default was overwritten");
  // The saved printer was removed meanwhile: nothing is invented.
  FakeUser gone; DefaultPrinterKeeper second(gone.access());
  require(second.claim() == R::claimed, "Claim 2");
  gone.printers.erase(L"HP LaserJet");
  require(second.release() == R::kept && gone.def == ours && gone.record.empty(), "A removed printer was set as default");
}

void crashRecovery() {
  // A stale default name with its queue gone is not current ownership proof.
  FakeUser user; { DefaultPrinterKeeper keeper(user.access()); require(keeper.claim() == R::claimed, "Claim"); }
  user.printers.erase(ours);
  DefaultPrinterKeeper next(user.access());
  require(next.recover() == R::kept && user.def == ours && user.record.empty(), "A missing queue was treated as a current owned default");
  require(next.recover() == R::none, "Recovery without a record did something");
  // Crash, queue still there and the user chose another default: left alone.
  FakeUser chosen; { DefaultPrinterKeeper keeper(chosen.access()); (void)keeper.claim(); }
  chosen.def = L"Office";
  require(DefaultPrinterKeeper(chosen.access()).recover() == R::kept && chosen.def == L"Office" && chosen.record.empty(), "Recovery overwrote the user's default");
  // Crash, queue still there and still the default: restored.
  FakeUser still; { DefaultPrinterKeeper keeper(still.access()); (void)keeper.claim(); }
  require(DefaultPrinterKeeper(still.access()).recover() == R::recovered && still.def == L"HP LaserJet", "A default still ours was not restored");
  // A new claim settles a leftover record first and saves the true previous default.
  FakeUser leftover; { DefaultPrinterKeeper keeper(leftover.access()); (void)keeper.claim(); }
  leftover.printers.erase(ours); leftover.def = L"Office"; leftover.printers.insert(ours);
  DefaultPrinterKeeper again(leftover.access());
  require(again.claim() == R::claimed && leftover.def == ours && leftover.record.find("Office") != std::string::npos, "A new claim did not start from the user's current default");
  require(again.release() == R::restored && leftover.def == L"Office", "The user's default did not return");
}

void failures() {
  FakeUser unreadable; unreadable.loadFails = true;
  require(DefaultPrinterKeeper(unreadable.access()).claim() == R::failed && unreadable.def == L"HP LaserJet" && unreadable.sets == 0, "An unreadable record still changed the default");
  FakeUser unsaved; unsaved.saveFails = true;
  require(DefaultPrinterKeeper(unsaved.access()).claim() == R::failed && unsaved.def == L"HP LaserJet" && unsaved.sets == 0, "The default changed without a recovery record");
  FakeUser refused; refused.setFails = true;
  require(DefaultPrinterKeeper(refused.access()).claim() == R::failed && refused.record.empty() && refused.def == L"HP LaserJet", "A refused set left a record or a change");
  // Restore refused: the record stays for the next start.
  FakeUser stuck; DefaultPrinterKeeper keeper(stuck.access()); (void)keeper.claim(); stuck.setFails = true;
  require(keeper.release() == R::failed && !stuck.record.empty(), "A failed restore dropped the record");
  stuck.setFails = false;
  require(keeper.recover() == R::recovered && stuck.def == L"HP LaserJet" && stuck.record.empty(), "The kept record did not recover");
  // Each failure says why (hostAction 242 stage 30 error / cause), so a field report tells them apart.
  using F = DefaultFailure;
  { DefaultPrinterKeeper named(unreadable.access()); (void)named.claim(); require(named.failure() == F::recordUnreadable && named.win32() == 0, "An unreadable record was not named"); }
  { DefaultPrinterKeeper named(unsaved.access()); (void)named.claim(); require(named.failure() == F::recordNotWritten, "A record that could not be written was not named"); }
  { refused.error = 1801; DefaultPrinterKeeper named(refused.access()); (void)named.claim(); require(named.failure() == F::setRefused && named.win32() == 1801, "A refused SetDefaultPrinter did not carry its Windows error"); }
  { (void)keeper.claim(); stuck.setFails = true; stuck.error = 5; DefaultPrinterKeeper named(stuck.access()); (void)named.release(); require(named.failure() == F::restoreRefused && named.win32() == 5, "A refused restore was not named with its Windows error"); }
  { FakeUser fine; DefaultPrinterKeeper named(fine.access()); fine.loadFails = true; (void)named.claim(); fine.loadFails = false; require(named.claim() == R::claimed && named.failure() == F::none && named.win32() == 0, "A later success kept the earlier failure"); }
  { FakeUser dead; dead.loadFails = true; DefaultPrinterKeeper named(dead.access()); (void)named.release(); require(named.failure() == F::recordUnreadable, "A release with an unreadable record was not named"); }
  // A corrupt record is dropped without any change.
  FakeUser corrupt; corrupt.record = "{not json";
  require(DefaultPrinterKeeper(corrupt.access()).recover() == R::kept && corrupt.record.empty() && corrupt.def == L"HP LaserJet" && corrupt.sets == 0, "A corrupt record changed the default");
}

void migrationAndOwnership() {
  const auto legacyRecord = [] { return std::string("{\"v\":1,\"previous\":\"HP LaserJet\"}"); };
  // v1 always means the old queue, even though the helper now creates Multitap.
  FakeUser old; old.record = legacyRecord(); old.def = printLegacyQueueName; old.printers.insert(printLegacyQueueName);
  require(DefaultPrinterKeeper(old.access()).recover() == R::recovered && old.def == L"HP LaserJet", "A live owned legacy default did not recover before migration");
  FakeUser migrate; migrate.record = legacyRecord(); migrate.def = printLegacyQueueName; migrate.printers.insert(printLegacyQueueName);
  DefaultPrinterKeeper keeper(migrate.access());
  require(keeper.claim() == R::claimed && migrate.def == ours, "A new claim did not settle the legacy record first");
  require(JsonParser::parse(migrate.record).get("v").number() == 2 && JsonParser::parse(migrate.record).get("previous").string() == "HP LaserJet", "Migration saved the temporary old queue as the user's previous default");
  require(keeper.release() == R::restored && migrate.def == L"HP LaserJet", "The original default was lost during migration");
  // Queue absence does not attribute any different nonempty (or empty) default.
  for (bool present : {false, true}) {
    FakeUser chosen; chosen.record = legacyRecord(); chosen.def = L"Office";
    if (present) chosen.printers.insert(printLegacyQueueName);
    require(DefaultPrinterKeeper(chosen.access()).recover() == R::kept && chosen.def == L"Office" && !chosen.sets, "Legacy recovery overwrote a different default after a rename");
  }
  FakeUser notLegacy; notLegacy.record = legacyRecord(); notLegacy.def = ours;
  require(DefaultPrinterKeeper(notLegacy.access()).recover() == R::kept && notLegacy.def == ours && !notLegacy.sets, "A v1 record was reinterpreted as the renamed queue");
  for (const auto& current : {std::wstring(L"Office"), std::wstring()}) {
    FakeUser gone; DefaultPrinterKeeper k(gone.access()); require(k.claim() == R::claimed, "Missing queue setup");
    gone.printers.erase(ours); gone.def = current; const auto sets = gone.sets;
    require(k.recover() == R::kept && gone.def == current && gone.sets == sets, "A missing v2 queue invented attribution for a later default");
  }
  // A same-name queue with a different driver or port is never the saved lease.
  for (bool otherDriver : {false, true}) {
    FakeUser replaced; DefaultPrinterKeeper k(replaced.access()); require(k.claim() == R::claimed, "Replacement setup");
    if (otherDriver) replaced.bindings[ours].driver = L"Someone else's driver";
    else replaced.bindings[ours].port = L"Multitap_Print_50002";
    const auto sets = replaced.sets;
    require(k.recover() == R::kept && replaced.def == ours && replaced.sets == sets, "A replaced same-name queue was treated as the previous owned binding");
  }
  // The previous label "(내 PC)", now "(PC)": a v2 record a previous Host wrote for its queue (the legacy label, its
  // own port) still gives the user's default back after an update in the middle of a session.
  {
    FakeUser renamed; renamed.def = printLegacyQueueName; renamed.printers.insert(printLegacyQueueName);
    renamed.record = std::string("{\"v\":2,\"queue\":") + jsonQuote(utf8(printLegacyQueueName)) + ",\"port\":\"Multitap_Print_50001\",\"previous\":\"HP LaserJet\"}";
    require(DefaultPrinterKeeper(renamed.access()).recover() == R::recovered && renamed.def == L"HP LaserJet" && renamed.record.empty(), "A v2 record of the previous label did not restore the default");
  }
  FakeUser foreignLegacy; foreignLegacy.record = legacyRecord(); foreignLegacy.def = printLegacyQueueName; foreignLegacy.printers.insert(printLegacyQueueName); foreignLegacy.bindings[printLegacyQueueName].driver = L"Other";
  require(DefaultPrinterKeeper(foreignLegacy.access()).recover() == R::kept && !foreignLegacy.sets, "Legacy name and port prefix alone proved ownership");
  for (const auto& binding : {PrintQueueBinding{L"LPT1:", printDriverName}, PrintQueueBinding{L"Multitap_Print_50000", L"Other"}}) {
    FakeUser foreign; foreign.bindings[ours] = binding;
    require(DefaultPrinterKeeper(foreign.access()).claim() == R::failed && foreign.def == L"HP LaserJet" && !foreign.sets && foreign.record.empty(), "A foreign same-name queue became the default");
  }
  FakeUser refused; refused.record = legacyRecord(); refused.def = printLegacyQueueName; refused.printers.insert(printLegacyQueueName); refused.setFails = true;
  const auto pending = refused.record;
  DefaultPrinterKeeper blocked(refused.access());
  require(blocked.claim() == R::failed && blocked.failure() == DefaultFailure::restoreRefused && refused.record == pending && refused.sets == 1, "A failed legacy restore was overwritten by a new claim");
  FakeUser racing; auto access = racing.access(); unsigned reads = 0;
  access.binding = [&](const std::wstring&) -> std::optional<PrintQueueBinding> { return PrintQueueBinding{++reads == 1 ? L"Multitap_Print_50000" : L"Multitap_Print_50001", printDriverName}; };
  require(DefaultPrinterKeeper(access).claim() == R::failed && !racing.sets && racing.record.empty(), "Claim did not recheck the saved binding before changing the default");
  FakeUser restoreRace; DefaultPrinterKeeper first(restoreRace.access()); require(first.claim() == R::claimed, "Restore race setup");
  auto changing = restoreRace.access(); reads = 0;
  changing.binding = [&](const std::wstring&) -> std::optional<PrintQueueBinding> { return PrintQueueBinding{++reads == 1 ? L"Multitap_Print_50000" : L"Multitap_Print_50001", printDriverName}; };
  const auto sets = restoreRace.sets;
  require(DefaultPrinterKeeper(changing).recover() == R::kept && restoreRace.def == ours && restoreRace.sets == sets, "Recovery did not recheck the exact queue binding before changing the default");
  for (const auto& record : {
      std::string("{\"v\":3,\"previous\":\"HP LaserJet\"}"),
      std::string("{\"v\":2,\"queue\":\"Office\",\"port\":\"Multitap_Print_50000\",\"previous\":\"HP LaserJet\"}"),
      std::string("{\"v\":2,\"queue\":") + jsonQuote(utf8(ours)) + ",\"port\":\"LPT1:\",\"previous\":\"HP LaserJet\"}",
      std::string("{\"v\":2,\"queue\":") + jsonQuote(utf8(ours)) + ",\"port\":\"Multitap_Print_65536\",\"previous\":\"HP LaserJet\"}"}) {
    FakeUser invalid; invalid.def = ours; invalid.record = record;
    require(DefaultPrinterKeeper(invalid.access()).recover() == R::kept && invalid.def == ours && !invalid.sets && invalid.record.empty(), "A foreign or malformed recovery record changed a default");
  }
}

void revokeAfterFailedRestore() {
  FakeUser user; DefaultPrinterKeeper keeper(user.access());
  R released = R::none; unsigned removals = 0;
  const std::string session = "00000000-0000-4000-8000-000000000242";
  PrintSessionCore core(session, [](const std::string&) { return true; }, {
    [&] { return keeper.claim() == R::claimed ? PrintSetup::ready : PrintSetup::failed; },
    [&] { released = keeper.release(); user.printers.erase(ours); ++removals; }
  });
  require(core.message(JsonParser::parse("{\"type\":\"print.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"print-v1\",\"limits\":{\"chunkBytes\":1024,\"windowChunks\":2,\"maxJobBytes\":1048576}}")) && user.def == ours, "Revocation setup did not claim the default");
  user.setFails = true; core.closed();
  require(released == R::failed && removals == 1 && !user.printers.count(ours) && !user.record.empty(), "A failed restore kept a revoked live queue or lost its pending record before removal");
  user.setFails = false; user.def = L"Office"; const auto sets = user.sets;
  require(keeper.recover() == R::kept && user.def == L"Office" && user.sets == sets && user.record.empty(), "Recovery after revoked queue removal invented fallback attribution or promised restoration");
  core.closed(); require(removals == 1, "Repeated revocation removed the queue twice");
}

bool junction(const std::filesystem::path& link, const std::filesystem::path& target) {
  if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
  const HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + target.wstring(), print = target.wstring();
  const size_t names = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::vector<unsigned char> buffer(16 + names, 0);
  const auto put16 = [&](size_t at, size_t value) { buffer[at] = BYTE(value & 0xff); buffer[at + 1] = BYTE((value >> 8) & 0xff); };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, 4);
  put16(4, 8 + names); put16(8, 0); put16(10, substitute.size() * 2); put16(12, (substitute.size() + 1) * 2); put16(14, print.size() * 2);
  std::memcpy(buffer.data() + 16, substitute.c_str(), (substitute.size() + 1) * 2);
  std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * 2, print.c_str(), (print.size() + 1) * 2);
  DWORD returned = 0;
  const bool made = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
  CloseHandle(directory);
  return made;
}
bool emptyFolder(const std::filesystem::path& folder) { return std::filesystem::is_directory(folder) && std::filesystem::directory_iterator(folder) == std::filesystem::directory_iterator(); }

// The real record file in a temporary profile: it round-trips, and a junction
// on the way never redirects it; an unsafe record changes no default.
void recordFile() {
  const auto root = std::filesystem::temp_directory_path() / (L"multitap-print-default-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ignored; std::filesystem::remove_all(root, ignored); std::filesystem::create_directories(root);
  struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code e; RemoveDirectoryW((root / L"linked").c_str()); std::filesystem::remove_all(root, e); } } cleanup{root};
  const auto path = root / L"Multitap" / L"Host" / L"print-default.state";
  const DisplayScaleRecordFile plain(path);
  { FakeUser user; auto access = user.access();
    access.load = [&] { return plain.load(); }; access.save = [&](const std::string& text) { return plain.save(text); };
    DefaultPrinterKeeper keeper(access);
    require(keeper.claim() == R::claimed && std::filesystem::is_regular_file(path), "The record file was not written");
    require(keeper.release() == R::restored && user.def == L"HP LaserJet" && !std::filesystem::exists(path), "The record file did not restore and go"); }
  const auto elsewhere = root / L"elsewhere"; std::filesystem::create_directories(elsewhere);
  require(junction(root / L"linked", elsewhere), "Junction fixture");
  const DisplayScaleRecordFile linked(root / L"linked" / L"print-default.state");
  { FakeUser user; auto access = user.access();
    access.load = [&] { return linked.load(); }; access.save = [&](const std::string& text) { return linked.save(text); };
    require(DefaultPrinterKeeper(access).claim() == R::failed && user.def == L"HP LaserJet" && user.sets == 0 && emptyFolder(elsewhere), "A junctioned record folder was followed or the default changed"); }
}

void codes() {
  require(uint32_t(R::claimed) == uint32_t(PrintStage::defaultClaimed) && uint32_t(R::restored) == uint32_t(PrintStage::defaultRestored) && uint32_t(R::kept) == uint32_t(PrintStage::defaultKept) &&
          uint32_t(R::failed) == uint32_t(PrintStage::defaultFailed) && uint32_t(R::recovered) == uint32_t(PrintStage::defaultRecovered), "Result codes differ from diagnostic stages");
}
}  // namespace

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"save-set-restore", saveSetRestore}, {"user-choice", userChoice}, {"crash-recovery", crashRecovery}, {"migration-ownership", migrationAndOwnership}, {"revoke-failed-restore", revokeAfterFailedRestore}, {"failures", failures}, {"record-file", recordFile}, {"codes", codes}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"print-default\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"printerApi\":\"substituted\",\"settingsChanged\":false}\n";
  return failed ? 1 : 0;
}
