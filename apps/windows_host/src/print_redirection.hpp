#pragma once
// print-v1 (docs/remote-session-protocol.md "Printing"): streaming printer
// redirection, like RDP Easy Print / Horizon integrated printing, without any
// new driver. Rules:
// - Only a session.start carrying the broker's grant (`printProtocol` and the
//   Host's own `printUrl`) does anything. The grant is the administrator's
//   per-user x VM `printers` permission (the RDP one), checked by the broker
//   when the session starts and continuously by the relay; a revocation closes
//   this session's print channel, and the Host then removes its printer.
// - While the channel is open the service (LocalSystem) keeps one printer queue,
//   "Multitap 프린터 (PC)": the in-box "Microsoft Print To PDF" driver (v4,
//   signed, part of Windows) on an in-box "Standard TCP/IP Port" (RAW) pointed
//   at 127.0.0.1:<port>, where this service listens on a fresh loopback port
//   bound with SO_EXCLUSIVEADDRUSE. Nothing is installed: when the driver or the
//   port monitor is missing the capability is simply not advertised.
// - Only the session's Windows user may print to it (printer DACL: that user
//   and their AppContainer apps Print; SYSTEM all; Administrators manage only,
//   no Print; nobody else). A connection to the loopback port is accepted only
//   from the Print Spooler service process (or its PrintIsolationHost), found by
//   the connection's owning process; anything else is closed unread.
// - Each spooler connection is one document (the rendered PDF). It is kept in
//   a delete-on-close file under ProgramData\Multitap\Host\private\print
//   (SYSTEM/Administrators only, links refused, final path verified), at most
//   256 MiB per job and 8 jobs / 512 MiB waiting, then offered to the client in
//   128 KiB chunks with an acknowledgement window and a SHA-256 the relay
//   verifies. A document that cannot be kept is drained (so the VM queue
//   completes) and reported to the client with a fixed reason.
// - The printer and its port are removed when the channel or the session ends,
//   and any left by a crash are removed at the next service start and before
//   the next install (identified by this product's port and exact driver;
//   every other printer, port, driver and setting is untouched).
// - Diagnostics: hostAction 242 with fixed stage codes and counts only, never a
//   document title, name or content.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "machine.hpp"
#include "operational_log.hpp"
#include "print_default.hpp"
#include "channel_redial.hpp"
#include <winspool.h>
#include <tcpxcv.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace host {
inline constexpr const char* printProtocolName = "print-v1";
inline constexpr uint64_t printMaxJobBytes = 256ull * 1024 * 1024;
inline constexpr size_t printMaxQueuedJobs = 8;
inline constexpr uint64_t printMaxQueuedBytes = 512ull * 1024 * 1024;
inline constexpr size_t printMaxTitleUnits = 200;

// hostAction 242 stages (fixed codes; `expected` carries a count). Not 240,
// which agent.hpp mediaDropped uses.
enum class PrintStage : uint32_t {
  installed = 1, removed = 2, received = 3, delivered = 4, tooLarge = 5, storageFull = 6, peerRejected = 7, cancelled = 8, leftovers = 9,
  driverMissing = 11, nameConflict = 13, listenerFailed = 14, portFailed = 15, printerFailed = 16, securityFailed = 17,
  userUnavailable = 18, storageUnavailable = 19, spoolerUnavailable = 20,
  resultPrinted = 21, resultSaved = 22, resultCancelled = 23, resultFailed = 24, channelClosed = 25, readFailed = 26,
  defaultClaimed = 27, defaultRestored = 28, defaultKept = 29, defaultFailed = 30, defaultRecovered = 31 };
// cause: the Windows error behind a failure, when known (default-printer steps).
inline void recordPrintEvent(PrintStage stage, bool ok, uint32_t error = 0, uint32_t count = 0, uint32_t cause = 0) noexcept {
  if (cause) recordHostEvent("hostAction", {{"action", 242}, {"stage", int64_t(stage)}, {"ok", ok ? 1 : 0}, {"error", int64_t(error)}, {"expected", int64_t(count)}, {"cause", int64_t(cause)}});
  else recordHostEvent("hostAction", {{"action", 242}, {"stage", int64_t(stage)}, {"ok", ok ? 1 : 0}, {"error", int64_t(error)}, {"expected", int64_t(count)}});
}
// Recover never returns claimed/restored: those belong to different helper
// operations. An unknown response cannot authorize deleting recovery evidence.
inline bool acceptPrintRecovery(uint32_t code) {
  if (code == uint32_t(DefaultPrinterResult::none)) return true;
  if (code == uint32_t(DefaultPrinterResult::kept) || code == uint32_t(DefaultPrinterResult::recovered)) {
    recordPrintEvent(PrintStage(code), true); return true;
  }
  const bool failed = code == uint32_t(DefaultPrinterResult::failed);
  recordPrintEvent(PrintStage::defaultFailed, false, failed ? printDefaultDetail().reason : uint32_t(DefaultFailure::unexpected), 0, failed ? printDefaultDetail().win32 : 0);
  return false;
}

// The outcome of preparing the printer, sent to the client as print.queue.
enum class PrintSetup { ready, driverUnavailable, spoolerUnavailable, nameConflict, failed };
inline const char* printSetupReason(PrintSetup setup) {
  switch (setup) {
    case PrintSetup::ready: return "";
    case PrintSetup::driverUnavailable: return "print_driver_unavailable";
    case PrintSetup::spoolerUnavailable: return "print_spooler_unavailable";
    case PrintSetup::nameConflict: return "print_name_conflict";
    default: return "print_setup_failed";
  }
}

// The Host's own print channel URL from a granted stream session.start, or "".
inline std::string printSessionUrl(const Json& start, const std::string& sessionId) {
  if (start.get("printProtocol").string() != printProtocolName || start.get("mode").string() != "stream" || sessionId.empty()) return {};
  const auto url = start.get("printUrl").string(), path = "/api/v1/hosts/sessions/" + sessionId + "/print";
  if (url.size() > 512 || !transport::relaySyntax(url,path)) return {};
  return url;
}

// A spooler document name for the client's dialog: control characters removed,
// at most 200 UTF-16 units, never a broken surrogate pair; "" when unusable.
inline std::string printTitle(const std::wstring& document) {
  std::wstring out;
  for (size_t at = 0; at < document.size() && out.size() < printMaxTitleUnits; ++at) {
    const wchar_t unit = document[at];
    if (unit < 32 || unit == 127) continue;
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (at + 1 < document.size() && document[at + 1] >= 0xdc00 && document[at + 1] <= 0xdfff && out.size() + 2 <= printMaxTitleUnits) { out.push_back(unit); out.push_back(document[++at]); }
      else ++at;  // an unpaired or cut pair is dropped
      continue;
    }
    if (unit >= 0xdc00 && unit <= 0xdfff) continue;
    out.push_back(unit);
  }
  try { return utf8(out); } catch (...) { return {}; }
}

inline bool printSidText(const std::wstring& sid) {
  return sid.size() >= 7 && sid.size() <= 184 && sid.rfind(L"S-1-", 0) == 0 && sid.find_first_not_of(L"S0123456789-") == std::wstring::npos;
}
// The printer's security: the session user (and that user's AppContainer apps,
// which Windows checks together with the user) may print and manage their own
// documents; SYSTEM (the spooler) everything; Administrators manage the queue
// but have no Print right; nobody else has any entry.
// LC=PRINTER_ACCESS_ADMINISTER SW=PRINTER_ACCESS_USE RP/WP=JOB_ACCESS_ADMINISTER/READ.
inline std::wstring printerSecurityDescriptor(const std::wstring& userSid) {
  if (!printSidText(userSid)) throw std::runtime_error("Invalid print user");
  return L"O:SYG:SYD:P(A;;LCSWSDRCWDWO;;;SY)(A;OIIO;RPWPSDRCWDWO;;;SY)(A;;LCSDRCWDWO;;;BA)(A;OIIO;RPWPSDRCWDWO;;;BA)"
         L"(A;;SWRC;;;" + userSid + L")(A;CIIO;GX;;;" + userSid + L")(A;;SWRC;;;AC)(A;CIIO;GX;;;AC)(A;OIIO;RPWPSDRCWDWO;;;CO)";
}
// --- Spooler access (substituted by tests) ---------------------------------
struct PrintPrinterEntry { std::wstring name, port, driver; };
struct PrintSpoolerAccess {
  std::function<bool()> spoolerRunning, driverAvailable, monitorAvailable;
  std::function<std::vector<PrintPrinterEntry>()> printers;  // throws when unreadable
  std::function<std::vector<std::wstring>()> ports;          // throws when unreadable
  std::function<DWORD(const std::wstring& port, uint16_t tcpPort)> addPort;
  std::function<DWORD(const std::wstring& port)> deletePort;
  std::function<DWORD(const std::wstring& name, const std::wstring& port, const std::wstring& sddl)> addPrinter;
  std::function<DWORD(const std::wstring& name)> deletePrinter;
  // A queue that could not be deleted: paused, emptied and moved to the "nul:" port, so that it never
  // stays bound to this session's TCP port after the listener closed.
  // ERROR_SUCCESS once it is off our port. Empty in some tests.
  std::function<DWORD(const std::wstring& name)> quarantinePrinter;
  std::function<void()> retryWait;   // between deletion attempts (production: 200 ms)
  std::function<std::wstring(const std::wstring& name)> printingTitle;
  // Device nodes of our queues the spooler no longer has; empty in tests.
  std::function<uint32_t()> removeStaleDevices;
};

