#pragma once
// print-v1: while a granted streaming session has the "Multitap 프린터 (PC)"
// queue, it is the session user's default printer in the VM, so printing goes to
// the user's PC without picking it.
//  * The default printer is a per-user setting (HKCU), so it is read and set
//    only by `MultitapHost.exe --print-default`, started by the service with the
//    session user's own token (the drive-v1 helper pattern), never as SYSTEM.
//  * Claim (queue ready): the current default is saved in a per-user recovery
//    record, then the queue becomes the default. Release (session or grant
//    ends, before the queue is removed): the saved default returns only when the
//    default is still our queue; a default the user picked meanwhile is left
//    alone. Recover (next session start and Host start): a record left by a
//    crash is settled only while the claimed queue binding is still known.
//    A missing queue cannot identify which later default Windows/the user chose.
//  * Nothing else is changed: "Windows가 기본 프린터 관리"
//    (LegacyDefaultPrinterMode) stays as the user has it.
//  * The record is opened with the same link-hardened file code as the display
//    scale record (DisplayScaleRecordFile). Diagnostics are fixed codes only.
#include "display_scale.hpp"
#include "drive_mount.hpp"
#include "print_queue_identity.hpp"
#include <winspool.h>
#pragma comment(lib, "winspool.lib")

namespace host {
// Fixed result codes (the same numbers as PrintStage in hostAction 240).
enum class DefaultPrinterResult : uint32_t { none = 0, claimed = 27, restored = 28, kept = 29, failed = 30, recovered = 31 };

// Why a step failed (hostAction 242 stage 30 error), so a field report can tell
// a helper that never ran from a record that could not be kept from a refused
// SetDefaultPrinter. 1..2 and 8 are set by the service when the helper does not
// answer; 3..7 by the helper's keeper.
enum class DefaultFailure : uint32_t { none = 0, noSession = 1, helperFailed = 2, recordUnreadable = 3, recordNotWritten = 4, setRefused = 5, restoreRefused = 6, unexpected = 7, helperNotStarted = 8 };

struct DefaultPrinterAccess {
  std::function<uint32_t()> lastError;                    // the Win32 error of the last failed set (optional)
  std::function<std::wstring()> current;                  // "" when there is no default
  std::function<bool(const std::wstring&)> set, exists;
  std::function<std::optional<PrintQueueBinding>(const std::wstring&)> binding;  // current local queue's actual driver and port
  std::function<std::optional<std::string>()> load;       // "" no record; nullopt unreadable
  std::function<bool(const std::string&)> save;           // "" removes the record
};

class DefaultPrinterKeeper {
 public:
  explicit DefaultPrinterKeeper(DefaultPrinterAccess access) : access_(std::move(access)) {}
  DefaultPrinterResult claim() noexcept {
    failure_ = DefaultFailure::none; win32_ = 0;
    try {
      const auto record = access_.load();
      if (!record) return fail(DefaultFailure::recordUnreadable);
      if (!record->empty() && settle(*record, true) == DefaultPrinterResult::failed) return DefaultPrinterResult::failed;
      const auto binding = access_.binding ? access_.binding(printQueueName) : std::nullopt;
      if (!binding || !printOwnedBinding(*binding)) return fail(DefaultFailure::unexpected);
      const auto now = access_.current();
      const std::wstring previous = samePrintText(now, printQueueName) ? std::wstring() : now;
      if (!access_.save(recordText(previous, binding->port))) return fail(DefaultFailure::recordNotWritten);
      const auto current = access_.binding(printQueueName);
      if (!current || !printOwnedBinding(*current) || !samePrintText(current->port, binding->port) || !samePrintText(access_.current(), now)) { (void)access_.save(""); return fail(DefaultFailure::unexpected); }
      if (!access_.set(printQueueName)) { const auto error = access_.lastError ? access_.lastError() : 0; (void)access_.save(""); return fail(DefaultFailure::setRefused, error); }
      return DefaultPrinterResult::claimed;
    } catch (...) { return fail(DefaultFailure::unexpected); }
  }
  // Why the last claim / release / recover failed (none after a success).
  DefaultFailure failure() const { return failure_; }
  uint32_t win32() const { return win32_; }
  // Session end / revocation, with the queue still present.
  DefaultPrinterResult release() noexcept { return finish(false); }
  // A record left by a crash, with the queue possibly gone.
  DefaultPrinterResult recover() noexcept { return finish(true); }
 private:
  DefaultPrinterAccess access_; DefaultFailure failure_ = DefaultFailure::none; uint32_t win32_ = 0;
  DefaultPrinterResult fail(DefaultFailure why, uint32_t error = 0) { failure_ = why; win32_ = error; return DefaultPrinterResult::failed; }
  static std::string recordText(const std::wstring& previous, const std::wstring& port) {
    return "{\"v\":2,\"queue\":" + jsonQuote(utf8(printQueueName)) + ",\"port\":" + jsonQuote(utf8(port)) + ",\"previous\":" + jsonQuote(utf8(previous)) + "}";
  }
  DefaultPrinterResult finish(bool crashed) noexcept {
    failure_ = DefaultFailure::none; win32_ = 0;
    try {
      const auto record = access_.load();
      if (!record) return fail(DefaultFailure::recordUnreadable);
      if (record->empty()) return DefaultPrinterResult::none;
      return settle(*record, crashed);
    } catch (...) { return fail(DefaultFailure::unexpected); }
  }
  DefaultPrinterResult settle(const std::string& record, bool crashed) {
    std::wstring previous, queue, port;
    try {
      if (record.size() > 4096) throw std::runtime_error("size");
      const auto json = JsonParser::parse(record);
      previous = wide(json.get("previous").string());
      if (json.get("v").number(0) == 1) queue = printLegacyQueueName;  // never reinterpret old records as the renamed queue
      else if (json.get("v").number(0) == 2) {
        queue = wide(json.get("queue").string()); port = wide(json.get("port").string());
        // A record of the previous label still restores the default.
        if (!(samePrintText(queue, printQueueName) || samePrintText(queue, printLegacyQueueName)) || !printOwnedPort(port)) throw std::runtime_error("binding");
      } else throw std::runtime_error("v");
    }
    catch (...) { return access_.save("") ? DefaultPrinterResult::kept : fail(DefaultFailure::recordNotWritten); }  // unusable record: drop it
    if (previous.size() > 512) previous.clear();
    const auto now = access_.current();
    const auto binding = samePrintText(now, queue) && access_.binding ? access_.binding(queue) : std::nullopt;
    const bool ours = binding && printOwnedBinding(*binding) && (port.empty() || samePrintText(port, binding->port));
    auto result = crashed ? DefaultPrinterResult::recovered : DefaultPrinterResult::restored;
    if (!ours || previous.empty() || !access_.exists(previous)) result = DefaultPrinterResult::kept;
    else if (!samePrintText(now, previous)) {
      const auto current = access_.binding(queue);
      if (!current || !printOwnedBinding(*current) || !samePrintText(current->port, binding->port) || !samePrintText(access_.current(), now)) result = DefaultPrinterResult::kept;
      else if (!access_.set(previous)) return fail(DefaultFailure::restoreRefused, access_.lastError ? access_.lastError() : 0);  // record kept for the next start
    }
    return access_.save("") ? result : fail(DefaultFailure::recordNotWritten);
  }
};

// --- Windows (runs as the session user) -----------------------------------------
inline std::filesystem::path printDefaultRecordPath() { return knownFolder(FOLDERID_LocalAppData) / L"Multitap" / L"Host" / L"print-default.state"; }
inline DWORD& lastSetError() { static thread_local DWORD error = 0; return error; }
inline DefaultPrinterAccess nativeDefaultPrinterAccess() {
  DefaultPrinterAccess access;
  access.current = [] {
    DWORD size = 0; GetDefaultPrinterW(nullptr, &size);
    if (!size || size > 4096) return std::wstring();
    std::wstring name(size, L'\0');
    if (!GetDefaultPrinterW(name.data(), &size) || !size) return std::wstring();
    name.resize(size - 1); return name;
  };
  access.set = [](const std::wstring& name) { const bool ok = SetDefaultPrinterW(name.c_str()) != FALSE; lastSetError() = ok ? 0 : GetLastError(); return ok; };
  access.lastError = [] { return uint32_t(lastSetError()); };
  access.exists = [](const std::wstring& name) {
    std::wstring copy = name; PRINTER_DEFAULTSW defaults{nullptr, nullptr, PRINTER_ACCESS_USE}; HANDLE printer = nullptr;
    if (!OpenPrinterW(copy.data(), &printer, &defaults)) return false;
    ClosePrinter(printer); return true;
  };
  access.binding = [](const std::wstring& name) -> std::optional<PrintQueueBinding> {
    std::wstring copy = name; PRINTER_DEFAULTSW defaults{nullptr, nullptr, PRINTER_ACCESS_USE}; HANDLE printer = nullptr;
    if (!OpenPrinterW(copy.data(), &printer, &defaults)) return std::nullopt;
    struct Close { HANDLE value; ~Close() { ClosePrinter(value); } } close{printer};
    DWORD needed = 0; GetPrinterW(printer, 2, nullptr, 0, &needed);
    if (!needed || needed > 1024 * 1024) return std::nullopt;
    std::vector<BYTE> buffer(needed);
    if (!GetPrinterW(printer, 2, buffer.data(), needed, &needed)) return std::nullopt;
    const auto* info = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
    if (!info->pPrinterName || !samePrintText(info->pPrinterName, name) || !info->pPortName || !info->pDriverName || (info->Attributes & PRINTER_ATTRIBUTE_NETWORK)) return std::nullopt;
    return PrintQueueBinding{info->pPortName, info->pDriverName};
  };
  access.load = []() -> std::optional<std::string> { try { return DisplayScaleRecordFile(printDefaultRecordPath()).load(); } catch (...) { return std::nullopt; } };
  access.save = [](const std::string& text) { try { return DisplayScaleRecordFile(printDefaultRecordPath()).save(text); } catch (...) { return false; } };
  return access;
}
// `MultitapHost.exe --print-default`: {"op":"claim"|"release"|"recover"} on stdin,
// {"code":N} on stdout.
inline int printDefaultHelper() {
  try {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) return 2;
    std::string bytes; char buffer[1024]; DWORD count = 0;
    while (ReadFile(input, buffer, sizeof(buffer), &count, nullptr) && count) { bytes.append(buffer, count); if (bytes.size() > 1024) return 2; }
    const auto op = JsonParser::parse(bytes).get("op").string();
    DefaultPrinterKeeper keeper(nativeDefaultPrinterAccess());
    const auto result = op == "claim" ? keeper.claim() : op == "release" ? keeper.release() : op == "recover" ? keeper.recover() : DefaultPrinterResult::failed;
    const auto answer = "{\"code\":" + std::to_string(uint32_t(result)) + ",\"reason\":" + std::to_string(uint32_t(keeper.failure())) + ",\"win32\":" + std::to_string(keeper.win32()) + "}\n";
    DWORD written = 0; return WriteFile(output, answer.data(), DWORD(answer.size()), &written, nullptr) && written == answer.size() ? 0 : 1;
  } catch (...) { return 1; }
}
// Service side: runs one operation as the user of `session`; the result code.
// The reason of a failure (DefaultFailure) and its Windows error are left in
// `printDefaultDetail()` of the calling thread, for the diagnostics record.
struct PrintDefaultDetail { uint32_t reason = 0, win32 = 0; };
inline PrintDefaultDetail& printDefaultDetail() { static thread_local PrintDefaultDetail detail; return detail; }
inline uint32_t runPrintDefault(DWORD session, const char* op) noexcept {
  auto& detail = printDefaultDetail(); detail = {};
  try {
    const auto answer = drive::runDriveMountHelper(session, std::string("{\"op\":\"") + op + "\"}", 15000, L"--print-default");
    detail.reason = uint32_t(answer.get("reason").number(0)); detail.win32 = uint32_t(answer.get("win32").number(0));
    return uint32_t(answer.get("code").number(30));
  } catch (const std::exception& failure) {
    const std::string what = failure.what();
    detail.reason = uint32_t(what == "no_interactive_session" ? DefaultFailure::noSession : what == "drive_helper_failed" ? DefaultFailure::helperFailed : DefaultFailure::helperNotStarted);
    return uint32_t(DefaultPrinterResult::failed);
  } catch (...) { detail.reason = uint32_t(DefaultFailure::helperNotStarted); return uint32_t(DefaultPrinterResult::failed); }
}}  // namespace host
