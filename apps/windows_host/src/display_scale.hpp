#pragma once
// display-scale-v1 (docs/remote-session-protocol.md "Display scale"): a client
// that requests the remote output at its own physical pixels (a phone) keeps
// text readable by asking for a Windows display scale (per-monitor DPI) on the
// owned virtual output it streams, like Microsoft Remote Desktop's desktop
// scale factor. Rules:
// - Only a session.start carrying the granted marker changes anything, and
//   only on the single owned output that session's capture is bound to (the
//   GDI name the worker selected from owned virtual outputs). The QEMU
//   console, other products' displays and independent outputs are never
//   touched.
// - display-scale-outputs-v1 (DisplayScaleOutputs below): a session.start
//   that also carries that marker grants the same request for each owned
//   output of an independent-outputs (tracks-v1) session, each output with
//   its own keeper; without it independent outputs are never touched.
// - Windows decides what is allowed: the request is clamped to the output's
//   own maximum (DisplayConfig reports it with the current value).
// - The previous value is recorded per user before the first change and put
//   back when the session ends; a record left by a crash is restored by the
//   next stream on that monitor. No registry, policy or security setting is
//   written directly: Windows persists its own per-monitor choice.
// Windows has no public API for this. DisplayConfigGetDeviceInfo/
// SetDeviceInfo with the source DPI-scale types (-3/-4) is what Windows'
// Settings app and common tools use; a failure leaves the scale unchanged and
// is reported with a fixed code (the stream itself is never affected).
#include "support.hpp"
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

namespace host {
inline constexpr std::array<int, 12> displayScalePercents{100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};
inline constexpr const char* displayScaleProtocolName = "display-scale-v1";
inline constexpr const char* displayScaleOutputsProtocolName = "display-scale-outputs-v1";
// Result codes (relay-validated 0..5).
enum class DisplayScaleCode : uint32_t { applied = 0, outputMissing = 1, unreadable = 2, refused = 3, recordUnavailable = 4, unsupported = 5 };

inline std::optional<int> displayScaleIndex(int percent) {
  for (size_t at = 0; at < displayScalePercents.size(); ++at) if (displayScalePercents[at] == percent) return int(at);
  return std::nullopt;
}
inline std::optional<int> displayScalePercentValue(const Json& value) {
  const auto* number = std::get_if<double>(&value.value);
  if (!number || !std::isfinite(*number) || *number != std::floor(*number) || *number < 100 || *number > 500) return std::nullopt;
  const int percent = int(*number);
  if (!displayScaleIndex(percent)) return std::nullopt;
  return percent;
}
inline bool displayScaleSessionGranted(const Json& start) { return start.get("displayScaleProtocol").string() == displayScaleProtocolName; }
// An independent-outputs session.start whose broker granted both markers.
inline bool displayScaleOutputsGranted(const Json& start) {
  return displayScaleSessionGranted(start) && start.get("displayScaleOutputsProtocol").string() == displayScaleOutputsProtocolName;
}
// The percent a granted session.start asks for (nullopt: none / not granted).
inline std::optional<int> displayScaleSessionRequest(const Json& start) {
  if (!displayScaleSessionGranted(start)) return std::nullopt;
  return displayScalePercentValue(start.get("displayScale"));
}
// A runtime request ({"type":"display.scale","sessionId":...,"scale":N}).
inline std::optional<int> displayScaleMessage(const Json& message) {
  if (message.get("type").string() != "display.scale") return std::nullopt;
  return displayScalePercentValue(message.get("scale"));
}

// Windows' relative steps for one source: 0 is the recommended scale,
// [minimum, maximum] what Windows allows for the output's current size.
struct DisplayScaleRange { int minimum = 0, current = 0, maximum = 0; };
struct DisplayScaleTarget { LUID adapter{}; UINT32 source = 0; std::wstring monitor; };
inline bool validDisplayScaleRange(const DisplayScaleRange& range) {
  const int recommended = -range.minimum, last = int(displayScalePercents.size()) - 1;
  return range.minimum <= 0 && range.maximum >= 0 && recommended <= last && recommended + range.maximum <= last &&
         range.current >= range.minimum && range.current <= range.maximum;
}
inline int displayScalePercentFor(const DisplayScaleRange& range, int relative) { return displayScalePercents[size_t(-range.minimum + relative)]; }
// The relative step for [percent], clamped to what Windows allows.
inline int displayScaleRelativeFor(const DisplayScaleRange& range, int percent) {
  const int wanted = *displayScaleIndex(percent) + range.minimum;  // index - recommended
  return std::clamp(wanted, range.minimum, range.maximum);
}

// What the keeper may do; tests substitute it (no display change, no file).
struct DisplayScaleAccess {
  std::function<std::optional<DisplayScaleTarget>(const std::wstring& gdiName)> find;
  std::function<std::optional<DisplayScaleRange>(const DisplayScaleTarget&)> read;
  std::function<bool(const DisplayScaleTarget&, int relative)> write;
  // The recovery record: lines of "<relative>\t<monitor path>"; empty = none.
  std::function<std::optional<std::string>()> load;
  std::function<bool(const std::string&)> save;
};

struct DisplayScaleResult { bool ok = false; int scale = 0, maximum = 0; DisplayScaleCode code = DisplayScaleCode::unsupported; };
inline std::string displayScaleAppliedPacket(const std::string& session, const DisplayScaleResult& result) {
  return "{\"type\":\"display.scale.applied\",\"sessionId\":" + jsonQuote(session) + ",\"protocol\":\"" + displayScaleProtocolName + "\",\"ok\":" + (result.ok ? "true" : "false") +
         ",\"scale\":" + std::to_string(result.scale) + ",\"maximum\":" + std::to_string(result.maximum) + ",\"code\":" + std::to_string(uint32_t(result.code)) + '}';
}

// One worker session's scale: applied on its first stream command, kept across
// its resize restarts (Windows recomputes the recommended step for a new size,
// so the absolute percent is applied again), changed by runtime requests and
// restored once at the end.
class DisplayScaleKeeper {
 public:
  explicit DisplayScaleKeeper(DisplayScaleAccess access) : access_(std::move(access)) {}

