// print-v1 on the host: the session.start grant, document titles, the printer
// security descriptor, the printer/port lifecycle and crash leftovers (with a
// substituted spooler), the session logic (window, acknowledgements, digest,
// cancel, backpressure, skipped jobs, revocation), the protected job files and
// the loopback listener with the real connection-owner lookup. No printer,
// port or spooler setting of this machine is created or changed.
#include <winsock2.h>
#include "print_redirection.hpp"
#include <winioctl.h>
#include <cstring>
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::string session = "00000000-0000-4000-8000-000000000240";
const std::wstring userSid = L"S-1-5-21-1111111111-2222222222-3333333333-1001";

std::vector<uint8_t> decode(const std::string& text) {
  DWORD length = 0;
  if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr)) return {};
  std::vector<uint8_t> out(length);
  if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, out.data(), &length, nullptr, nullptr)) return {};
  out.resize(length); return out;
}
std::string sha256(const std::vector<uint8_t>& bytes) { PrintDigest digest; digest.update(bytes.data(), bytes.size()); return digest.finish(); }

void grant() {
  const auto start = [](const std::string& extra) { return JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\"" + extra + '}'); };
  const auto url = "wss://vdi.example/api/v1/hosts/sessions/" + session + "/print";
  require(printSessionUrl(start(",\"printProtocol\":\"print-v1\",\"printUrl\":\"" + url + "\""), session) == url, "A granted start was not recognised");
  require(printSessionUrl(start(",\"printUrl\":\"" + url + "\""), session).empty(), "A URL without the grant started printing");
  require(printSessionUrl(start(",\"printProtocol\":\"print-v2\",\"printUrl\":\"" + url + "\""), session).empty(), "Another protocol started printing");
  require(printSessionUrl(start(",\"printProtocol\":\"print-v1\""), session).empty(), "A grant without a URL started printing");
  require(printSessionUrl(start(",\"printProtocol\":\"print-v1\",\"printUrl\":\"https://vdi.example/api/v1/hosts/sessions/" + session + "/print\""), session).empty(), "A non-WebSocket URL was accepted");
  require(printSessionUrl(start(",\"printProtocol\":\"print-v1\",\"printUrl\":\"wss://vdi.example/api/v1/hosts/sessions/other/print\""), session).empty(), "Another session's URL was accepted");
  require(printSessionUrl(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"rdp\",\"sessionId\":\"" + session + "\",\"printProtocol\":\"print-v1\",\"printUrl\":\"" + url + "\"}"), session).empty(), "An RDP start started streaming printing");
}

void titles() {
  require(printTitle(L"분기 보고서.docx - Word") == utf8(L"분기 보고서.docx - Word"), "A plain Korean title changed");
  require(printTitle(L"a\tb\r\nc\x7f") == "abc", "Control characters were kept");
  require(printTitle(std::wstring(500, L'x')).size() == printMaxTitleUnits, "A long title was not bounded");
  require(printTitle(L"ok\xd800") == "ok" && printTitle(L"\xdc00ok") == "ok", "A broken surrogate was kept");
  require(printTitle(L"\xd83d\xde00") == utf8(L"\xd83d\xde00"), "A surrogate pair was broken");
  const std::wstring edge = std::wstring(199, L'y') + L"\xd83d\xde00";
  require(wide(printTitle(edge)).size() == 199, "A pair cut at the bound was half kept");
  require(printTitle(L"").empty(), "An empty title is not empty");
}

void security() {
  const auto sddl = printerSecurityDescriptor(userSid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "The printer descriptor is not valid SDDL");
  LocalFree(descriptor);
  require(sddl.find(L"(A;;SWRC;;;" + userSid + L")") != std::wstring::npos, "The session user cannot print");
  require(sddl.find(L";;;WD)") == std::wstring::npos && sddl.find(L";;;BU)") == std::wstring::npos && sddl.find(L";;;AU)") == std::wstring::npos && sddl.find(L";;;IU)") == std::wstring::npos, "Other users were granted access");
  require(sddl.find(L"(A;;LCSDRCWDWO;;;BA)") != std::wstring::npos && sddl.find(L"SW", sddl.find(L"(A;;LCSDRCWDWO;;;BA)")) > sddl.find(L";;;BA)", sddl.find(L"(A;;LCSDRCWDWO;;;BA)")), "Administrators were given Print");
  bool refused = false; try { (void)printerSecurityDescriptor(L"S-1-5-21-1)(A;;GA;;;WD"); } catch (...) { refused = true; }
  require(refused, "An injected SID was accepted");
  require(printOwnedPort(L"Multitap_Print_51000") && printOwnedPort(L"multitap_print_1"), "Our ports were not recognised");
  require(!printOwnedPort(L"Multitap_Print_") && !printOwnedPort(L"Multitap_PrintX") && !printOwnedPort(L"Multitap_Print_12a") && !printOwnedPort(L"Multitap_Print_123456") && !printOwnedPort(L"Multitap_Print_0") && !printOwnedPort(L"Multitap_Print_65536") && !printOwnedPort(L"Multitap_Print_00001") && !printOwnedPort(L"IP_192.0.2.5") && !printOwnedPort(L"LPT1:"), "A foreign port looked like ours");
}