// The one printer queue of a session and its port.
class PrintQueueKeeper {
 public:
  explicit PrintQueueKeeper(PrintSpoolerAccess access) : access_(std::move(access)) {}
  // Printers on this product's ports and unused ports of this product, left by
  // an earlier run. Returns how many were removed.
  uint32_t removeLeftovers() noexcept {
    uint32_t removed = 0;
    try {
      for (const auto& printer : access_.printers()) if ((printOwnedBinding({printer.port, printer.driver}) || printQuarantined(printer)) && access_.deletePrinter(printer.name) == ERROR_SUCCESS) ++removed;
      const auto printers = access_.printers();
      for (const auto& port : access_.ports()) {
        if (!printOwnedPort(port) || std::any_of(printers.begin(), printers.end(), [&](const auto& printer) { return samePrintText(printer.port, port); })) continue;
        if (access_.deletePort(port) == ERROR_SUCCESS) ++removed;
      }
      if (access_.removeStaleDevices) removed += access_.removeStaleDevices();
    } catch (...) { }
    return removed;
  }
  PrintSetup install(const std::wstring& userSid, uint16_t tcpPort, const std::function<bool()>& recoverDefault = {}) {
    try {
      if (!access_.spoolerRunning()) { recordPrintEvent(PrintStage::spoolerUnavailable, false); return PrintSetup::spoolerUnavailable; }
      if (!access_.driverAvailable() || !access_.monitorAvailable()) { recordPrintEvent(PrintStage::driverMissing, false); return PrintSetup::driverUnavailable; }
      if (!printSidText(userSid) || !tcpPort) return PrintSetup::failed;
      // Preserve the old queue/record if the session user's recovery failed.
      if (recoverDefault && !recoverDefault()) return PrintSetup::failed;
      if (const auto removed = removeLeftovers()) recordPrintEvent(PrintStage::leftovers, true, 0, removed);
      // Any same-name queue not removed as a proven legacy binding is kept.
      for (const auto& printer : access_.printers()) if (samePrintText(printer.name, printQueueName)) { recordPrintEvent(PrintStage::nameConflict, false); return PrintSetup::nameConflict; }
      const auto port = std::wstring(printPortPrefix) + std::to_wstring(tcpPort);
      if (const DWORD error = access_.addPort(port, tcpPort)) { recordPrintEvent(PrintStage::portFailed, false, error); return PrintSetup::failed; }
      port_ = port;
      if (const DWORD error = access_.addPrinter(printQueueName, port_, printerSecurityDescriptor(userSid))) {
        recordPrintEvent(error == ERROR_ACCESS_DENIED ? PrintStage::securityFailed : PrintStage::printerFailed, false, error);
        (void)access_.deletePort(port_); port_.clear(); return PrintSetup::failed;
      }
      recordPrintEvent(PrintStage::installed, true);
      return PrintSetup::ready;
    } catch (...) { remove(); return PrintSetup::failed; }
  }
  // A queue of ours that an earlier removal could not delete and moved to "nul:".
  static bool printQuarantined(const PrintPrinterEntry& printer) {
    return (samePrintText(printer.name, printQueueName) || samePrintText(printer.name, printLegacyQueueName)) &&
           samePrintText(printer.driver, printDriverName) && samePrintText(printer.port, L"nul:");
  }
  // Removes this session's printer (its pending documents are purged) and port. A queue that cannot
  // be deleted is tried again, then quarantined (paused, emptied, moved to "nul:"): the listener of
  // this port is about to close, and a queue left on it would send documents to whatever program
  // binds 127.0.0.1:<port> next. The next session's removeLeftovers deletes it.
  void remove() noexcept {
    if (port_.empty()) return;
    bool ok = true;
    try {
      for (const auto& printer : access_.printers()) {
        if (!samePrintText(printer.port, port_) || !printOwnedBinding({printer.port, printer.driver})) continue;
        DWORD error = ERROR_GEN_FAILURE;
        for (int attempt = 0; attempt < 3 && error != ERROR_SUCCESS; ++attempt) { error = access_.deletePrinter(printer.name); if (error != ERROR_SUCCESS && attempt < 2 && access_.retryWait) access_.retryWait(); }
        if (error == ERROR_SUCCESS) continue;
        ok = false;
        const DWORD quarantined = access_.quarantinePrinter ? access_.quarantinePrinter(printer.name) : ERROR_NOT_SUPPORTED;
        recordPrintEvent(PrintStage::removed, quarantined == ERROR_SUCCESS, error, 0, quarantined == ERROR_SUCCESS ? 2 : 3);
      }
    } catch (...) { ok = false; }
    try {
      const auto remaining = access_.printers();
      if (std::any_of(remaining.begin(), remaining.end(), [&](const auto& printer) { return samePrintText(printer.port, port_); })) ok = false;
      else ok = access_.deletePort(port_) == ERROR_SUCCESS && ok;
    } catch (...) { ok = false; }
    recordPrintEvent(PrintStage::removed, ok);
    port_.clear();
  }
  bool installed() const { return !port_.empty(); }
  const std::wstring& port() const { return port_; }
 private:
  PrintSpoolerAccess access_;
  std::wstring port_;
};

