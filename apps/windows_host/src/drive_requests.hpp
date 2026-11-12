#pragma once
// drive-v1 request multiplexer (docs/drive-redirection.md): WebDAV handlers
// submit file operations; each becomes one drive.request (or a binary kind-2
// write) with a strictly increasing id, at most 8 in flight (the relay's
// bound; more submitters wait), answered by the client's drive.response or
// binary kind-1 read data. A closed channel answers drive_unavailable.
#include "drive_dav.hpp"

namespace host::drive {
inline std::string requestJson(uint32_t id, const Call& call) {
  static const char* names[] = {"stat", "list", "read", "write", "create", "rename", "delete", "touch", "setsize", "flush"};
  std::string out = "{\"type\":\"drive.request\",\"id\":" + std::to_string(id) + ",\"op\":\"" + names[int(call.op)] + "\",\"share\":" + jsonQuote(call.share) + ",\"path\":" + jsonQuote(call.path);
  switch (call.op) {
    case Op::list: out += ",\"cursor\":" + std::to_string(call.cursor); break;
    case Op::read: out += ",\"offset\":" + std::to_string(call.offset) + ",\"length\":" + std::to_string(call.length); break;
    case Op::create: out += std::string(",\"dir\":") + (call.dir ? "true" : "false") + ",\"truncate\":" + (call.truncate ? "true" : "false"); break;
    case Op::rename: out += ",\"to\":" + jsonQuote(call.to) + ",\"replace\":" + (call.replace ? "true" : "false"); break;
    case Op::touch: out += ",\"modified\":" + std::to_string(call.modified); break;
    case Op::setsize: out += ",\"size\":" + std::to_string(call.size); break;
    default: break;
  }
  return out + '}';
}
// [0x02][u32 id][u16 n][n bytes JSON header][data]
inline std::vector<uint8_t> writeFrame(uint32_t id, const Call& call) {
  const auto header = "{\"op\":\"write\",\"share\":" + jsonQuote(call.share) + ",\"path\":" + jsonQuote(call.path) + ",\"offset\":" + std::to_string(call.offset) + ",\"truncate\":" + (call.truncate ? "true" : "false") + '}';
  std::vector<uint8_t> out(7 + header.size() + call.data.size());
  out[0] = 2; for (int i = 0; i < 4; ++i) out[1 + i] = uint8_t(id >> (8 * i));
  out[5] = uint8_t(header.size()); out[6] = uint8_t(header.size() >> 8);
  std::copy(header.begin(), header.end(), out.begin() + 7);
  std::copy(call.data.begin(), call.data.end(), out.begin() + 7 + header.size());
  return out;
}
inline bool integerIn(const Json& value, double min, double max) { const auto* number = std::get_if<double>(&value.value); return number && std::isfinite(*number) && std::floor(*number) == *number && *number >= min && *number <= max; }
// An entry the client reported; false for anything outside the contract.
inline bool parseEntry(const Json& value, bool root, Entry& out) {
  const auto* fields = std::get_if<Json::Object>(&value.value); if (!fields) return false;
  for (const auto& [key, item] : *fields) { (void)item; if (key != "name" && key != "dir" && key != "size" && key != "modified" && key != "created" && key != "readOnly" && !(root && key == "space")) return false; }
  const auto* name = std::get_if<std::string>(&value.get("name").value); const auto* dir = std::get_if<bool>(&value.get("dir").value); const auto* readOnly = std::get_if<bool>(&value.get("readOnly").value);
  if (!name || !dir || !readOnly || !integerIn(value.get("size"), 0, 9007199254740991.0) || !integerIn(value.get("modified"), 0, 253402300799999.0) || !integerIn(value.get("created"), 0, 253402300799999.0)) return false;
  if (root ? !name->empty() : !safeName(*name)) return false;
  out.name = *name; out.dir = *dir; out.readOnly = *readOnly; out.size = uint64_t(value.get("size").number()); out.modified = int64_t(value.get("modified").number()); out.created = int64_t(value.get("created").number());
  if (out.dir && out.size) return false;
  if (fields->count("space")) {
    const auto& space = value.get("space"); const auto* parts = std::get_if<Json::Object>(&space.value);
    if (!parts || parts->size() != 2 || !integerIn(space.get("free"), 0, 9007199254740991.0) || !integerIn(space.get("total"), 0, 9007199254740991.0) || space.get("free").number() > space.get("total").number()) return false;
    out.hasSpace = true; out.free = uint64_t(space.get("free").number()); out.total = uint64_t(space.get("total").number());
  }
  return true;
}

class Requests final : public Backend {
 public:
  using SendText = std::function<bool(std::string)>;
  using SendBinary = std::function<bool(std::vector<uint8_t>)>;
  Requests(SendText text, SendBinary binary, std::chrono::milliseconds slotWait = std::chrono::seconds(65)) : text_(std::move(text)), binary_(std::move(binary)), slotWait_(slotWait) {}
  ~Requests() override { close(); }
  std::future<Result> submit(Call call) override {
    std::unique_lock lock(mutex_);
    if (!slots_.wait_for(lock, slotWait_, [this] { return closed_ || pending_.size() < maxInFlight; })) return readyResult("drive_timeout");
    if (closed_) return readyResult("drive_unavailable");
    if (lastId_ == 0xffffffffu) return readyResult("drive_unavailable");
    const uint32_t id = ++lastId_;
    auto& pending = pending_[id];
    pending.op = call.op; pending.length = call.length; pending.cursor = call.cursor; pending.root = call.path.empty();
    auto future = pending.promise.get_future();
    // Ids must reach the relay in order: assigned and queued under one lock.
    const bool sent = call.op == Op::write ? binary_(writeFrame(id, call)) : text_(requestJson(id, call));
    if (!sent) { Result result; result.error = "drive_unavailable"; pending.promise.set_value(std::move(result)); pending_.erase(id); slots_.notify_all(); }
    return future;
  }
  // false: a message outside the contract (the caller ends the channel).
  bool receive(const Json& value) {
    const auto id = value.get("id");
    if (value.get("type").string() != "drive.response" || !integerIn(id, 1, 4294967295.0)) return false;
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(uint32_t(id.number()));
    if (found == pending_.end()) return false;
    Result result; const auto* ok = std::get_if<bool>(&value.get("ok").value);
    if (!ok) return false;
    if (!*ok) { result.error = value.get("error").string(); if (!errorCode(result.error)) return false; }
    else {
      if (found->second.op == Op::read) return false;
      if (found->second.op == Op::stat && !parseEntry(value.get("entry"), found->second.root, result.entry)) return false;
      if (found->second.op == Op::list) {
        const auto* entries = std::get_if<Json::Array>(&value.get("entries").value);
        if (!entries || entries->size() > 256) return false;
        for (const auto& item : *entries) { Entry entry; if (!parseEntry(item, false, entry)) return false; result.entries.push_back(std::move(entry)); }
        if (std::holds_alternative<double>(value.get("next").value)) { if (!integerIn(value.get("next"), double(found->second.cursor) + 1, 4294967295.0)) return false; result.next = uint32_t(value.get("next").number()); }
      }
    }
    found->second.promise.set_value(std::move(result)); pending_.erase(found); slots_.notify_all();
    return true;
  }
  bool receiveBinary(const std::vector<uint8_t>& data) {
    if (data.size() < 5 || data[0] != 1) return false;
    const uint32_t id = uint32_t(data[1]) | uint32_t(data[2]) << 8 | uint32_t(data[3]) << 16 | uint32_t(data[4]) << 24;
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(id);
    if (found == pending_.end() || found->second.op != Op::read || data.size() - 5 > found->second.length) return false;
    Result result; result.data.assign(data.begin() + 5, data.end());
    found->second.promise.set_value(std::move(result)); pending_.erase(found); slots_.notify_all();
    return true;
  }
  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    for (auto& [id, pending] : pending_) { (void)id; Result result; result.error = "drive_unavailable"; pending.promise.set_value(std::move(result)); }
    pending_.clear(); slots_.notify_all();
  }
  size_t inFlight() { std::lock_guard lock(mutex_); return pending_.size(); }
 private:
  struct Pending { Op op = Op::stat; uint32_t length = 0, cursor = 0; bool root = false; std::promise<Result> promise; };
  SendText text_; SendBinary binary_; std::chrono::milliseconds slotWait_;
  std::mutex mutex_; std::condition_variable slots_; bool closed_ = false; uint32_t lastId_ = 0; std::map<uint32_t, Pending> pending_;
};
}