// A fake spooler: printers (name, port) and ports, with every call logged.
struct FakeSpooler {
  bool running = true, driver = true, monitor = true; DWORD addPrinterError = 0, addPortError = 0, deletePrinterError = 0, quarantineError = 0;
  int deletes = 0, quarantines = 0, waits = 0;
  std::vector<PrintPrinterEntry> printers; std::vector<std::wstring> ports; std::vector<std::wstring> log; std::wstring sddl;
  PrintSpoolerAccess access() {
    PrintSpoolerAccess value;
    value.spoolerRunning = [this] { return running; }; value.driverAvailable = [this] { return driver; }; value.monitorAvailable = [this] { return monitor; };
    value.printers = [this] { return printers; }; value.ports = [this] { return ports; };
    value.addPort = [this](const std::wstring& port, uint16_t tcp) { log.push_back(L"addPort:" + port + L":" + std::to_wstring(tcp)); if (addPortError) return addPortError; ports.push_back(port); return DWORD(0); };
    value.deletePort = [this](const std::wstring& port) { log.push_back(L"deletePort:" + port); std::erase_if(ports, [&](const auto& item) { return samePrintText(item, port); }); return DWORD(0); };
    value.addPrinter = [this](const std::wstring& name, const std::wstring& port, const std::wstring& security) { log.push_back(L"addPrinter:" + name + L":" + port); if (addPrinterError) return addPrinterError; sddl = security; printers.push_back({name, port, printDriverName}); return DWORD(0); };
    value.deletePrinter = [this](const std::wstring& name) { ++deletes; log.push_back(L"deletePrinter:" + name); if (deletePrinterError) return deletePrinterError; std::erase_if(printers, [&](const auto& item) { return samePrintText(item.name, name); }); return DWORD(0); };
    value.quarantinePrinter = [this](const std::wstring& name) { ++quarantines; log.push_back(L"quarantine:" + name); if (quarantineError) return quarantineError; for (auto& printer : printers) if (samePrintText(printer.name, name)) printer.port = L"nul:"; return DWORD(0); };
    value.retryWait = [this] { ++waits; };
    value.printingTitle = [](const std::wstring&) { return std::wstring(L"Document"); };
    return value;
  }
  bool hasPrinter(const std::wstring& name) const { return std::any_of(printers.begin(), printers.end(), [&](const auto& item) { return item.name == name; }); }
  bool hasPort(const std::wstring& name) const { return std::find(ports.begin(), ports.end(), name) != ports.end(); }
};