// --- Windows spooler ---------------------------------------------------------
inline DWORD printSpoolerProcessId() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!manager) return 0;
  DWORD pid = 0;
  if (SC_HANDLE service = OpenServiceW(manager, L"Spooler", SERVICE_QUERY_STATUS)) {
    SERVICE_STATUS_PROCESS status{}; DWORD needed = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) && status.dwCurrentState == SERVICE_RUNNING) pid = status.dwProcessId;
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager); return pid;
}
template <class Enumerate> inline std::vector<BYTE> printEnumeration(Enumerate enumerate, DWORD& count) {
  DWORD needed = 0; count = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::vector<BYTE> buffer(needed);
    if (enumerate(buffer.empty() ? nullptr : buffer.data(), DWORD(buffer.size()), &needed, &count)) return buffer;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed || needed > 16 * 1024 * 1024) break;
  }
  throw std::runtime_error("print_enumeration_failed");
}
inline bool printDriverInstalled() {
  try {
    DWORD count = 0;
    const auto buffer = printEnumeration([](BYTE* data, DWORD size, DWORD* needed, DWORD* found) { return EnumPrinterDriversW(nullptr, nullptr, 1, data, size, needed, found); }, count);
    const auto* drivers = reinterpret_cast<const DRIVER_INFO_1W*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) if (drivers[i].pName && samePrintText(drivers[i].pName, printDriverName)) return true;
  } catch (...) { }
  return false;
}
inline bool printMonitorInstalled() {
  try {
    DWORD count = 0;
    const auto buffer = printEnumeration([](BYTE* data, DWORD size, DWORD* needed, DWORD* found) { return EnumMonitorsW(nullptr, 1, data, size, needed, found); }, count);
    const auto* monitors = reinterpret_cast<const MONITOR_INFO_1W*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) if (monitors[i].pName && samePrintText(monitors[i].pName, printMonitorName)) return true;
  } catch (...) { }
  return false;
}
inline std::vector<PrintPrinterEntry> printLocalPrinters() {
  DWORD count = 0;
  const auto buffer = printEnumeration([](BYTE* data, DWORD size, DWORD* needed, DWORD* found) { return EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2, data, size, needed, found); }, count);
  const auto* printers = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
  std::vector<PrintPrinterEntry> out;
  for (DWORD i = 0; i < count; ++i) if (printers[i].pPrinterName) out.push_back({printers[i].pPrinterName, printers[i].pPortName ? printers[i].pPortName : L"", !(printers[i].Attributes & PRINTER_ATTRIBUTE_NETWORK) && printers[i].pDriverName ? printers[i].pDriverName : L""});
  return out;
}
inline std::vector<std::wstring> printLocalPorts() {
  DWORD count = 0;
  const auto buffer = printEnumeration([](BYTE* data, DWORD size, DWORD* needed, DWORD* found) { return EnumPortsW(nullptr, 1, data, size, needed, found); }, count);
  const auto* ports = reinterpret_cast<const PORT_INFO_1W*>(buffer.data());
  std::vector<std::wstring> out;
  for (DWORD i = 0; i < count; ++i) if (ports[i].pName) out.emplace_back(ports[i].pName);
  return out;
}
inline DWORD printTcpPortCommand(const wchar_t* command, const BYTE* data, DWORD size) {
  wchar_t monitor[] = L",XcvMonitor Standard TCP/IP Port";
  PRINTER_DEFAULTSW defaults{nullptr, nullptr, SERVER_ACCESS_ADMINISTER}; HANDLE xcv = nullptr;
  if (!OpenPrinterW(monitor, &xcv, &defaults)) return GetLastError();
  DWORD needed = 0, status = ERROR_SUCCESS;
  const BOOL called = XcvDataW(xcv, command, const_cast<BYTE*>(data), size, nullptr, 0, &needed, &status);
  const DWORD error = called ? status : GetLastError();
  ClosePrinter(xcv); return error;
}
// A RAW (9100-style) port to 127.0.0.1:tcpPort without SNMP.
inline DWORD printAddTcpPort(const std::wstring& name, uint16_t tcpPort) {
  if (name.empty() || name.size() >= MAX_PORTNAME_LEN) return ERROR_INVALID_PARAMETER;
  PORT_DATA_1 data{};
  wcsncpy_s(data.sztPortName, name.c_str(), _TRUNCATE); wcscpy_s(data.sztHostAddress, L"127.0.0.1"); wcscpy_s(data.sztIPAddress, L"127.0.0.1");
  data.dwVersion = 1; data.dwProtocol = PROTOCOL_RAWTCP_TYPE; data.cbSize = sizeof(data); data.dwPortNumber = tcpPort; data.dwSNMPEnabled = 0; data.dwDoubleSpool = 0;
  return printTcpPortCommand(L"AddPort", reinterpret_cast<const BYTE*>(&data), sizeof(data));
}
inline DWORD printDeleteTcpPort(const std::wstring& name) {
  if (name.empty() || name.size() >= MAX_PORTNAME_LEN) return ERROR_INVALID_PARAMETER;
  DELETE_PORT_DATA_1 data{}; wcsncpy_s(data.psztPortName, name.c_str(), _TRUNCATE); data.dwVersion = 1;
  const DWORD error = printTcpPortCommand(L"DeletePort", reinterpret_cast<const BYTE*>(&data), sizeof(data));
  return error == ERROR_UNKNOWN_PORT ? ERROR_SUCCESS : error;
}
// True when every entry granting Print belongs to SYSTEM, [user] or ALL
// APPLICATION PACKAGES (checked after creation: the queue must never be
// usable by another account, even for a moment after a refused descriptor).
inline bool printQueueSecured(HANDLE printer, const std::wstring& userSid) {
  DWORD needed = 0; GetPrinterW(printer, 3, nullptr, 0, &needed);
  if (!needed || needed > 1024 * 1024) return false;
  std::vector<BYTE> buffer(needed);
  if (!GetPrinterW(printer, 3, buffer.data(), needed, &needed)) return false;
  const auto* info = reinterpret_cast<const PRINTER_INFO_3*>(buffer.data());
  PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  if (!info->pSecurityDescriptor || !GetSecurityDescriptorDacl(info->pSecurityDescriptor, &present, &dacl, &defaulted) || !present || !dacl) return false;
  PSID user = nullptr, packages = nullptr;
  if (!ConvertStringSidToSidW(userSid.c_str(), &user)) return false;
  struct FreeSid { PSID& value; ~FreeSid() { if (value) LocalFree(value); } } freeUser{user}, freePackages{packages};
  if (!ConvertStringSidToSidW(L"S-1-15-2-1", &packages)) return false;
  bool userCanPrint = false;
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* raw = nullptr; if (!GetAce(dacl, i, &raw)) return false;
    const auto* header = static_cast<const ACE_HEADER*>(raw);
    if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    if (!(ace->Mask & (PRINTER_ACCESS_USE | GENERIC_ALL | GENERIC_EXECUTE))) continue;
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    if (EqualSid(sid, user)) { userCanPrint = true; continue; }
    if (IsWellKnownSid(sid, WinLocalSystemSid) || EqualSid(sid, packages)) continue;
    return false;
  }
  return userCanPrint;
}
inline DWORD printAddQueue(const std::wstring& name, const std::wstring& port, const std::wstring& sddl) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
  struct FreeDescriptor { PSECURITY_DESCRIPTOR value; ~FreeDescriptor() { LocalFree(value); } } freeDescriptor{descriptor};
  const auto userStart = sddl.find(L"(A;;SWRC;;;") + 11, userEnd = sddl.find(L')', userStart);
  const auto userSid = sddl.substr(userStart, userEnd - userStart);
  std::wstring printerName = name, portName = port, driver = printDriverName, processor = L"winprint", datatype = L"RAW",
      // Settings > Printers shows it to the session's person: their language (the service is SYSTEM).
      comment = ui_text::get(ui_text::Id::PrinterComment, multitap::ui::sidLanguage(userSid));
  PRINTER_INFO_2W info{};
  info.pPrinterName = printerName.data(); info.pPortName = portName.data(); info.pDriverName = driver.data(); info.pPrintProcessor = processor.data();
  info.pDatatype = datatype.data(); info.pComment = comment.data(); info.pSecurityDescriptor = descriptor;
  info.Attributes = PRINTER_ATTRIBUTE_LOCAL | PRINTER_ATTRIBUTE_QUEUED; info.Priority = 1; info.DefaultPriority = 1;
  HANDLE printer = AddPrinterW(nullptr, 2, reinterpret_cast<LPBYTE>(&info));
  if (!printer) return GetLastError();
  bool secured = printQueueSecured(printer, userSid);
  if (!secured) {
    PRINTER_INFO_3 security{descriptor};
    secured = SetPrinterW(printer, 3, reinterpret_cast<LPBYTE>(&security), 0) && printQueueSecured(printer, userSid);
  }
  if (!secured) { SetPrinterW(printer, 0, nullptr, PRINTER_CONTROL_PURGE); DeletePrinter(printer); ClosePrinter(printer); return ERROR_ACCESS_DENIED; }
  ClosePrinter(printer); return ERROR_SUCCESS;
}
inline DWORD printDeleteQueue(const std::wstring& name) {
  std::wstring copy = name; PRINTER_DEFAULTSW defaults{nullptr, nullptr, PRINTER_ALL_ACCESS}; HANDLE printer = nullptr;
  if (!OpenPrinterW(copy.data(), &printer, &defaults)) { const DWORD error = GetLastError(); return error == ERROR_INVALID_PRINTER_NAME ? ERROR_SUCCESS : error; }
  SetPrinterW(printer, 0, nullptr, PRINTER_CONTROL_PURGE);
  const DWORD error = DeletePrinter(printer) ? ERROR_SUCCESS : GetLastError();
  ClosePrinter(printer); return error;
}
// A queue that could not be deleted: paused and emptied first (nothing is sent anywhere any more), then
// moved to the "nul:" port. ERROR_SUCCESS only when it is no longer on its TCP port.
inline DWORD printQuarantineQueue(const std::wstring& name) {
  std::wstring copy = name; PRINTER_DEFAULTSW defaults{nullptr, nullptr, PRINTER_ALL_ACCESS}; HANDLE printer = nullptr;
  if (!OpenPrinterW(copy.data(), &printer, &defaults)) return GetLastError();
  SetPrinterW(printer, 0, nullptr, PRINTER_CONTROL_PAUSE);
  SetPrinterW(printer, 0, nullptr, PRINTER_CONTROL_PURGE);
  DWORD error = ERROR_GEN_FAILURE, needed = 0;
  GetPrinterW(printer, 5, nullptr, 0, &needed);
  if (needed && needed < 65536) {
    std::vector<BYTE> buffer(needed);
    if (GetPrinterW(printer, 5, buffer.data(), needed, &needed)) {
      auto* info = reinterpret_cast<PRINTER_INFO_5W*>(buffer.data());
      std::wstring nul = L"nul:"; info->pPortName = nul.data();
      error = SetPrinterW(printer, 5, buffer.data(), 0) ? ERROR_SUCCESS : GetLastError();
    } else error = GetLastError();
  }
  ClosePrinter(printer); return error;
}
// The document the spooler is sending now (queued printing: one at a time).
inline std::wstring printCurrentDocument(const std::wstring& name) {
  std::wstring copy = name; PRINTER_DEFAULTSW defaults{nullptr, nullptr, PRINTER_ACCESS_USE}; HANDLE printer = nullptr;
  if (!OpenPrinterW(copy.data(), &printer, &defaults)) return {};
  std::wstring title;
  try {
    DWORD count = 0;
    const auto buffer = printEnumeration([printer](BYTE* data, DWORD size, DWORD* needed, DWORD* found) { return EnumJobsW(printer, 0, 64, 1, data, size, needed, found); }, count);
    const auto* jobs = reinterpret_cast<const JOB_INFO_1W*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) if ((jobs[i].Status & JOB_STATUS_PRINTING) && jobs[i].pDocument) { title = jobs[i].pDocument; break; }
  } catch (...) { }
  ClosePrinter(printer); return title;
}
// Windows keeps a PnP node (SWD\PRINTENUM\{GUID}, class PrintQueue) for each
// queue; a deleted queue's node can stay behind as a non-present device, and
// Settings > Printers lists it as one more "Multitap 프린터 (PC)" that is
// "연결되어 있지 않음" (in a field case two next to the live queue, one per
// earlier session). Removed only when the node is not present, enumerated by
// PRINTENUM, of class PrintQueue and named exactly like our queue; a live
// queue's node and every other device stay untouched. Returns how many went.
inline uint32_t printRemoveStaleQueueDevices() noexcept {
  static const GUID printQueueClass{0x1ed2bbf9, 0x11f0, 0x4084, {0xb2, 0x1f, 0xad, 0x83, 0xa8, 0xe6, 0xdc, 0xdc}};
  std::vector<std::wstring> stale;
  if (const HDEVINFO set = SetupDiGetClassDevsW(&printQueueClass, nullptr, nullptr, 0); set != INVALID_HANDLE_VALUE) {
    SP_DEVINFO_DATA info{sizeof(info)};
    for (DWORD i = 0; i < 4096 && SetupDiEnumDeviceInfo(set, i, &info); ++i) {
      wchar_t id[MAX_DEVICE_ID_LEN]{};
      if (CM_Get_Device_IDW(info.DevInst, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS || _wcsnicmp(id, L"SWD\\PRINTENUM\\{", 15) != 0) continue;
      DEVINST live = 0;
      if (CM_Locate_DevNodeW(&live, id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) continue;  // present: a live queue
      wchar_t name[256]{}; DWORD type = 0;
      if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, &type, reinterpret_cast<BYTE*>(name), sizeof(name) - sizeof(wchar_t), nullptr) || type != REG_SZ) continue;
      if (printQueueName == name || printLegacyQueueName == name) stale.emplace_back(id);  // also ghosts of the previous label
    }
    SetupDiDestroyDeviceInfoList(set);
  }
  uint32_t removed = 0;
  for (const auto& id : stale) {
    const HDEVINFO one = SetupDiCreateDeviceInfoList(&printQueueClass, nullptr);
    if (one == INVALID_HANDLE_VALUE) continue;
    SP_DEVINFO_DATA info{sizeof(info)};
    if (SetupDiOpenDeviceInfoW(one, id.c_str(), nullptr, 0, &info) && SetupDiCallClassInstaller(DIF_REMOVE, one, &info)) ++removed;
    SetupDiDestroyDeviceInfoList(one);
  }
  return removed;
}
inline PrintSpoolerAccess nativePrintSpoolerAccess() {
  PrintSpoolerAccess access;
  access.spoolerRunning = [] { return printSpoolerProcessId() != 0; };
  access.driverAvailable = printDriverInstalled; access.monitorAvailable = printMonitorInstalled;
  access.printers = printLocalPrinters; access.ports = printLocalPorts;
  access.addPort = printAddTcpPort; access.deletePort = printDeleteTcpPort;
  access.addPrinter = printAddQueue; access.deletePrinter = printDeleteQueue;
  access.quarantinePrinter = printQuarantineQueue; access.retryWait = [] { Sleep(200); };
  access.printingTitle = printCurrentDocument;
  access.removeStaleDevices = printRemoveStaleQueueDevices;
  return access;
}
// Advertised only when nothing would have to be installed (checked at most
// once a minute; the spooler can be stopped or the optional feature removed).
// Why printing is not offered ("" when it is): the spooler is not running, or the
// driver / port monitor the printer needs is missing. A client whose account may
// print says so instead of silently having no printer.
inline const char* printUnavailableReason(bool spooler, bool driver, bool monitor) {
  return !spooler ? "print_spooler_unavailable" : (!driver || !monitor) ? "print_driver_unavailable" : "";
}
// Set when Host settings changed something printing depends on (the spooler):
// the next read observes again instead of answering from the cache.
inline std::atomic<bool>& printRedirectionRecheck() { static std::atomic<bool> value{false}; return value; }
// The capability report's print check: the spooler's SCM status (five SCM
// calls) and its drivers and port monitors (two enumerations of two spooler
// calls each). It ran every minute. A result is now kept while the spooler
// process it found keeps running (a handle checked without waiting), at most
// ten minutes while printing is offered (a removed driver or port monitor);
// one minute while it is not, or when the spooler cannot be watched.
struct PrintCheckAccess {
  std::function<DWORD()> spooler;        // the running spooler's PID, 0 when it is not running
  std::function<HANDLE(DWORD)> watch;    // signalled when that process ends; nullptr when it cannot be opened
  std::function<bool()> driver, monitor;
};
class PrintAvailability {
 public:
  static constexpr uint64_t offeredMaxAgeMs = 600000, unofferedMaxAgeMs = 60000;
  const char* reason(uint64_t now, const PrintCheckAccess& access, bool recheck) {
    std::lock_guard lock(mutex_);
    const bool ended = spooler_.value && WaitForSingleObject(spooler_.value, 0) != WAIT_TIMEOUT;
    const uint64_t maxAge = !*reason_ && spooler_.value ? offeredMaxAgeMs : unofferedMaxAgeMs;
    if (!recheck && !ended && checked_ && now >= at_ && now - at_ < maxAge) return reason_;
    checked_ = true; at_ = now;
    const DWORD pid = access.spooler(); spooler_ = WinHandle(pid ? access.watch(pid) : nullptr);
    reason_ = printUnavailableReason(pid != 0, pid && access.driver(), pid && access.monitor());
    return reason_;
  }
 private:
  std::mutex mutex_; WinHandle spooler_; bool checked_ = false; uint64_t at_ = 0; const char* reason_ = "print_spooler_unavailable";
};
inline const char* printRedirectionState() {
  static PrintAvailability availability;
  static const PrintCheckAccess access{printSpoolerProcessId, [](DWORD pid) { return OpenProcess(SYNCHRONIZE, FALSE, pid); }, printDriverInstalled, printMonitorInstalled};
  return availability.reason(GetTickCount64(), access, printRedirectionRecheck().exchange(false));
}
inline bool printRedirectionAvailable() { return !*printRedirectionState(); }
inline std::string printCapabilityFields() {
  const char* reason = printRedirectionState();
  return *reason ? std::string(",\"printUnavailableReason\":\"") + reason + '"' : std::string(",\"printProtocol\":\"") + printProtocolName + '"';
}