  // Each single-output stream command of this worker (first start and every
  // resize restart). Recovers a crash record for this output first. Returns
  // the result to publish, or nullopt when the session was not granted. A
  // marker-only grant observes current DPI; it does not choose a DPI value.
  std::optional<DisplayScaleResult> stream(const Json& command) {
    const auto name = command.get("displayDeviceName").string();
    if (!started_) {
      started_ = true; granted_ = displayScaleSessionGranted(command);
      if (granted_) wanted_ = displayScaleSessionRequest(command);
    }
    return bind(name.empty() ? std::wstring() : wide(name));
  }

  // One owned output of a granted independent-outputs session
  // (DisplayScaleOutputs): the session's latest request instead of a stream
  // command, then exactly like one (empty name: not a single owned output).
  DisplayScaleResult streamOutput(std::optional<int> wanted, const std::wstring& gdiName) {
    started_ = true; granted_ = true; wanted_ = wanted;
    return *bind(gdiName);
  }

  // Independent outputs (tracks-v1) without display-scale-outputs-v1 are never
  // scaled, but they are the next stream on their monitors: a leftover record
  // of any of them is put back like at an ordinary start.
  void streamOutputs(const std::vector<std::wstring>& gdiNames) {
    for (const auto& name : gdiNames) {
      const auto target = name.empty() ? std::nullopt : access_.find(name); if (!target) continue;
      if (const auto found = recorded(target->monitor)) if (access_.write(*target, found->relative)) forget(found->monitor);
    }
  }