void lifecycle() {
  FakeSpooler spooler;
  spooler.printers = {{L"Office", L"IP_192.0.2.5", L"Other"}, {printLegacyQueueName, L"Multitap_Print_50000", printDriverName}, {L"Renamed by someone", L"Multitap_Print_50001", printDriverName}};
  spooler.ports = {L"LPT1:", L"IP_192.0.2.5", L"Multitap_Print_50000", L"Multitap_Print_50001", L"Multitap_Print_50002", L"Multitap_PrintX"};
  PrintQueueKeeper keeper(spooler.access());
  require(keeper.install(userSid, 51000) == PrintSetup::ready && keeper.installed(), "The printer was not installed");
  require(spooler.hasPrinter(L"Office") && spooler.hasPort(L"IP_192.0.2.5") && spooler.hasPort(L"LPT1:") && spooler.hasPort(L"Multitap_PrintX"), "A foreign printer or port was touched");
  require(!spooler.hasPrinter(L"Renamed by someone") && !spooler.hasPort(L"Multitap_Print_50000") && !spooler.hasPort(L"Multitap_Print_50001") && !spooler.hasPort(L"Multitap_Print_50002"), "Leftovers of an earlier run stayed");
  require(spooler.hasPrinter(printQueueName) && spooler.hasPort(L"Multitap_Print_51000") && spooler.printers.back().port == L"Multitap_Print_51000", "The session printer is not on its own port");
  require(spooler.sddl == printerSecurityDescriptor(userSid), "The printer was created without the session user's descriptor");
  keeper.remove();
  require(!spooler.hasPrinter(printQueueName) && !spooler.hasPort(L"Multitap_Print_51000") && spooler.hasPrinter(L"Office") && !keeper.installed(), "Removal left the printer or touched another");
  keeper.remove();  // idempotent
  require(spooler.log.back() == L"deletePort:Multitap_Print_51000", "A second removal called the spooler again");

  // A queue that cannot be deleted never stays on the port whose listener
  // closes. Tried three times, then paused, emptied and moved to "nul:"; the port goes; the next
  // session's leftovers sweep deletes the quarantined queue and installs as usual.
  {
    FakeSpooler stuck; PrintQueueKeeper stuckKeeper(stuck.access());
    require(stuckKeeper.install(userSid, 51002) == PrintSetup::ready, "The printer was not installed");
    stuck.deletePrinterError = ERROR_ACCESS_DENIED; stuck.deletes = 0;
    stuckKeeper.remove();
    require(stuck.deletes == 3 && stuck.waits == 2 && stuck.quarantines == 1, "A queue that could not be deleted was not retried and then quarantined");
    require(stuck.hasPrinter(printQueueName) && stuck.printers.back().port == L"nul:" && !stuck.hasPort(L"Multitap_Print_51002") && !stuckKeeper.installed(), "A queue stayed on the closed listener's port");
    stuck.deletePrinterError = 0;
    PrintQueueKeeper next(stuck.access());
    require(next.install(userSid, 51004) == PrintSetup::ready && stuck.printers.size() == 1 && stuck.printers.back().port == L"Multitap_Print_51004", "The quarantined queue was not removed by the next session");
    // A quarantine that fails as well: the port is not deleted under a queue that still uses it.
    FakeSpooler stubborn; PrintQueueKeeper own(stubborn.access()); require(own.install(userSid, 51005) == PrintSetup::ready, "install");
    stubborn.deletePrinterError = ERROR_ACCESS_DENIED; stubborn.quarantineError = ERROR_ACCESS_DENIED;
    own.remove();
    require(stubborn.hasPrinter(printQueueName) && stubborn.hasPort(L"Multitap_Print_51005") && stubborn.quarantines == 1 && !own.installed(), "A failed quarantine deleted a port still in use");
    // A queue on "nul:" is ours only with our name and driver.
    FakeSpooler other; other.printers = {{printQueueName, L"nul:", L"Other driver"}, {L"Someone's", L"nul:", printDriverName}}; other.ports = {};
    require(PrintQueueKeeper(other.access()).removeLeftovers() == 0 && other.printers.size() == 2, "A queue on nul: that is not ours was deleted");
  }
  // Someone else's printer of the same name stays untouched; no install.
  FakeSpooler conflict; conflict.printers = {{printQueueName, L"LPT1:", L"Other"}}; conflict.ports = {L"LPT1:"};
  PrintQueueKeeper second(conflict.access());
  require(second.install(userSid, 51001) == PrintSetup::nameConflict && conflict.printers.size() == 1 && conflict.ports.size() == 1 && !second.installed(), "A foreign printer of the same name was replaced");
  FakeSpooler samePrefix; samePrefix.printers = {{printQueueName, L"Multitap_Print_50003", L"Other driver"}}; samePrefix.ports = {L"Multitap_Print_50003"};
  require(PrintQueueKeeper(samePrefix.access()).install(userSid, 51001) == PrintSetup::nameConflict && samePrefix.printers.size() == 1 && samePrefix.ports.size() == 1 && samePrefix.log.empty(), "A foreign same-name queue with only our port prefix was removed");
  FakeSpooler pending; pending.printers = {{printLegacyQueueName, L"Multitap_Print_50001", printDriverName}}; pending.ports = {L"Multitap_Print_50001"};
  unsigned recovery = 0;
  require(PrintQueueKeeper(pending.access()).install(userSid, 51001, [&] { ++recovery; return false; }) == PrintSetup::failed && recovery == 1 && pending.printers.size() == 1 && pending.ports.size() == 1 && pending.log.empty(), "A failed default recovery still removed the legacy queue");
  for (const uint32_t result : {27u, 28u, 30u, 32u, UINT32_MAX}) {
    FakeSpooler unknown; unknown.printers = {{printLegacyQueueName, L"Multitap_Print_50001", printDriverName}}; unknown.ports = {L"Multitap_Print_50001"};
    require(PrintQueueKeeper(unknown.access()).install(userSid, 51001, [&] { return acceptPrintRecovery(result); }) == PrintSetup::failed && unknown.printers.size() == 1 && unknown.ports.size() == 1 && unknown.log.empty(), "An unknown or wrong-operation recovery result authorized legacy cleanup");
  }
  for (const uint32_t result : {0u, 29u, 31u}) {
    FakeSpooler recovered; recovered.printers = {{printLegacyQueueName, L"Multitap_Print_50001", printDriverName}}; recovered.ports = {L"Multitap_Print_50001"};
    PrintQueueKeeper ready(recovered.access());
    require(ready.install(userSid, 51001, [&] { return acceptPrintRecovery(result); }) == PrintSetup::ready && recovered.hasPrinter(printQueueName) && !recovered.hasPrinter(printLegacyQueueName), "A known recovery result did not permit normal queue migration");
    ready.remove();
  }
  FakeSpooler changed; PrintQueueKeeper owned(changed.access()); require(owned.install(userSid, 51001) == PrintSetup::ready, "Changed queue setup");
  changed.printers.front().driver = L"Other driver"; changed.log.clear(); owned.remove();
  require(changed.printers.size() == 1 && changed.ports.size() == 1 && changed.log.empty(), "Session cleanup removed a replaced foreign queue or its live port");
  // A refused printer leaves no port behind.
  FakeSpooler refused; refused.addPrinterError = ERROR_ACCESS_DENIED;
  PrintQueueKeeper third(refused.access());
  require(third.install(userSid, 51002) == PrintSetup::failed && refused.ports.empty() && refused.printers.empty(), "A failed printer left its port");
  FakeSpooler noPort; noPort.addPortError = ERROR_ACCESS_DENIED;
  PrintQueueKeeper fourth(noPort.access());
  require(fourth.install(userSid, 51003) == PrintSetup::failed && noPort.printers.empty(), "A printer was added without its port");
  // Nothing is installed when Windows lacks the in-box parts or the spooler.
  FakeSpooler bare; bare.driver = false;
  require(PrintQueueKeeper(bare.access()).install(userSid, 51004) == PrintSetup::driverUnavailable && bare.log.empty(), "A missing driver did not refuse");
  FakeSpooler stopped; stopped.running = false;
  require(PrintQueueKeeper(stopped.access()).install(userSid, 51005) == PrintSetup::spoolerUnavailable && stopped.log.empty(), "A stopped spooler did not refuse");
  require(PrintQueueKeeper(FakeSpooler{}.access()).install(L"not-a-sid", 51006) == PrintSetup::failed, "An invalid user was accepted");
}

