#pragma once
// WinFsp backend, service <-> user-session helper wire (docs/drive-redirection.md,
// "WinFsp backend"). The file system must run in the user's session with the
// user's token (so the drive letter belongs to that logon session only), while
// the drive-v1 relay channel lives in the Session-0 service. The helper
// (`MultitapHost.exe --drive-fs`) therefore forwards every file-system callback
// as a drive-v1 `Call` to the service over two anonymous pipes, and the service
// answers with the relay's `Result`. This header holds the pure framing; it
// has no WinFsp or process dependency, so both ends and the tests share it.
//
//   frame   = [u32 length][u8 kind][u32 id][u32 headerLength][header JSON][data]
//             (length counts everything after itself; id 0 for control frames)
//   kind 1  call     helper -> service   header = Call fields, data = write bytes
//   kind 2  result   service -> helper   header = Result fields, data = read bytes
//   kind 3  control  both ways           header = {"type":...}, no data
#include "drive_dav.hpp"

namespace host::drive::fs {
// The shared folder's volume: its owner (the Windows user the helper runs as),
// SYSTEM and Administrators may use it; nobody else, even with the UNC name.
// The owner is named by account, never by group, so the volume serves a
// standard user as it serves an administrator. "" (no owner) has no text: the
// file system then answers every security query with access denied.
inline std::wstring volumeSecurityText(const std::wstring& ownerSid) {
  if (ownerSid.empty()) return {};
  return L"O:" + ownerSid + L"G:" + ownerSid + L"D:P(A;;FA;;;" + ownerSid + L")(A;;FA;;;SY)(A;;FA;;;BA)";
}
}
namespace host::drive::wire {
inline constexpr size_t maxFrame = 2 * 1024 * 1024, maxHeader = 128 * 1024;
inline constexpr uint64_t maxFileSize = 9007199254740991ULL;
inline constexpr size_t maxQueuedCalls = 128;
enum class Kind : uint8_t { call = 1, result = 2, control = 3 };
struct Frame { Kind kind = Kind::control; uint32_t id = 0; std::string header; std::vector<uint8_t> data; };

inline const char* opName(Op op) {
  static const char* names[] = {"stat", "list", "read", "write", "create", "rename", "delete", "touch", "setsize", "flush"};
  return names[int(op)];
}
inline std::optional<Op> opFromName(const std::string& name) {
  for (int i = 0; i <= int(Op::flush); ++i) if (name == opName(Op(i))) return Op(i);
  return std::nullopt;
}

inline std::vector<uint8_t> encode(const Frame& frame) {
  const size_t inner = 1 + 4 + 4 + frame.header.size() + frame.data.size();
  std::vector<uint8_t> out(4 + inner);
  const auto put32 = [&](size_t at, uint32_t value) { for (int i = 0; i < 4; ++i) out[at + i] = uint8_t(value >> (8 * i)); };
  put32(0, uint32_t(inner)); out[4] = uint8_t(frame.kind); put32(5, frame.id); put32(9, uint32_t(frame.header.size()));
  std::copy(frame.header.begin(), frame.header.end(), out.begin() + 13);
  std::copy(frame.data.begin(), frame.data.end(), out.begin() + 13 + frame.header.size());
  return out;
}
// One frame from `body` (everything after the length field); false when malformed.
inline bool decode(const uint8_t* body, size_t size, Frame& out) {
  if (size < 9) return false;
  const auto get32 = [&](size_t at) { return uint32_t(body[at]) | uint32_t(body[at + 1]) << 8 | uint32_t(body[at + 2]) << 16 | uint32_t(body[at + 3]) << 24; };
  if (body[0] < 1 || body[0] > 3) return false;
  const uint32_t header = get32(5);
  if (header > maxHeader || size < 9 + size_t(header)) return false;
  out.kind = Kind(body[0]); out.id = get32(1);
  out.header.assign(reinterpret_cast<const char*>(body + 9), header);
  out.data.assign(body + 9 + header, body + size);
  return true;
}
// Blocking whole-frame reader over any byte source (`read` returns the bytes
// read, 0 at end/error). Returns false at end of stream or on a malformed frame.
template <class Read>
bool readFrame(Read&& read, Frame& out) {
  const auto exact = [&](uint8_t* buffer, size_t size) { size_t done = 0; while (done < size) { const size_t count = read(buffer + done, size - done); if (!count) return false; done += count; } return true; };
  uint8_t prefix[4];
  if (!exact(prefix, 4)) return false;
  const uint32_t length = uint32_t(prefix[0]) | uint32_t(prefix[1]) << 8 | uint32_t(prefix[2]) << 16 | uint32_t(prefix[3]) << 24;
  if (length < 9 || length > maxFrame) return false;
  std::vector<uint8_t> body(length);
  if (!exact(body.data(), length)) return false;
  return decode(body.data(), body.size(), out);
}

// ---- entries: [name,dir,size,modified,created,readOnly,hasSpace,free,total] ----
inline std::string entryJson(const Entry& e) {
  std::string out = "[" + jsonQuote(e.name) + (e.dir ? ",true," : ",false,") + std::to_string(e.size) + "," + std::to_string(e.modified) + "," + std::to_string(e.created) + (e.readOnly ? ",true" : ",false");
  if (e.hasSpace) out += ",true," + std::to_string(e.free) + "," + std::to_string(e.total);
  return out + "]";
}
inline bool numberIn(const Json& value, double max, double& out) {
  const auto* number = std::get_if<double>(&value.value);
  if (!number || !std::isfinite(*number) || std::floor(*number) != *number || *number < 0 || *number > max) return false;
  out = *number; return true;
}
inline bool entryParse(const Json& value, Entry& e) {
  const auto* items = std::get_if<Json::Array>(&value.value);
  if (!items || (items->size() != 6 && items->size() != 9)) return false;
  const auto* name = std::get_if<std::string>(&(*items)[0].value); const auto* dir = std::get_if<bool>(&(*items)[1].value); const auto* readOnly = std::get_if<bool>(&(*items)[5].value);
  double size = 0, modified = 0, created = 0;
  if (!name || !dir || !readOnly || !numberIn((*items)[2], 9007199254740991.0, size) || !numberIn((*items)[3], 253402300799999.0, modified) || !numberIn((*items)[4], 253402300799999.0, created)) return false;
  e = Entry{}; e.name = *name; e.dir = *dir; e.readOnly = *readOnly; e.size = uint64_t(size); e.modified = int64_t(modified); e.created = int64_t(created);
  if (items->size() == 9) {
    double free = 0, total = 0; const auto* has = std::get_if<bool>(&(*items)[6].value);
    if (!has || !numberIn((*items)[7], 9007199254740991.0, free) || !numberIn((*items)[8], 9007199254740991.0, total)) return false;
    e.hasSpace = *has; e.free = uint64_t(free); e.total = uint64_t(total);
  }
  return true;
}

// ---- calls ----
inline Frame callFrame(uint32_t id, const Call& call) {
  Frame frame; frame.kind = Kind::call; frame.id = id;
  std::string h = std::string("{\"op\":\"") + opName(call.op) + "\",\"share\":" + jsonQuote(call.share) + ",\"path\":" + jsonQuote(call.path);
  switch (call.op) {
    case Op::list: h += ",\"cursor\":" + std::to_string(call.cursor); break;
    case Op::read: h += ",\"offset\":" + std::to_string(call.offset) + ",\"length\":" + std::to_string(call.length); break;
    case Op::write: h += ",\"offset\":" + std::to_string(call.offset) + std::string(",\"truncate\":") + (call.truncate ? "true" : "false"); break;
    case Op::create: h += std::string(",\"dir\":") + (call.dir ? "true" : "false") + ",\"truncate\":" + (call.truncate ? "true" : "false"); break;
    case Op::rename: h += ",\"to\":" + jsonQuote(call.to) + ",\"replace\":" + (call.replace ? "true" : "false"); break;
    case Op::touch: h += ",\"modified\":" + std::to_string(call.modified); break;
    case Op::setsize: h += ",\"size\":" + std::to_string(call.size); break;
    default: break;
  }
  frame.header = h + "}"; if (call.op == Op::write) frame.data = call.data;
  return frame;
}
// Strict: the service never trusts the helper's call beyond the drive-v1 rules.
inline bool callParse(const Frame& frame, Call& call) {
  if (frame.kind != Kind::call || frame.id == 0) return false;
  Json json; try { json = JsonParser::parse(frame.header); } catch (...) { return false; }
  const auto op = opFromName(json.get("op").string());
  if (!op) return false;
  call = Call{}; call.op = *op; call.share = json.get("share").string(); call.path = json.get("path").string();
  if (!shareId(call.share) || !safePath(call.path)) return false;
  double n = 0;
  switch (*op) {
    case Op::stat: break;
    case Op::list: if (!numberIn(json.get("cursor"), 4294967295.0, n)) return false; call.cursor = uint32_t(n); break;
    case Op::read: if (!numberIn(json.get("offset"), 9007199254740991.0, n)) return false; call.offset = uint64_t(n); if (!numberIn(json.get("length"), double(chunkBytes), n) || n < 1) return false; call.length = uint32_t(n); break;
    case Op::write: {
      if (call.path.empty() || frame.data.empty() || frame.data.size() > chunkBytes || !numberIn(json.get("offset"), 9007199254740991.0, n)) return false; call.offset = uint64_t(n);
      const auto* truncate = std::get_if<bool>(&json.get("truncate").value); if (!truncate || (*truncate && call.offset != 0) || frame.data.size() > maxFileSize - call.offset) return false; call.truncate = *truncate; call.data = frame.data; break; }
    case Op::create: {
      const auto* dir = std::get_if<bool>(&json.get("dir").value); const auto* truncate = std::get_if<bool>(&json.get("truncate").value);
      if (call.path.empty() || !dir || !truncate || (*dir && *truncate)) return false; call.dir = *dir; call.truncate = *truncate; break; }
    case Op::rename: {
      const auto* replace = std::get_if<bool>(&json.get("replace").value); call.to = json.get("to").string();
      if (call.path.empty() || call.to.empty() || call.to == call.path || !replace || !safePath(call.to)) return false; call.replace = *replace; break; }
    case Op::remove: if (call.path.empty()) return false; break;
    case Op::touch: if (call.path.empty() || !numberIn(json.get("modified"), 253402300799999.0, n)) return false; call.modified = int64_t(n); break;
    case Op::setsize: if (call.path.empty() || !numberIn(json.get("size"), 9007199254740991.0, n)) return false; call.size = uint64_t(n); break;
    case Op::flush: break;
  }
  if (*op != Op::write && !frame.data.empty()) return false;
  return true;
}

// ---- results ----
inline Frame resultFrame(uint32_t id, const Result& result) {
  Frame frame; frame.kind = Kind::result; frame.id = id;
  if (!result.ok()) { frame.header = "{\"error\":" + jsonQuote(result.error) + "}"; return frame; }
  std::string h = "{\"ok\":true";
  if (result.entry.name.size() || result.entry.dir || result.entry.size || result.entry.modified || result.entry.created || result.entry.hasSpace) h += ",\"entry\":" + entryJson(result.entry);
  if (!result.entries.empty()) { h += ",\"entries\":["; for (size_t i = 0; i < result.entries.size(); ++i) h += (i ? "," : "") + entryJson(result.entries[i]); h += "]"; }
  if (result.next) h += ",\"next\":" + std::to_string(*result.next);
  frame.header = h + "}"; frame.data = result.data;
  return frame;
}
inline bool resultParse(const Frame& frame, Result& result) {
  if (frame.kind != Kind::result) return false;
  Json json; try { json = JsonParser::parse(frame.header); } catch (...) { return false; }
  result = Result{};
  const auto error = json.get("error").string();
  if (!error.empty()) { result.error = error; return true; }
  if (!json.get("ok").boolean()) return false;
  if (std::holds_alternative<Json::Array>(json.get("entry").value) && !entryParse(json.get("entry"), result.entry)) return false;
  if (const auto* list = std::get_if<Json::Array>(&json.get("entries").value)) for (const auto& item : *list) { Entry e; if (!entryParse(item, e)) return false; result.entries.push_back(std::move(e)); }
  double next = 0; if (std::holds_alternative<double>(json.get("next").value)) { if (!numberIn(json.get("next"), 4294967295.0, next)) return false; result.next = uint32_t(next); }
  result.data = frame.data;
  return true;
}
inline Frame controlFrame(const std::string& json) { Frame frame; frame.kind = Kind::control; frame.header = json; return frame; }
}