  // A runtime request of the granted session; nullopt when not granted.
  std::optional<DisplayScaleResult> request(int percent) {
    if (!started_ || !granted_ || !displayScaleIndex(percent)) return std::nullopt;
    wanted_ = percent;
    if (device_.empty()) return DisplayScaleResult{false, 0, 0, DisplayScaleCode::unsupported};
    return apply(percent);
  }

  // End of the session: the recorded previous value goes back. True when
  // nothing was changed or the restore succeeded (the record is then gone).
  bool restore() {
    if (!original_) return true;
    const auto target = device_.empty() ? std::nullopt : access_.find(device_);
    if (!target || target->monitor != original_->monitor) return false;  // recovered by a later stream
    if (!access_.write(*target, original_->relative)) return false;
    forget(original_->monitor);
    original_.reset();
    return true;
  }

  bool changed() const { return original_.has_value(); }

 private:
  struct Original { std::wstring monitor; int relative = 0; };
  DisplayScaleAccess access_;
  bool started_ = false, granted_ = false;
  std::optional<int> wanted_;
  std::wstring device_;
  std::optional<Original> original_;
  bool recovered_ = false;

  std::optional<DisplayScaleResult> bind(std::wstring device) {
    device_ = std::move(device);
    if (device_.empty()) return granted_ ? std::optional(DisplayScaleResult{false, 0, 0, DisplayScaleCode::unsupported}) : std::nullopt;
    recover();
    if (!granted_) return std::nullopt;
    if (!wanted_) return observe();
    return apply(*wanted_);
  }

  // Read only the single owned output to which this worker's capture was
  // bound. The worker runs in the same interactive user session as DPI state.
  // No default percentage, recovery record or Windows setting is written by
  // this observation. Normal crash-record recovery still runs before it.
  DisplayScaleResult observe() {
    const auto target = access_.find(device_);
    if (!target || target->monitor.empty()) return {false, 0, 0, DisplayScaleCode::outputMissing};
    const auto range = access_.read(*target);
    if (!range || !validDisplayScaleRange(*range)) return {false, 0, 0, DisplayScaleCode::unreadable};
    return {true, displayScalePercentFor(*range, range->current),
            displayScalePercentFor(*range, range->maximum), DisplayScaleCode::applied};
  }