PrintJob memoryJob(const std::string& id, std::vector<uint8_t> bytes, const std::string& title = utf8(L"보고서")) {
  auto data = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  PrintJob job; job.id = id; job.size = data->size(); job.sha256 = sha256(*data); job.title = title;
  job.read = [data](uint64_t offset, uint8_t* out, size_t size) { if (offset > data->size() || size > data->size() - offset) return false; std::memcpy(out, data->data() + offset, size); return true; };
  job.owner = data;
  return job;
}
std::string ready(const std::string& id, int chunk, int window, uint64_t maximum) {
  return "{\"type\":\"print.ready\",\"sessionId\":\"" + id + "\",\"protocol\":\"print-v1\",\"limits\":{\"chunkBytes\":" + std::to_string(chunk) + ",\"windowChunks\":" + std::to_string(window) + ",\"maxJobBytes\":" + std::to_string(maximum) + "}}";
}
struct Wire {
  std::vector<Json> sent; bool full = false;
  PrintSessionCore::Send send() { return [this](const std::string& message) { if (full) return false; sent.push_back(JsonParser::parse(message)); return true; }; }
  std::vector<Json> take() { auto out = std::move(sent); sent.clear(); return out; }
};

void sessionCore() {
  Wire wire; int installs = 0, removes = 0;
  PrintSessionCore core(session, wire.send(), {[&] { ++installs; return PrintSetup::ready; }, [&] { ++removes; }});
  require(!core.message(JsonParser::parse("{\"type\":\"print.ack\",\"jobId\":\"x\",\"offset\":1}")), "A message before ready was accepted");
  require(core.message(JsonParser::parse(ready(session, 1024, 2, 1 << 20))) && installs == 1, "Ready did not install the printer");
  auto sent = wire.take();
  require(sent.size() == 1 && sent[0].get("type").string() == "print.queue" && sent[0].get("state").string() == "ready", "The client was not told the printer is ready");
  require(!core.message(JsonParser::parse(ready(session, 1024, 2, 1 << 20))), "A second ready was accepted");

  // One document: header, two chunks (window), more after each acknowledgement, end.
  std::vector<uint8_t> pdf(3000); for (size_t i = 0; i < pdf.size(); ++i) pdf[i] = uint8_t(i * 7 + 3);
  core.offer(memoryJob("job1", pdf)); core.pump();
  sent = wire.take();
  require(sent.size() == 3 && sent[0].get("type").string() == "print.job" && sent[0].get("jobId").string() == "job1" && sent[0].get("size").number() == 3000 &&
          sent[0].get("format").string() == "pdf" && sent[0].get("title").string() == utf8(L"보고서"), "The header is wrong");
  require(sent[1].get("type").string() == "print.chunk" && sent[1].get("offset").number() == 0 && sent[2].get("offset").number() == 1024, "Chunks are out of order");
  core.pump(); require(wire.take().empty(), "More than the window was sent without acknowledgements");
  std::vector<uint8_t> received;
  for (const auto& chunk : {sent[1], sent[2]}) { const auto bytes = decode(chunk.get("data").string()); received.insert(received.end(), bytes.begin(), bytes.end()); }
  // A future or another job's acknowledgement opens no room.
  core.message(JsonParser::parse("{\"type\":\"print.ack\",\"jobId\":\"job1\",\"offset\":9999}")); core.message(JsonParser::parse("{\"type\":\"print.ack\",\"jobId\":\"other\",\"offset\":2048}"));
  core.pump(); require(wire.take().empty(), "An invalid acknowledgement opened the window");
  require(core.message(JsonParser::parse("{\"type\":\"print.ack\",\"jobId\":\"job1\",\"offset\":1024}")), "An acknowledgement was refused");
  // The last chunk fits the window again; the end (all bytes sent) follows at once.
  core.pump(); sent = wire.take();
  require(sent.size() == 2 && sent[0].get("offset").number() == 2048 && decode(sent[0].get("data").string()).size() == 952, "The last chunk did not follow the acknowledgement");
  { const auto last = decode(sent[0].get("data").string()); received.insert(received.end(), last.begin(), last.end()); }
  require(sent[1].get("type").string() == "print.end" && sent[1].get("jobId").string() == "job1" && sent[1].get("sha256").string() == sha256(pdf) && received == pdf, "The document did not arrive intact");
  core.message(JsonParser::parse("{\"type\":\"print.ack\",\"jobId\":\"job1\",\"offset\":3000}"));
  core.pump(); require(wire.take().empty(), "A late acknowledgement of an ended job sent something");
  require(core.message(JsonParser::parse("{\"type\":\"print.result\",\"jobId\":\"job1\",\"result\":\"printed\"}")), "A result was refused");

  // A client cancel ends only that job; the next one starts at once.
  core.offer(memoryJob("job2", std::vector<uint8_t>(5000, 1))); core.offer(memoryJob("job3", std::vector<uint8_t>(10, 2)));
  core.pump(); sent = wire.take();
  require(sent.size() == 3 && sent[0].get("jobId").string() == "job2", "The second job did not start");
  core.message(JsonParser::parse("{\"type\":\"print.cancel\",\"jobId\":\"job2\"}"));
  core.pump(); sent = wire.take();
  require(sent.size() == 3 && sent[0].get("type").string() == "print.job" && sent[0].get("jobId").string() == "job3" && sent[2].get("type").string() == "print.end", "The job after a cancel did not go");

  // Backpressure: nothing is lost, the job continues when the channel drains.
  wire.full = true; core.offer(memoryJob("job4", std::vector<uint8_t>(100, 4))); core.pump();
  require(wire.sent.empty() && core.busy(), "A full channel lost the job");
  wire.full = false; core.pump(); sent = wire.take();
  require(sent.size() == 3 && sent[0].get("jobId").string() == "job4" && sent[2].get("type").string() == "print.end", "The job did not resume after backpressure");

  // The relay's error (revocation, session end) removes the printer at once.
  core.offer(memoryJob("job5", std::vector<uint8_t>(4000, 5))); core.pump(); wire.take();
  require(!core.message(JsonParser::parse("{\"type\":\"print.error\",\"reason\":\"print_policy_changed\"}")), "An error did not close the channel");
  core.closed();
  require(removes == 1 && !core.installed() && !core.busy(), "The printer stayed after the channel closed");
  core.offer(memoryJob("job6", std::vector<uint8_t>(10, 6))); core.pump();
  require(wire.sent.empty(), "A document was offered after the channel closed");
  core.closed(); require(removes == 1, "Removal ran twice");
}