// The session's Windows user (the identity allowed to print to the queue).
inline std::optional<std::wstring> printSessionUserSid(DWORD session) {
  HANDLE raw = nullptr;
  if (session == 0 || session == 0xffffffff || !WTSQueryUserToken(session, &raw)) return std::nullopt;
  WinHandle token(raw); DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  if (!size || size > 65536) return std::nullopt;
  std::vector<BYTE> data(size);
  if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size)) return std::nullopt;
  const auto sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
  if (!IsValidSid(sid) || IsWellKnownSid(sid, WinLocalSystemSid)) return std::nullopt;
  LPWSTR text = nullptr; if (!ConvertSidToStringSidW(sid, &text)) return std::nullopt;
  std::wstring value(text); LocalFree(text);
  return printSidText(value) ? std::optional(value) : std::nullopt;
}
// The process that owns the other end of an accepted loopback connection.
inline DWORD printLoopbackPeerProcess(SOCKET socket) {
  sockaddr_in peer{}, local{}; int peerSize = sizeof(peer), localSize = sizeof(local);
  if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peerSize) || getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localSize)) return 0;
  if (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 0;
  DWORD size = 0; std::vector<BYTE> data;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD result = GetExtendedTcpTable(data.empty() ? nullptr : data.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    if (result == NO_ERROR && !data.empty()) break;
    if ((result != ERROR_INSUFFICIENT_BUFFER && result != NO_ERROR) || !size || size > 16 * 1024 * 1024) return 0;
    data.assign(size + 4096, 0); size = DWORD(data.size());
  }
  if (data.empty()) return 0;
  const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(data.data());
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto& row = table->table[i];
    if (row.dwLocalAddr == peer.sin_addr.s_addr && u_short(row.dwLocalPort) == peer.sin_port &&
        row.dwRemoteAddr == local.sin_addr.s_addr && u_short(row.dwRemotePort) == local.sin_port) return row.dwOwningPid;
  }
  return 0;
}
// The Print Spooler service process, or its driver isolation host (System32,
// session 0). A user process cannot be either.
inline bool printTrustedSpooler(DWORD pid) {
  if (!pid) return false;
  if (pid == printSpoolerProcessId()) return true;
  DWORD session = 1; if (!ProcessIdToSessionId(pid, &session) || session != 0) return false;
  WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); if (!process.value) return false;
  std::wstring path(1024, L'\0'); DWORD length = DWORD(path.size());
  if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &length)) return false;
  path.resize(length);
  wchar_t system[MAX_PATH]{}; const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
  if (!systemLength || systemLength >= MAX_PATH) return false;
  return samePrintText(path, std::wstring(system) + L"\\PrintIsolationHost.exe");
}