  static std::vector<Original> parse(const std::string& text) {
    std::vector<Original> records; size_t at = 0;
    while (at < text.size() && records.size() < 8) {
      const auto end = text.find('\n', at); const auto line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
      at = end == std::string::npos ? text.size() : end + 1;
      const auto tab = line.find('\t'); if (tab == std::string::npos || tab == 0 || tab > 3 || line.size() - tab - 1 > 512) continue;
      try {
        size_t used = 0; const int relative = std::stoi(line.substr(0, tab), &used);
        if (used != tab || relative < -11 || relative > 11) continue;
        records.push_back({wide(line.substr(tab + 1)), relative});
      } catch (...) {}
    }
    return records;
  }
  static std::string serialize(const std::vector<Original>& records) {
    std::string text; for (const auto& record : records) text += std::to_string(record.relative) + '\t' + utf8(record.monitor) + '\n';
    return text;
  }
  std::vector<Original> records() { const auto text = access_.load ? access_.load() : std::nullopt; return text ? parse(*text) : std::vector<Original>{}; }
  bool remember(const Original& value) {
    auto list = records();
    std::erase_if(list, [&](const Original& item) { return item.monitor == value.monitor; });
    list.insert(list.begin(), value); if (list.size() > 8) list.resize(8);
    return access_.save && access_.save(serialize(list));
  }
  void forget(const std::wstring& monitor) {
    auto list = records(); const auto before = list.size();
    std::erase_if(list, [&](const Original& item) { return item.monitor == monitor; });
    if (list.size() != before && access_.save) (void)access_.save(serialize(list));
  }
  // The recorded original of [monitor], if the record has one.
  std::optional<Original> recorded(const std::wstring& monitor) {
    const auto list = records();
    const auto found = std::find_if(list.begin(), list.end(), [&](const Original& item) { return item.monitor == monitor; });
    return found == list.end() ? std::nullopt : std::optional(*found);
  }
  // A record for this output left by an earlier worker (crash, killed
  // process): adopt it as this session's original, or put it back now when
  // this session asks for nothing. An output that cannot be identified yet
  // (Windows still re-laying out) is tried again on the next stream command.
  void recover() {
    if (recovered_ || original_) return;
    const auto list = records(); if (list.empty()) { recovered_ = true; return; }
    const auto target = access_.find(device_); if (!target) return;
    recovered_ = true;
    const auto found = std::find_if(list.begin(), list.end(), [&](const Original& item) { return item.monitor == target->monitor; });
    if (found == list.end()) return;
    if (granted_ && wanted_) { original_ = *found; return; }
    if (access_.write(*target, found->relative)) forget(found->monitor);
  }
  DisplayScaleResult apply(int percent) {
    const auto target = access_.find(device_);
    if (!target || target->monitor.empty()) return {false, 0, 0, DisplayScaleCode::outputMissing};
    // A different monitor than before: its record stays for recovery when
    // that monitor appears again; this one gets its own.
    if (original_ && original_->monitor != target->monitor) original_.reset();
    // A record this monitor already has (an earlier worker's, or this
    // session's before its capture moved away and back) holds the value to
    // put back: adopt it, never replace it with a step changed since.
    if (!original_) original_ = recorded(target->monitor);
    const auto range = access_.read(*target);
    if (!range || !validDisplayScaleRange(*range)) return {false, 0, 0, DisplayScaleCode::unreadable};
    const int maximum = displayScalePercentFor(*range, range->maximum);
    const int relative = displayScaleRelativeFor(*range, percent);
    if (relative == range->current) return {true, displayScalePercentFor(*range, relative), maximum, DisplayScaleCode::applied};
    // Never change what cannot be put back.
    if (!original_) {
      const Original previous{target->monitor, range->current};
      if (!remember(previous)) return {false, displayScalePercentFor(*range, range->current), maximum, DisplayScaleCode::recordUnavailable};
      original_ = previous;
    }
    if (!access_.write(*target, relative)) return {false, displayScalePercentFor(*range, range->current), maximum, DisplayScaleCode::refused};
    const auto now = access_.read(*target);
    if (!now || !validDisplayScaleRange(*now)) return {false, 0, maximum, DisplayScaleCode::unreadable};
    return {now->current == relative, displayScalePercentFor(*now, now->current), displayScalePercentFor(*now, now->maximum), now->current == relative ? DisplayScaleCode::applied : DisplayScaleCode::refused};
  }
};

// display-scale-outputs-v1: one chosen scale for every owned output of an
// independent-outputs (tracks-v1) session. Each output has its own keeper, so
// each is clamped to what Windows allows for it, records its own previous
// value (one shared record file, keyed by monitor) before its first change
// and is put back at the end or by crash recovery like a single output.
struct DisplayScaleOutput { std::string id; std::wstring gdiName; };  // empty name: not one owned output
struct DisplayScaleOutputResult { std::string id; DisplayScaleResult result; };
using DisplayScaleOutputResults = std::vector<DisplayScaleOutputResult>;
class DisplayScaleOutputs {
 public:
  explicit DisplayScaleOutputs(DisplayScaleAccess access) : access_(std::move(access)) {}