// channel-resume-v1: when a pair ends, the documents the client never heard of are counted (the next pair says
// so once); the ones it heard of it counts itself.
void sessionUnsent() {
  Wire wire;
  PrintSessionCore core(session, wire.send(), {[] { return PrintSetup::ready; }, [] {}});
  require(core.unsent() == 0, "An idle printer has unsent documents");
  require(core.message(JsonParser::parse(ready(session, 1024, 2, 1 << 20))), "Ready was refused");
  wire.take();
  // Three documents arrive while the channel is full: none was announced.
  wire.full = true;
  // Larger than the window (two chunks of 1024), so the first document stays in transfer.
  for (const char* id : {"a", "b", "c"}) core.offer(memoryJob(id, std::vector<uint8_t>(5000, 1)));
  core.pump();
  require(core.unsent() == 3, "Documents that were never announced were not counted");
  // The first is announced (the client counts it from here on); two still wait.
  wire.full = false; core.pump();
  const auto sent = wire.take();
  require(!sent.empty() && sent[0].get("type").string() == "print.job" && sent[0].get("jobId").string() == "a", "The first document was not announced");
  require(core.unsent() == 2, "An announced document was counted twice");
  // The new pair's printer says it once, with the fixed reason the client already shows.
  core.skipped("print_job_failed"); core.pump();
  bool said = false; for (const auto& value : wire.take()) if (value.get("type").string() == "print.skipped" && value.get("reason").string() == "print_job_failed") said = true;
  require(said, "The lost documents were not reported");
  core.closed();
  require(core.unsent() == 0, "A closed printer still counts documents");
}