// --- Job storage -------------------------------------------------------------
class PrintDigest {
 public:
  PrintDigest() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("print_digest_unavailable");
    DWORD size = 0, got = 0;
    if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &got, 0) < 0) { close(); throw std::runtime_error("print_digest_unavailable"); }
    object_.resize(size);
    if (BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) < 0) { close(); throw std::runtime_error("print_digest_unavailable"); }
  }
  ~PrintDigest() { close(); }
  PrintDigest(const PrintDigest&) = delete; PrintDigest& operator=(const PrintDigest&) = delete;
  void update(const uint8_t* data, size_t size) { if (size && BCryptHashData(hash_, const_cast<PUCHAR>(data), ULONG(size), 0) < 0) throw std::runtime_error("print_digest_failed"); }
  std::string finish() {
    std::array<uint8_t, 32> digest{};
    if (BCryptFinishHash(hash_, digest.data(), ULONG(digest.size()), 0) < 0) throw std::runtime_error("print_digest_failed");
    const char hex[] = "0123456789abcdef"; std::string out;
    for (const auto value : digest) { out += hex[value >> 4]; out += hex[value & 15]; }
    return out;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr; BCRYPT_HASH_HANDLE hash_ = nullptr; std::vector<uint8_t> object_;
  void close() { if (hash_) BCryptDestroyHash(hash_); hash_ = nullptr; if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr; }
};
inline std::string printBase64(const uint8_t* data, size_t size) {
  DWORD length = 0;
  if (!CryptBinaryToStringA(data, DWORD(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length)) throw std::runtime_error("print_base64_failed");
  std::string out(length, '\0');
  if (!CryptBinaryToStringA(data, DWORD(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &length)) throw std::runtime_error("print_base64_failed");
  out.resize(length); return out;
}
inline std::wstring printFinalPath(HANDLE handle) {
  std::wstring buffer(512, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length) return {};
    if (length < buffer.size()) { buffer.resize(length); return buffer; }
    buffer.resize(size_t(length) + 1);
  }
  return {};
}
inline std::filesystem::path printJobDirectory() { return machineDirectory() / L"private" / L"print"; }
// Waiting jobs, shared with the listener (which only accepts with room left).
struct PrintBudget {
  std::atomic<uint32_t> jobs{0}; std::atomic<uint64_t> bytes{0};
  bool room() const { return jobs.load() < printMaxQueuedJobs && bytes.load() < printMaxQueuedBytes; }
};
// One document: a delete-on-close file in the protected folder, written while
// the spooler sends it and read while it goes to the client. Windows deletes it
// when the last handle closes, including when this process ends abnormally.
class PrintJobFile {
 public:
  PrintJobFile(WinHandle file, std::shared_ptr<PrintBudget> budget) : file_(std::move(file)), budget_(std::move(budget)) { if (budget_) budget_->jobs++; }
  ~PrintJobFile() { if (budget_) { budget_->jobs--; budget_->bytes -= size_; } }
  PrintJobFile(const PrintJobFile&) = delete; PrintJobFile& operator=(const PrintJobFile&) = delete;
  bool append(const uint8_t* data, size_t size) {
    DWORD written = 0;
    if (!size) return true;
    if (!WriteFile(file_.value, data, DWORD(size), &written, nullptr) || written != size) return false;
    digest_.update(data, size); size_ += size; if (budget_) budget_->bytes += size;
    return true;
  }
  bool read(uint64_t offset, uint8_t* out, size_t size) const {
    if (offset > size_ || size > size_ - offset) return false;
    OVERLAPPED at{}; at.Offset = DWORD(offset); at.OffsetHigh = DWORD(offset >> 32); DWORD read = 0;
    return ReadFile(file_.value, out, DWORD(size), &read, &at) && read == size;
  }
  std::string finish() { return digest_.finish(); }
  uint64_t size() const { return size_; }
 private:
  WinHandle file_; std::shared_ptr<PrintBudget> budget_; PrintDigest digest_; uint64_t size_ = 0;
};
// The protected folder, opened as itself and held for the session (never a link,
// owned and writable only by SYSTEM/Administrators unless a test opts out).
class PrintJobStore {
 public:
  PrintJobStore(std::filesystem::path directory, std::shared_ptr<PrintBudget> budget, bool secure = true) : directory_(std::move(directory)), budget_(std::move(budget)) {
    if (!directory_.is_absolute() || !directory_.has_relative_path() || !directory_.has_filename()) throw std::runtime_error("print_storage_untrusted");
    if (secure) prepareMachineDirectory();
    // Every folder from the drive root down is opened as itself, refused when
    // it is a junction or symbolic link, and held without delete sharing, so the
    // final path below is where the documents really are. The parents are
    // checked before the documents' folder is created in them.
    auto current = directory_.root_path();
    const auto hold = [&](const std::filesystem::path& folderPath) {
      WinHandle folder(CreateFileW(folderPath.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      FILE_ATTRIBUTE_TAG_INFO info{};
      if (folder.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(folder.value, FileAttributeTagInfo, &info, sizeof(info)) ||
          !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("print_storage_untrusted");
      folders_.push_back(std::move(folder));
    };
    for (const auto& part : directory_.parent_path().relative_path()) {
      if (part.empty() || part == L"." || part == L"..") throw std::runtime_error("print_storage_untrusted");
      current /= part; hold(current);
    }
    if (secure) secureDirectory(directory_, false);
    else if (!CreateDirectoryW(directory_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) throw std::runtime_error("print_storage_unavailable");
    hold(directory_);
    if (secure && !trustedMachineFile(directory_)) throw std::runtime_error("print_storage_untrusted");
    final_ = printFinalPath(folders_.back().value);
    if (final_.empty()) throw std::runtime_error("print_storage_untrusted");
  }
  // A fresh document file, or nullptr when none can be made safely.
  std::shared_ptr<PrintJobFile> create() const {
    try {
      const auto name = L"job-" + randomName() + L".pdf";
      WinHandle file(CreateFileW((directory_ / name).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (file.value == INVALID_HANDLE_VALUE) return nullptr;
      if (!samePrintText(printFinalPath(file.value), final_ + L"\\" + name)) return nullptr;
      return std::make_shared<PrintJobFile>(std::move(file), budget_);
    } catch (...) { return nullptr; }
  }
  // Documents left by a power loss (delete-on-close never ran): regular files
  // named job-<32 hex>.pdf only.
  static uint32_t sweep(const std::filesystem::path& directory, bool requireTrusted = true) noexcept {
    uint32_t removed = 0;
    try {
      const DWORD attributes = GetFileAttributesW(directory.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || (requireTrusted && !trustedMachineFile(directory))) return 0;
      size_t seen = 0;
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (++seen > 4096) break;
        const auto name = entry.path().filename().wstring();
        if (name.size() != 40 || name.rfind(L"job-", 0) != 0 || name.substr(36) != L".pdf" || name.substr(4, 32).find_first_not_of(L"0123456789abcdef") != std::wstring::npos) continue;
        WinHandle file(CreateFileW(entry.path().c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO info{}; FILE_DISPOSITION_INFO disposition{TRUE};
        if (file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &info, sizeof(info)) &&
            !(info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) ++removed;
      }
    } catch (...) { }
    return removed;
  }
 private:
  std::filesystem::path directory_; std::shared_ptr<PrintBudget> budget_; std::vector<WinHandle> folders_; std::wstring final_;
};

// One document ready for the client.
struct PrintJob {
  std::string id, sha256, title; uint64_t size = 0;
  std::function<bool(uint64_t offset, uint8_t* out, size_t size)> read;
  std::shared_ptr<void> owner;  // keeps the file (and its budget share) alive
};

// --- Session logic (no transport, no spooler: tests drive it directly) --------
// Relay messages in, print.* messages out. One document is offered at a time:
// header, chunks while the client's acknowledgements leave room in the window,
// then the end with its SHA-256.
class PrintSessionCore {
 public:
  // false: the message could not be queued now (retried by pump()).
  using Send = std::function<bool(const std::string&)>;
  struct Hooks { std::function<PrintSetup()> install; std::function<void()> remove; };
  PrintSessionCore(std::string sessionId, Send send, Hooks hooks) : sessionId_(std::move(sessionId)), send_(std::move(send)), hooks_(std::move(hooks)) {}
  ~PrintSessionCore() { closed(); }
  // A relay message. false: the channel must close.
  bool message(const Json& value) {
    const auto type = value.get("type").string();
    if (type == "print.error" || type == "print.channel.closed") return false;
    if (type == "print.ready") {
      if (ready_ || value.get("sessionId").string() != sessionId_ || value.get("protocol").string() != printProtocolName) return false;
      const auto& limits = value.get("limits");
      const auto integer = [](double number, double low, double high) { return std::isfinite(number) && std::floor(number) == number && number >= low && number <= high; };
      const double chunk = limits.get("chunkBytes").number(-1), window = limits.get("windowChunks").number(-1), maximum = limits.get("maxJobBytes").number(-1);
      if (!integer(chunk, 1024, 131072) || !integer(window, 1, 64) || !integer(maximum, 1, 1ull << 40)) return false;
      chunkBytes_ = size_t(chunk); windowChunks_ = size_t(window); maxJobBytes_ = std::min<uint64_t>(uint64_t(maximum), printMaxJobBytes); ready_ = true;
      const auto setup = hooks_.install ? hooks_.install() : PrintSetup::failed;
      installed_ = setup == PrintSetup::ready;
      const std::string queue = installed_ ? "{\"type\":\"print.queue\",\"state\":\"ready\"}"
                                           : std::string("{\"type\":\"print.queue\",\"state\":\"unavailable\",\"reason\":\"") + printSetupReason(setup) + "\"}";
      (void)send_(queue);
      return true;
    }
    if (!ready_) return false;
    const auto id = value.get("jobId").string();
    if (type == "print.ack") {
      const double offset = value.get("offset").number(-1);
      if (active_ && id == active_->id && std::isfinite(offset) && std::floor(offset) == offset && offset > double(acked_) && offset <= double(sent_)) acked_ = uint64_t(offset);
      return true;
    }
    if (type == "print.cancel") {
      if (active_ && id == active_->id) { recordPrintEvent(PrintStage::cancelled, true); active_.reset(); }
      return true;
    }
    if (type == "print.result") {
      const auto result = value.get("result").string();
      const auto stage = result == "printed" ? PrintStage::resultPrinted : result == "saved" ? PrintStage::resultSaved : result == "cancelled" ? PrintStage::resultCancelled : PrintStage::resultFailed;
      recordPrintEvent(stage, stage == PrintStage::resultPrinted || stage == PrintStage::resultSaved);
      return true;
    }
    return true;  // nothing else is relayed to the Host
  }
  // A document from the spooler (dropped when the queue is not installed).
  void offer(PrintJob job) {
    if (!installed_) return;
    if (job.size > maxJobBytes_) { skipped("print_job_too_large"); return; }
    queue_.push_back(std::move(job));
  }
  // A document the Host received but cannot offer (fixed reason).
  void skipped(const char* reason) {
    if (!installed_) return;
    pendingNotices_.push_back(std::string("{\"type\":\"print.skipped\",\"reason\":\"") + reason + "\"}");
  }
  // Sends what the window and the channel allow now.
  void pump() {
    if (!ready_ || !installed_) return;
    while (!pendingNotices_.empty()) { if (!send_(pendingNotices_.front())) return; pendingNotices_.pop_front(); }
    std::vector<uint8_t> buffer;
    for (;;) {
      if (!active_) {
        if (queue_.empty()) return;
        active_ = std::move(queue_.front()); queue_.pop_front(); sent_ = acked_ = 0; headerSent_ = false;
      }
      if (!headerSent_) {
        if (!send_("{\"type\":\"print.job\",\"jobId\":" + jsonQuote(active_->id) + ",\"format\":\"pdf\",\"size\":" + std::to_string(active_->size) + ",\"title\":" + jsonQuote(active_->title) + '}')) return;
        headerSent_ = true;
      }
      while (sent_ < active_->size) {
        const size_t count = size_t(std::min<uint64_t>(chunkBytes_, active_->size - sent_));
        if (sent_ + count - acked_ > uint64_t(chunkBytes_) * windowChunks_) return;  // waiting for acknowledgements
        buffer.resize(count);
        std::string data;
        try { if (!active_->read(sent_, buffer.data(), count)) throw std::runtime_error("read"); data = printBase64(buffer.data(), count); }
        catch (...) { recordPrintEvent(PrintStage::readFailed, false); (void)send_("{\"type\":\"print.cancel\",\"jobId\":" + jsonQuote(active_->id) + '}'); active_.reset(); break; }
        if (!send_("{\"type\":\"print.chunk\",\"jobId\":" + jsonQuote(active_->id) + ",\"offset\":" + std::to_string(sent_) + ",\"data\":\"" + data + "\"}")) return;
        sent_ += count;
      }
      if (!active_) continue;
      if (!send_("{\"type\":\"print.end\",\"jobId\":" + jsonQuote(active_->id) + ",\"sha256\":\"" + active_->sha256 + "\"}")) return;
      recordPrintEvent(PrintStage::delivered, true);
      active_.reset();
    }
  }
  // The channel ended (revocation, session end, failure): no printer remains.
  void closed() {
    if (installed_ && hooks_.remove) hooks_.remove();
    if (ready_) recordPrintEvent(PrintStage::channelClosed, true, 0, uint32_t(queue_.size() + (active_ ? 1 : 0)));
    installed_ = false; ready_ = false; active_.reset(); queue_.clear(); pendingNotices_.clear();
  }
  bool installed() const { return installed_; }
  bool busy() const { return active_.has_value() || !queue_.empty() || !pendingNotices_.empty(); }
  // channel-resume-v1: documents the client never heard of (waiting, or taken up but not announced yet).
  // The ones it heard of it counts itself when the pair ends.
  size_t unsent() const { return queue_.size() + (active_ && !headerSent_ ? 1 : 0); }
 private:
  std::string sessionId_; Send send_; Hooks hooks_;
  bool ready_ = false, installed_ = false, headerSent_ = false;
  size_t chunkBytes_ = 131072, windowChunks_ = 8; uint64_t maxJobBytes_ = printMaxJobBytes;
  std::optional<PrintJob> active_; std::deque<PrintJob> queue_; std::deque<std::string> pendingNotices_;
  uint64_t sent_ = 0, acked_ = 0;
};

// --- Loopback listener (the spooler's RAW port) -------------------------------
class PrintListener {
 public:
  struct Access {
    std::function<bool(SOCKET)> trusted = [](SOCKET socket) { return printTrustedSpooler(printLoopbackPeerProcess(socket)); };
    std::function<std::string()> title;
    std::function<std::shared_ptr<PrintJobFile>()> file;
  };
  using Deliver = std::function<void(PrintJob)>;
  using Skip = std::function<void(const char*)>;
  ~PrintListener() { stop(); }
  // Listens on 127.0.0.1 at a free port; returns it, or 0.
  uint16_t start(Access access, Deliver deliver, Skip skip, std::shared_ptr<PrintBudget> budget) {
    stop();
    SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT | WSA_FLAG_OVERLAPPED);
    if (listener == INVALID_SOCKET) return 0;
    BOOL exclusive = TRUE; sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
    int size = sizeof(address);
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) ||
        bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 4) ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) || !address.sin_port) { closesocket(listener); return 0; }
    access_ = std::move(access); deliver_ = std::move(deliver); skip_ = std::move(skip); budget_ = std::move(budget);
    listener_ = listener; stopped_ = false;
    thread_ = std::thread([this] { run(); });
    return ntohs(address.sin_port);
  }
  void stop() {
    stopped_ = true;
    { std::lock_guard lock(clientMutex_); if (client_ != INVALID_SOCKET) shutdown(client_, SD_BOTH); }
    if (thread_.joinable()) thread_.join();
    if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
  }
 private:
  Access access_; Deliver deliver_; Skip skip_; std::shared_ptr<PrintBudget> budget_;
  SOCKET listener_ = INVALID_SOCKET; std::mutex clientMutex_; SOCKET client_ = INVALID_SOCKET; std::atomic<bool> stopped_{true}; std::thread thread_;
  // Any local program can connect to the port; a refused peer is recorded at most once a minute,
  // with how many were refused quietly since (`expected`).
  uint64_t lastRejectRecord_ = 0; uint32_t quietRejects_ = 0;
 public:
  static constexpr uint64_t rejectRecordIntervalMs = 60000;
  // Records a refused peer unless one was recorded within the interval. True when recorded.
  bool noteRejected(uint64_t now = GetTickCount64()) noexcept {
    if (lastRejectRecord_ && now >= lastRejectRecord_ && now - lastRejectRecord_ < rejectRecordIntervalMs) { if (quietRejects_ < UINT32_MAX) ++quietRejects_; return false; }
    recordPrintEvent(PrintStage::peerRejected, false, 0, quietRejects_);
    lastRejectRecord_ = now ? now : 1; quietRejects_ = 0; return true;
  }
 private:
  void run() {
    while (!stopped_) {
      // No room: leave the spooler's connection waiting (its document stays
      // queued in the VM) instead of accepting what cannot be kept.
      if (budget_ && !budget_->room()) { Sleep(100); continue; }
      fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable); timeval wait{0, 200000};
      const int ready = select(0, &readable, nullptr, nullptr, &wait);
      if (ready < 0) break;
      if (!ready || stopped_) continue;
      const SOCKET client = accept(listener_, nullptr, nullptr);
      if (client == INVALID_SOCKET) continue;
      { std::lock_guard lock(clientMutex_); client_ = client; }
      if (!stopped_) try { receive(client); } catch (...) { }
      { std::lock_guard lock(clientMutex_); client_ = INVALID_SOCKET; }
      shutdown(client, SD_BOTH); closesocket(client);
    }
  }
  void receive(SOCKET client) {
    if (!access_.trusted || !access_.trusted(client)) { noteRejected(); return; }
    DWORD timeout = 120000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const auto title = access_.title ? access_.title() : std::string();
    auto file = access_.file ? access_.file() : nullptr;
    const bool storage = file != nullptr;
    bool tooLarge = false; uint64_t total = 0; std::vector<uint8_t> buffer(65536);
    for (;;) {
      const int count = recv(client, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
      if (count == 0) break;                    // the spooler finished this document
      if (count < 0 || stopped_) {              // failed or stopping: nothing is offered
        // The spooler stopped sending mid-document for two minutes: the user is told, not left
        // waiting for a page that never comes. A reset (the job was
        // cancelled in the VM) stays quiet.
        if (count < 0 && !stopped_ && total && WSAGetLastError() == WSAETIMEDOUT) { recordPrintEvent(PrintStage::readFailed, false); skip_("print_job_failed"); }
        return;
      }
      total += uint64_t(count);
      if (file && total > printMaxJobBytes) { file.reset(); tooLarge = true; }
      if (file && !file->append(buffer.data(), size_t(count))) { file.reset(); }
    }
    if (!total) return;
    if (tooLarge) { recordPrintEvent(PrintStage::tooLarge, false); skip_("print_job_too_large"); return; }
    if (!file) { recordPrintEvent(storage ? PrintStage::storageFull : PrintStage::storageUnavailable, false); skip_("print_storage_unavailable"); return; }
    PrintJob job; job.id = utf8(randomName()); job.size = file->size(); job.sha256 = file->finish(); job.title = title;
    job.read = [file](uint64_t offset, uint8_t* out, size_t size) { return file->read(offset, out, size); };
    job.owner = file;
    recordPrintEvent(PrintStage::received, true);
    deliver_(std::move(job));
  }
};

// --- Relay channel (Host side of the session's print socket pair) -------------
class PrintChannel {
 public:
  using Receive = std::function<void(const std::string&)>;
  static constexpr size_t maximumSend = 200000, maximumQueue = 3 * 1024 * 1024;
  ~PrintChannel() { stop(); }
  void stop() { std::lock_guard lifecycle(lifecycle_); close(); if (thread_.joinable()) thread_.join(); receive_ = {}; }
  // Queues one message; false (nothing queued) when the queue is full now.
  bool trySend(const std::string& json) {
    { std::lock_guard lock(mutex_); if (stopped_ || json.size() > maximumSend || bytes_ + json.size() > maximumQueue || queue_.size() >= 64) return false; queue_.push_back(json); bytes_ += json.size(); }
    condition_.notify_one(); return true;
  }
  void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, Receive receive) {
    std::lock_guard lifecycle(lifecycle_); stop();
    if(!credentialOrigin(credentials).relayMatches(url,"/api/v1/hosts/sessions/"+sessionId+"/print"))throw std::runtime_error("Invalid print relay URL");
    receive_ = std::move(receive); stopped_ = false; status_ = 0;
    thread_ = std::thread([this, credentials, sessionId] {
      std::thread sender;
      try {
        const auto origin=verifiedCredentialOrigin(credentials); Internet session(httpSession()), connection(WinHttpConnect(session.handle, origin.hostname.c_str(), origin.port, 0));
        const auto path = L"/api/v1/hosts/sessions/" + wide(sessionId) + L"/print";
        Internet request(WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, origin.flags()));
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)), "Print relay redirects");
        winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0), "Print relay WebSocket");
        const auto authorization = L"Authorization: Host " + wide(credentials.token) + L"\r\n";
        winCheck(WinHttpSendRequest(request.handle, authorization.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "Print relay connect");
        winCheck(WinHttpReceiveResponse(request.handle, nullptr), "Print relay response");
        DWORD status = 0, length = sizeof(status);
        winCheck(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX), "Print relay status");
        status_ = status;
        if (status != 101) throw std::runtime_error("Print relay unavailable");
        const auto web = WinHttpWebSocketCompleteUpgrade(request.handle, 0); if (!web) winCheck(FALSE, "Print relay upgrade");
        if (stopped_) { WinHttpCloseHandle(web); throw std::runtime_error("Print relay stopped"); }
        socket_ = web;
        sender = std::thread([this] {
          try {
            while (!stopped_) {
              std::string json;
              { std::unique_lock lock(mutex_); condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); }); if (stopped_) break; json = std::move(queue_.front()); queue_.pop_front(); bytes_ -= json.size(); }
              const auto socket = socket_.load();
              if (!socket || WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, json.data(), DWORD(json.size())) != NO_ERROR) break;
            }
          } catch (...) { }
          close();
        });
        std::array<char, 16384> buffer{}; std::string json;
        while (!stopped_) {
          DWORD count = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{}; const auto socket = socket_.load();
          if (!socket || WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &count, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
          if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) throw std::runtime_error("Non-JSON print relay payload");
          if (json.size() + count > 65000) throw std::runtime_error("Print relay message exceeds bounds");
          json.append(buffer.data(), count);
          if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            if (JsonParser::parse(json).get("type").string().rfind("print.", 0) != 0) throw std::runtime_error("Unexpected print relay message");
            if (receive_) receive_(json);
            json.clear();
          }
        }
      } catch (...) { }
      close(); if (sender.joinable()) sender.join();
      if (receive_) try { receive_("{\"type\":\"print.channel.closed\"}"); } catch (...) { }
    });
  }
  // channel-resume-v1: the Broker answered the last dial with a refusal (not "not now", not a lost network).
  bool refused() const { return channelDialRefused(status_.load()); }
 private:
  std::atomic<DWORD> status_{0};
  std::atomic<bool> stopped_{true}; std::atomic<HINTERNET> socket_{nullptr}; std::thread thread_; Receive receive_;
  std::mutex mutex_; std::recursive_mutex lifecycle_; std::condition_variable condition_; std::deque<std::string> queue_; size_t bytes_ = 0;
  void close() { stopped_ = true; if (const auto socket = socket_.exchange(nullptr)) WinHttpCloseHandle(socket); { std::lock_guard lock(mutex_); queue_.clear(); bytes_ = 0; } condition_.notify_all(); }
};