  // Each start and restart of the session's outputs, in track order (Windows
  // recommends a new step for a new size, so the percent is applied again).
  // nullopt without the grant: then only a leftover record of a streamed
  // monitor is put back, once, at the first start.
  std::optional<DisplayScaleOutputResults> stream(const Json& start, const std::vector<DisplayScaleOutput>& outputs) {
    if (!started_) {
      started_ = true; granted_ = displayScaleOutputsGranted(start);
      if (granted_) wanted_ = displayScaleSessionRequest(start);
      else {
        std::vector<std::wstring> names; for (const auto& output : outputs) names.push_back(output.gdiName);
        DisplayScaleKeeper(access_).streamOutputs(names);
      }
    }
    if (!granted_ || outputs.empty()) return std::nullopt;
    // A track session keeps its output count; a keeper is never dropped with
    // the original it holds.
    while (keepers_.size() < outputs.size()) keepers_.emplace_back(access_);
    ids_.clear();
    DisplayScaleOutputResults results;
    for (size_t at = 0; at < outputs.size(); ++at) {
      ids_.push_back(outputs[at].id);
      results.push_back({outputs[at].id, keepers_[at].streamOutput(wanted_, outputs[at].gdiName)});
    }
    return results;
  }

  // A runtime request of the granted session, for every current output.
  std::optional<DisplayScaleOutputResults> request(int percent) {
    if (!granted() || ids_.empty() || !displayScaleIndex(percent)) return std::nullopt;
    wanted_ = percent;
    DisplayScaleOutputResults results;
    for (size_t at = 0; at < ids_.size(); ++at) results.push_back({ids_[at], keepers_[at].request(percent).value_or(DisplayScaleResult{})});
    return results;
  }

  bool granted() const { return started_ && granted_; }

  // End of the session: every output's recorded value goes back. True when
  // all of them were put back (or nothing was changed).
  bool restore() {
    bool all = true;
    for (auto& keeper : keepers_) all = keeper.restore() && all;
    return all;
  }

 private:
  DisplayScaleAccess access_;
  bool started_ = false, granted_ = false;
  std::optional<int> wanted_;
  std::vector<DisplayScaleKeeper> keepers_;
  std::vector<std::string> ids_;
};
// The result of every output: display-scale-v1's fields summarise (ok only
// when every output is, the first failing output's code, output "0"'s scale
// and the largest maximum, so a step any output allows stays selectable) and
// `outputs` carries each output's own values.
inline std::string displayScaleOutputsAppliedPacket(const std::string& session, const DisplayScaleOutputResults& outputs) {
  DisplayScaleResult summary{!outputs.empty(), outputs.empty() ? 0 : outputs.front().result.scale, 0, outputs.empty() ? DisplayScaleCode::unsupported : DisplayScaleCode::applied};
  std::string list;
  for (const auto& output : outputs) {
    const auto& value = output.result;
    if (summary.ok && !value.ok) { summary.ok = false; summary.code = value.code; }
    summary.maximum = std::max(summary.maximum, value.maximum);
    list += std::string(list.empty() ? "" : ",") + "{\"id\":" + jsonQuote(output.id) + ",\"ok\":" + (value.ok ? "true" : "false") + ",\"scale\":" + std::to_string(value.scale) +
            ",\"maximum\":" + std::to_string(value.maximum) + ",\"code\":" + std::to_string(uint32_t(value.code)) + '}';
  }
  auto packet = displayScaleAppliedPacket(session, summary);
  packet.pop_back();
  return packet + ",\"outputs\":[" + list + "]}";
}

// --- Windows access ---------------------------------------------------------
struct DisplayScaleSourceGet { DISPLAYCONFIG_DEVICE_INFO_HEADER header; INT32 minimum; INT32 current; INT32 maximum; };
struct DisplayScaleSourceSet { DISPLAYCONFIG_DEVICE_INFO_HEADER header; INT32 relative; };
inline constexpr auto displayScaleGetType = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3);
inline constexpr auto displayScaleSetType = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4);