void sessionLimits() {
  Wire wire; int removes = 0;
  PrintSessionCore core(session, wire.send(), {[] { return PrintSetup::ready; }, [&] { ++removes; }});
  require(!core.message(JsonParser::parse(ready("another-session", 1024, 2, 1 << 20))), "Another session's ready was accepted");
  require(!core.message(JsonParser::parse(ready(session, 512, 2, 1 << 20))), "A chunk below the minimum was accepted");
  require(!core.message(JsonParser::parse(ready(session, 262144, 2, 1 << 20))), "A chunk above the maximum was accepted");
  require(core.message(JsonParser::parse(ready(session, 1024, 4, 2000))), "Valid limits were refused");
  wire.take();
  core.offer(memoryJob("big", std::vector<uint8_t>(2001, 9))); core.pump();
  auto sent = wire.take();
  require(sent.size() == 1 && sent[0].get("type").string() == "print.skipped" && sent[0].get("reason").string() == "print_job_too_large", "A document above the relay's limit was offered");
  core.skipped("print_storage_unavailable"); core.pump(); sent = wire.take();
  require(sent.size() == 1 && sent[0].get("reason").string() == "print_storage_unavailable", "A skipped notice was lost");
  // A read failure cancels only that document.
  PrintJob broken = memoryJob("broken", std::vector<uint8_t>(100, 1)); broken.read = [](uint64_t, uint8_t*, size_t) { return false; };
  core.offer(std::move(broken)); core.offer(memoryJob("after", std::vector<uint8_t>(10, 3))); core.pump(); sent = wire.take();
  require(sent.size() == 5 && sent[0].get("jobId").string() == "broken" && sent[1].get("type").string() == "print.cancel" && sent[2].get("jobId").string() == "after" && sent[4].get("type").string() == "print.end", "A read failure was not limited to its document");

  // The printer could not be prepared: the client is told why; nothing is offered.
  Wire other; int otherRemoves = 0;
  PrintSessionCore failed(session, other.send(), {[] { return PrintSetup::nameConflict; }, [&] { ++otherRemoves; }});
  require(failed.message(JsonParser::parse(ready(session, 1024, 2, 1 << 20))), "A failed setup closed the channel");
  sent = other.take();
  require(sent.size() == 1 && sent[0].get("state").string() == "unavailable" && sent[0].get("reason").string() == "print_name_conflict", "The setup failure was not reported");
  failed.offer(memoryJob("x", std::vector<uint8_t>(10, 1))); failed.pump(); failed.closed();
  require(other.sent.empty() && otherRemoves == 0, "A queue that was never installed offered a document or was removed");
}

std::filesystem::path scratch(const wchar_t* name) {
  const auto root = std::filesystem::temp_directory_path() / (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ignored; std::filesystem::remove_all(root, ignored); std::filesystem::create_directories(root);
  return root;
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
  CloseHandle(directory); return made;
}
size_t jobFiles(const std::filesystem::path& folder) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder)) if (entry.path().filename().wstring().rfind(L"job-", 0) == 0) ++count;
  return count;
}