// --- One stream session's printer redirection ---------------------------------
class PrintRedirection {
 public:
  struct Options {
    PrintSpoolerAccess spooler = nativePrintSpoolerAccess();
    std::function<std::optional<std::wstring>(DWORD)> userSid = printSessionUserSid;
    std::filesystem::path storage = printJobDirectory();
    bool secureStorage = true;
    // Claim / release / recover the default printer as the session user; a DefaultPrinterResult code.
    std::function<uint32_t(DWORD session, const char* op)> defaultPrinter = runPrintDefault;
    std::function<bool(SOCKET)> trusted = [](SOCKET socket) { return printTrustedSpooler(printLoopbackPeerProcess(socket)); };
  };
  ~PrintRedirection() { stop(); }
  void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, DWORD windowsSession, Options options = {}) {
    std::lock_guard lifecycle(lifecycle_);
    stop();
    { std::lock_guard lock(mutex_); inbox_.clear(); arrivals_.clear(); skips_.clear(); closed_ = false; }
    credentials_ = credentials; url_ = url; if (!redialling_) carried_ = 0;
    options_ = std::move(options); sessionId_ = sessionId; windowsSession_ = windowsSession; stopped_ = false;
    channel_.start(credentials, sessionId, url, [this](const std::string& json) {
      Json value; try { value = JsonParser::parse(json); } catch (...) { value = JsonParser::parse("{\"type\":\"print.error\"}"); }
      { std::lock_guard lock(mutex_); if (value.get("type").string() == "print.channel.closed") closed_ = true; else if (inbox_.size() < 256) inbox_.push_back(std::move(value)); else closed_ = true; }
      wake_.notify_all();
    });
    controller_ = std::thread([this] { run(); });
  }
  void stop() {
    std::lock_guard lifecycle(lifecycle_);
    stopped_ = true; wake_.notify_all();
    channel_.stop();
    if (controller_.joinable()) controller_.join();
  }
  bool active() const { return !stopped_; }
  // channel-resume-v1 (channel_redial.hpp): a fresh pair for the same session after the channel ended for a
  // transport reason. The printer went with the ended pair (and the user's default printer was given back);
  // the new pair makes it again. Documents the client never heard of were dropped with the pair: the new
  // pair says so once (print.skipped), so nothing is lost silently.
  void redial() {
    std::lock_guard lifecycle(lifecycle_);
    if (url_.empty()) throw std::runtime_error("print_not_started");
    const auto credentials = credentials_; const auto sessionId = sessionId_; const auto url = url_; auto options = options_;
    redialling_ = true;
    try { start(credentials, sessionId, url, windowsSession_, std::move(options)); } catch (...) { redialling_ = false; throw; }
    redialling_ = false;
  }
  // Told when the channel ended by itself (never by stop()). `transient`: the same session may pair again.
  // Called on the controller thread after it let everything go; it must not block.
  void onEnded(std::function<void(bool transient)> hook) { std::lock_guard lock(mutex_); ended_ = std::move(hook); }
 private:
  Credentials credentials_; std::string url_; std::function<void(bool)> ended_; bool redialling_ = false; std::atomic<size_t> carried_{0};
  std::recursive_mutex lifecycle_;
  Options options_; std::string sessionId_; DWORD windowsSession_ = 0xffffffff;
  PrintChannel channel_; PrintListener listener_; std::thread controller_;
  std::mutex mutex_; std::condition_variable wake_; std::deque<Json> inbox_; std::deque<PrintJob> arrivals_; std::deque<const char*> skips_;
  bool closed_ = false; std::atomic<bool> stopped_{true};
  static void defaultResult(uint32_t code) {
    if (code >= uint32_t(PrintStage::defaultClaimed) && code <= uint32_t(PrintStage::defaultRecovered)) {
      const bool failed = code == uint32_t(PrintStage::defaultFailed);
      // A failure carries its reason (DefaultFailure) and Windows error.
      recordPrintEvent(PrintStage(code), !failed, failed ? printDefaultDetail().reason : 0, 0, failed ? printDefaultDetail().win32 : 0);
    }
  }
  void run() {
    auto budget = std::make_shared<PrintBudget>();
    PrintQueueKeeper keeper(options_.spooler);
    std::unique_ptr<PrintJobStore> store;
    PrintSessionCore::Hooks hooks;
    hooks.install = [&]() -> PrintSetup {
      const auto sid = options_.userSid ? options_.userSid(windowsSession_) : std::nullopt;
      if (!sid) { recordPrintEvent(PrintStage::userUnavailable, false); return PrintSetup::failed; }
      try { store = std::make_unique<PrintJobStore>(options_.storage, budget, options_.secureStorage); }
      catch (...) { recordPrintEvent(PrintStage::storageUnavailable, false); return PrintSetup::failed; }
      PrintListener::Access access;
      access.trusted = options_.trusted;
      access.title = [this] { try { return printTitle(options_.spooler.printingTitle ? options_.spooler.printingTitle(printQueueName) : std::wstring()); } catch (...) { return std::string(); } };
      access.file = [&store] { return store ? store->create() : nullptr; };
      const auto port = listener_.start(std::move(access),
          [this](PrintJob job) { { std::lock_guard lock(mutex_); arrivals_.push_back(std::move(job)); } wake_.notify_all(); },
          [this](const char* reason) { { std::lock_guard lock(mutex_); if (skips_.size() < 16) skips_.push_back(reason); } wake_.notify_all(); }, budget);
      if (!port) { recordPrintEvent(PrintStage::listenerFailed, false, DWORD(WSAGetLastError())); return PrintSetup::failed; }
      const auto setup = keeper.install(*sid, port, [&] {
        const auto result = options_.defaultPrinter ? options_.defaultPrinter(windowsSession_, "recover") : 0;
        return acceptPrintRecovery(result);
      });
      if (setup != PrintSetup::ready) listener_.stop();
      else defaultResult(options_.defaultPrinter ? options_.defaultPrinter(windowsSession_, "claim") : 0);
      return setup;
    };
    // Try release while ownership is still observable. Revocation still removes
    // the queue if restoration fails; a later recovery cannot invent ownership
    // after that removal and may conservatively keep the user's later default.
    hooks.remove = [&] { defaultResult(options_.defaultPrinter ? options_.defaultPrinter(windowsSession_, "release") : 0); keeper.remove(); listener_.stop(); };
    PrintSessionCore core(sessionId_, [this](const std::string& message) { return channel_.trySend(message); }, std::move(hooks));
    // channel-resume-v1: why the pair ended. The Broker's own reason (print.error), a violation seen here, or
    // neither (the transport just ended).
    std::string endReason; bool errored = false, violated = false;
    for (;;) {
      std::deque<Json> messages; std::deque<PrintJob> jobs; std::deque<const char*> skips; bool closed = false;
      {
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(core.busy() ? 20 : 500), [this] { return stopped_ || closed_ || !inbox_.empty() || !arrivals_.empty() || !skips_.empty(); });
        messages.swap(inbox_); jobs.swap(arrivals_); skips.swap(skips_); closed = closed_ || stopped_;
      }
      for (auto& message : messages) if (!closed) {
        if (message.get("type").string() == "print.error") { errored = true; endReason = message.get("reason").string(); }
        if (!core.message(message)) { closed = true; if (!errored) violated = true; }
      }
      if (closed) break;
      // Documents dropped with the pair this one replaces, which the client never heard of: said once.
      if (carried_ && core.installed()) { carried_ = 0; core.skipped("print_job_failed"); }
      for (auto& job : jobs) core.offer(std::move(job));
      for (const auto* reason : skips) core.skipped(reason);
      core.pump();
    }
    const bool own = stopped_;
    if (!own) carried_ += core.unsent();
    core.closed();
    listener_.stop();
    { std::lock_guard lock(mutex_); arrivals_.clear(); skips_.clear(); inbox_.clear(); }
    store.reset();
    channel_.stop();
    // An end of its own (not stop()) is reported once everything is let go: a refused dial, a violation or a
    // reason of the pair's own is final.
    std::function<void(bool)> hook;
    if (!own) { std::lock_guard lock(mutex_); hook = ended_; }
    if (hook) hook(!violated && !channel_.refused() && (!errored || channelEndTransient("print", endReason)));
  }
};