inline std::optional<DisplayScaleTarget> nativeDisplayScaleTarget(const std::wstring& gdiName) {
  UINT32 pathCount = 0, modeCount = 0; const UINT32 flags = QDC_ONLY_ACTIVE_PATHS;
  if (gdiName.empty() || GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount > 64 || modeCount > 256) return std::nullopt;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount); std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return std::nullopt;
  paths.resize(pathCount);
  std::optional<DisplayScaleTarget> found;
  for (const auto& path : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{}; source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id};
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || _wcsicmp(source.viewGdiDeviceName, gdiName.c_str()) != 0) continue;
    // A source shown on more than one target (clone) is not one owned output.
    if (found) return std::nullopt;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{}; target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.targetInfo.adapterId, path.targetInfo.id};
    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || !target.monitorDevicePath[0]) return std::nullopt;
    found = DisplayScaleTarget{path.sourceInfo.adapterId, path.sourceInfo.id, target.monitorDevicePath};
  }
  return found;
}
inline std::optional<DisplayScaleRange> nativeDisplayScaleRead(const DisplayScaleTarget& target) {
  DisplayScaleSourceGet value{}; value.header = {displayScaleGetType, sizeof(value), target.adapter, target.source};
  if (DisplayConfigGetDeviceInfo(&value.header) != ERROR_SUCCESS) return std::nullopt;
  return DisplayScaleRange{value.minimum, value.current, value.maximum};
}
inline bool nativeDisplayScaleWrite(const DisplayScaleTarget& target, int relative) {
  DisplayScaleSourceSet value{}; value.header = {displayScaleSetType, sizeof(value), target.adapter, target.source}; value.relative = relative;
  return DisplayConfigSetDeviceInfo(&value.header) == ERROR_SUCCESS;
}
// Per user (Windows keeps per-monitor scale per user): the worker runs as the
// signed-in user, so the record lives in that user's local application data.
inline std::filesystem::path displayScaleRecordPath() { return knownFolder(FOLDERID_LocalAppData) / L"Multitap" / L"Host" / L"display-scale.state"; }
// The recovery record file. The worker writes it with the user's elevated
// (linked) token into that user's own profile, which the user's unelevated
// programs can change, so nothing on the way is followed (the same rules as
// the clipboard store, windows_clipboard/storage.hpp, and the client's
// thumbnail writer): every folder from the drive root down is opened as
// itself, refused when it is a junction or symbolic link and held without
// delete sharing for the whole operation; the record and its temporary are
// opened as themselves, must be exactly where they were meant to be (final
// path), and are written, renamed and deleted by handle. Anything else
// refuses: no record, so the keeper changes nothing (recordUnavailable).
class DisplayScaleRecordFile {
 public:
  static constexpr size_t maximum = 8192;
  explicit DisplayScaleRecordFile(std::filesystem::path path) : path_(std::move(path)) {}
  // The record's text ("" when there is none); nullopt when it cannot be read
  // safely or at all.
  std::optional<std::string> load() const {
    try {
      Folders folders; const int opened = open(folders, false);
      if (opened < 0) return std::nullopt;
      if (!opened) return std::string();
      WinHandle file(CreateFileW(path_.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (file.value == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? std::optional<std::string>(std::string()) : std::nullopt;
      LARGE_INTEGER size{};
      if (!plainFile(file.value, folders.final + L'\\' + path_.filename().wstring()) || !GetFileSizeEx(file.value, &size) || size.QuadPart < 0 || size.QuadPart > LONGLONG(maximum)) return std::nullopt;
      std::string text(size_t(size.QuadPart), '\0'); DWORD read = 0;
      if (!text.empty() && (!ReadFile(file.value, text.data(), DWORD(text.size()), &read, nullptr) || read != text.size())) return std::nullopt;
      return text;
    } catch (...) { return std::nullopt; }
  }
  // Replaces the record ("" removes it). False changes nothing usable.
  bool save(const std::string& text) const {
    try {
      if (text.size() > maximum) return false;
      Folders folders; const int opened = open(folders, !text.empty());
      if (opened < 0) return false;
      if (!opened) return true;  // no folder: no record to remove
      const auto destination = folders.final + L'\\' + path_.filename().wstring();
      if (text.empty()) {
        WinHandle file(CreateFileW(path_.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
        return plainFile(file.value, destination) && deleteHandle(file.value);
      }
      const auto temporary = path_.wstring() + L".new";
      // A leftover temporary (or a link planted under its name) goes as itself.
      { WinHandle stale(CreateFileW(temporary.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (stale.value != INVALID_HANDLE_VALUE) (void)deleteHandle(stale.value); }
      WinHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (file.value == INVALID_HANDLE_VALUE) return false;
      DWORD written = 0;
      const bool saved = plainFile(file.value, destination + L".new") && WriteFile(file.value, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size() &&
                         FlushFileBuffers(file.value) && renameHandle(file.value, destination) && sameText(finalPath(file.value), destination);
      if (!saved) (void)deleteHandle(file.value);
      return saved;
    } catch (...) { return false; }
  }

 private:
  struct Folders { std::vector<WinHandle> held; std::wstring final; };
  std::filesystem::path path_;
  static bool sameText(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && (a.empty() || CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL);
  }
  // Where Windows says an open handle is (\\?\C:\...), or empty.
  static std::wstring finalPath(HANDLE handle) {
    std::wstring buffer(512, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
      const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (!length) return {};
      if (length < buffer.size()) { buffer.resize(length); return buffer; }
      buffer.resize(size_t(length) + 1);
    }
    return {};
  }
  static DWORD attributesOf(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ? info.FileAttributes : INVALID_FILE_ATTRIBUTES;
  }
  // A regular file (never a link or folder) exactly at [expected].
  static bool plainFile(HANDLE handle, const std::wstring& expected) {
    const DWORD attributes = attributesOf(handle);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && sameText(finalPath(handle), expected);
  }
  static bool deleteHandle(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
  }
  // Renames exactly the open file to the full path [path], replacing a file of
  // that name (a link there is replaced as itself, never followed).
  static bool renameHandle(HANDLE handle, const std::wstring& path) {
    std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + path.size() * sizeof(wchar_t), 0);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = TRUE; info->RootDirectory = nullptr; info->FileNameLength = DWORD(path.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, path.data(), path.size() * sizeof(wchar_t));
    return SetFileInformationByHandle(handle, FileRenameInfo, info, DWORD(buffer.size())) != FALSE;
  }
  // Every folder from the drive root to the record's folder, each opened as
  // itself and held: 1 opened, 0 one is missing (only when not creating), -1
  // refused (a link or not a folder) or failed.
  int open(Folders& folders, bool create) const {
    if (!path_.is_absolute() || !path_.has_filename() || !path_.has_parent_path()) return -1;
    const auto parent = path_.parent_path(); auto current = parent.root_path();
    for (const auto& part : parent.relative_path()) {
      if (part.empty() || part == L"." || part == L"..") return -1;
      current /= part;
      if (create && !CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
      WinHandle folder(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (folder.value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return !create && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? 0 : -1;
      }
      const DWORD attributes = attributesOf(folder.value);
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
      folders.held.push_back(std::move(folder));
    }
    if (folders.held.empty()) return -1;
    folders.final = finalPath(folders.held.back().value);
    return folders.final.empty() ? -1 : 1;
  }
};
inline DisplayScaleAccess nativeDisplayScaleAccess() {
  DisplayScaleAccess access;
  access.find = nativeDisplayScaleTarget; access.read = nativeDisplayScaleRead; access.write = nativeDisplayScaleWrite;
  access.load = []() -> std::optional<std::string> { try { return DisplayScaleRecordFile(displayScaleRecordPath()).load(); } catch (...) { return std::nullopt; } };
  access.save = [](const std::string& text) { try { return DisplayScaleRecordFile(displayScaleRecordPath()).save(text); } catch (...) { return false; } };
  return access;
}
}  // namespace host