void storage() {
  const auto root = scratch(L"multitap-print-store");
  struct Cleanup { std::filesystem::path root; ~Cleanup() { RemoveDirectoryW((root / L"linked").c_str()); std::error_code e; std::filesystem::remove_all(root, e); } } cleanup{root};
  auto budget = std::make_shared<PrintBudget>();
  {
    PrintJobStore store(root / L"print", budget, false);
    auto file = store.create();
    require(file && jobFiles(root / L"print") == 1 && budget->jobs == 1, "A document file was not created in the folder");
    const std::vector<uint8_t> abc{'a', 'b', 'c'};
    require(file->append(abc.data(), abc.size()) && file->size() == 3 && budget->bytes == 3, "A document file did not take its bytes");
    std::array<uint8_t, 3> back{}; require(file->read(0, back.data(), 3) && std::equal(back.begin(), back.end(), abc.begin()), "A document file did not read back");
    require(!file->read(1, back.data(), 3) && !file->read(4, back.data(), 0), "A read past the end succeeded");
    require(file->finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "The document digest is wrong");
    file.reset();
    require(jobFiles(root / L"print") == 0 && budget->jobs == 0 && budget->bytes == 0, "A released document file stayed on disk or in the budget");
  }
  // A power loss leaves a document behind: only exact job names are swept.
  { WinHandle stale(CreateFileW((root / L"print" / L"job-0123456789abcdef0123456789abcdef.pdf").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)); require(stale.value != INVALID_HANDLE_VALUE, "Stale fixture"); }
  { WinHandle other(CreateFileW((root / L"print" / L"keep.pdf").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)); require(other.value != INVALID_HANDLE_VALUE, "Other fixture"); }
  require(PrintJobStore::sweep(root / L"print") == 0 && jobFiles(root / L"print") == 1, "An untrusted folder was swept");
  require(PrintJobStore::sweep(root / L"print", false) == 1 && jobFiles(root / L"print") == 0 && std::filesystem::exists(root / L"print" / L"keep.pdf"), "The sweep removed the wrong files");
  // The folder (or a folder on the way) being a junction is refused.
  std::filesystem::create_directories(root / L"target");
  require(junction(root / L"linked", root / L"target"), "Junction fixture");
  bool refused = false; try { PrintJobStore linked(root / L"linked", budget, false); } catch (...) { refused = true; }
  require(refused, "A junction folder was used for documents");
  refused = false; try { PrintJobStore below(root / L"linked" / L"print", budget, false); } catch (...) { refused = true; }
  require(refused && !std::filesystem::exists(root / L"target" / L"print"), "A junction on the way was followed");
}

SOCKET connectTo(uint16_t port) {
  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
  if (client == INVALID_SOCKET || connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { if (client != INVALID_SOCKET) closesocket(client); return INVALID_SOCKET; }
  return client;
}
bool sendAll(SOCKET socket, const std::vector<uint8_t>& bytes) {
  size_t at = 0; while (at < bytes.size()) { const int sent = send(socket, reinterpret_cast<const char*>(bytes.data() + at), int(std::min<size_t>(bytes.size() - at, 65536)), 0); if (sent <= 0) return false; at += size_t(sent); }
  return true;
}

void listener() {
  // A refused peer is recorded at most once a minute (any local program can connect to the port).
  { PrintListener quiet;
    require(quiet.noteRejected(1000) && !quiet.noteRejected(2000) && !quiet.noteRejected(60999) && quiet.noteRejected(61000) && !quiet.noteRejected(61001), "Refused peers were not throttled"); }
  const auto root = scratch(L"multitap-print-listener");
  struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code e; std::filesystem::remove_all(root, e); } } cleanup{root};
  auto budget = std::make_shared<PrintBudget>();
  PrintJobStore store(root / L"print", budget, false);
  std::mutex mutex; std::condition_variable changed; std::vector<PrintJob> jobs; std::vector<std::string> skips; DWORD owner = 0; bool trust = true;
  PrintListener::Access access;
  access.trusted = [&](SOCKET socket) { std::lock_guard lock(mutex); owner = printLoopbackPeerProcess(socket); return trust; };
  access.title = [] { return printTitle(L"Report"); };
  access.file = [&] { return store.create(); };
  PrintListener server;
  const auto port = server.start(access, [&](PrintJob job) { std::lock_guard lock(mutex); jobs.push_back(std::move(job)); changed.notify_all(); },
                                 [&](const char* reason) { std::lock_guard lock(mutex); skips.emplace_back(reason); changed.notify_all(); }, budget);
  require(port != 0, "The loopback listener did not start");
  // Another listener cannot take the same port (exclusive address use).
  SOCKET squatter = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); BOOL reuse = TRUE; setsockopt(squatter, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in same{}; same.sin_family = AF_INET; same.sin_addr.s_addr = htonl(INADDR_LOOPBACK); same.sin_port = htons(port);
  require(bind(squatter, reinterpret_cast<sockaddr*>(&same), sizeof(same)) != 0, "Another socket could bind the print port"); closesocket(squatter);

  std::vector<uint8_t> pdf(300000); for (size_t i = 0; i < pdf.size(); ++i) pdf[i] = uint8_t((i * 31) ^ (i >> 7));
  SOCKET client = connectTo(port); require(client != INVALID_SOCKET, "Connect to the listener");
  require(sendAll(client, pdf), "Send a document"); shutdown(client, SD_SEND);
  const auto deliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  { std::unique_lock lock(mutex); require(changed.wait_until(lock, deliveryDeadline, [&] { return !jobs.empty(); }), "The document was not delivered"); }
  closesocket(client);
  {
    std::lock_guard lock(mutex);
    require(owner == GetCurrentProcessId(), "The connection's owning process was not found");
    auto& job = jobs.front();
    require(job.size == pdf.size() && job.sha256 == sha256(pdf) && job.title == "Report" && job.id.size() == 32, "The delivered document is wrong");
    std::vector<uint8_t> back(pdf.size()); require(job.read(0, back.data(), back.size()) && back == pdf, "The kept document differs");
    require(budget->jobs == 1 && budget->bytes == pdf.size(), "The budget does not count the waiting document");
    jobs.clear();
  }
  // Delivery can notify before receive() drops its own shared file reference.
  // Observe actual release within the original delivery deadline, not a scheduler race.
  while (std::chrono::steady_clock::now() < deliveryDeadline && (budget->jobs != 0 || jobFiles(root / L"print") != 0)) Sleep(1);
  require(budget->jobs == 0 && jobFiles(root / L"print") == 0, "A delivered and released document stayed");
  // An untrusted peer is closed unread; an empty connection is no document.
  { std::lock_guard lock(mutex); trust = false; }
  client = connectTo(port); sendAll(client, {1, 2, 3}); shutdown(client, SD_SEND);
  char one = 0; const int answer = recv(client, &one, 1, 0); closesocket(client);
  require(answer <= 0, "An untrusted peer got a reply");
  { std::lock_guard lock(mutex); trust = true; }
  client = connectTo(port); shutdown(client, SD_SEND); recv(client, &one, 1, 0); closesocket(client);
  Sleep(300);
  { std::lock_guard lock(mutex); require(jobs.empty() && skips.empty(), "An untrusted or empty connection became a document"); }
  // The real spooler check never trusts this test process.
  require(!printTrustedSpooler(GetCurrentProcessId()) && !printTrustedSpooler(0), "A user process was trusted as the spooler");
  server.stop();
  require(connectTo(port) == INVALID_SOCKET, "The port stayed open after stop");
}