// Removes printers, ports and documents left by a run that ended abnormally.
// Waits (bounded) for the spooler, which may start after this service.
inline void removePrintLeftovers(HANDLE stopping) noexcept {
  try {
    for (int attempt = 0; attempt < 45 && !printSpoolerProcessId(); ++attempt) if (WaitForSingleObject(stopping, 2000) == WAIT_OBJECT_0) return;
    if (!printSpoolerProcessId()) return;
    PrintQueueKeeper keeper(nativePrintSpoolerAccess());
    const auto removed = keeper.removeLeftovers() + PrintJobStore::sweep(printJobDirectory());
    // A default printer left pointing at our gone queue by a crash, for the signed-in user.
    if (const DWORD session = WTSGetActiveConsoleSessionId(); session && session != 0xffffffff) {
      const auto code = runPrintDefault(session, "recover");
      if (code >= uint32_t(PrintStage::defaultClaimed) && code <= uint32_t(PrintStage::defaultRecovered)) {
        const bool failed = code == uint32_t(PrintStage::defaultFailed);
        recordPrintEvent(PrintStage(code), !failed, failed ? printDefaultDetail().reason : 0, 0, failed ? printDefaultDetail().win32 : 0);
      }
    }
    if (removed) recordPrintEvent(PrintStage::leftovers, true, 0, removed);
  } catch (...) { }
}
class PrintLeftoverSweep {
 public:
  ~PrintLeftoverSweep() { if (thread_.joinable()) thread_.join(); }
  void start(HANDLE stopEvent) {
    HANDLE copy = nullptr;
    if (!stopEvent || !DuplicateHandle(GetCurrentProcess(), stopEvent, GetCurrentProcess(), &copy, SYNCHRONIZE, FALSE, 0)) return;
    stop_ = WinHandle(copy);
    thread_ = std::thread([this] { removePrintLeftovers(stop_.value); });
  }
 private:
  WinHandle stop_; std::thread thread_;
};
}  // namespace host