// Read-only queries of this machine's spooler (reported, never changed).
void probes() {
  const bool spooler = printSpoolerProcessId() != 0, driver = printDriverInstalled(), monitor = printMonitorInstalled();
  std::cout << "{\"spoolerRunning\":" << (spooler ? "true" : "false") << ",\"pdfDriver\":" << (driver ? "true" : "false") << ",\"tcpMonitor\":" << (monitor ? "true" : "false") << "}\n";
  require(printRedirectionAvailable() == (spooler && driver && monitor), "The capability does not match the probes");
  const auto fields = printCapabilityFields();
  const bool ready = spooler && driver && monitor;
  require((fields.find("printProtocol") != std::string::npos) == ready && (fields.find("printUnavailableReason") != std::string::npos) == !ready, "The capability field does not match");
  // The reason is told apart: a stopped spooler, or a missing driver or port monitor.
  require(std::string(printUnavailableReason(true, true, true)).empty(), "A ready machine gave a reason");
  require(std::string(printUnavailableReason(false, true, true)) == "print_spooler_unavailable" && std::string(printUnavailableReason(false, false, false)) == "print_spooler_unavailable", "A stopped spooler was not named first");
  require(std::string(printUnavailableReason(true, false, true)) == "print_driver_unavailable" && std::string(printUnavailableReason(true, true, false)) == "print_driver_unavailable", "A missing driver or monitor was not named");
  require(fields == (ready ? std::string(",\"printProtocol\":\"print-v1\"") : std::string(",\"printUnavailableReason\":\"") + printUnavailableReason(spooler, spooler && driver, spooler && monitor) + "\""), "The capability fields are not the exact JSON fragment");
  if (spooler) { const auto printers = printLocalPrinters(); const auto ports = printLocalPorts(); require(!ports.empty() || printers.empty(), "Ports were unreadable"); }
}
}  // namespace

int main() {
  WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
  const struct { const char* name; void (*run)(); } cases[] = {{"grant", grant}, {"titles", titles}, {"security", security}, {"lifecycle", lifecycle}, {"session-core", sessionCore}, {"session-unsent", sessionUnsent},
                                                                {"session-limits", sessionLimits}, {"storage", storage}, {"listener", listener}, {"probes", probes}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"print-v1\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"spooler\":\"substituted\",\"printersChanged\":false}\n";
  WSACleanup();
  return failed ? 1 : 0;
}
