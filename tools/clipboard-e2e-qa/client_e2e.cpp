// Client side of the clipboard end-to-end harness: the real native clipboard adapter of the Windows client
// (apps/windows_client/windows/runner/client_clipboard.cpp, ClientClipboardChannel with its job queue, clipboard thread and
// pull events) on an in-memory clipboard. The Dart ClipboardSession (through the real NativeClipboardBridge, over a mocked
// MethodChannel) calls it by JSON lines on stdin; replies and native-to-Dart events go to stdout.
#include "client_clipboard.h"
#include "e2e_common.hpp"
#include <flutter/method_call.h>
#include <flutter/method_result.h>
#include <thread>

namespace {
using e2e::JV;
using e2e::object;
using V = flutter::EncodableValue;
using M = flutter::EncodableMap;
constexpr UINT kCommand = WM_APP + 1;

e2e::Board board;
std::unique_ptr<multitap::ClientClipboardChannel> channel;
HWND window = nullptr;
int serveDelayMs = 0;

V ToValue(const JV& json) {
  if (std::holds_alternative<std::nullptr_t>(json.v)) return V();
  if (const auto* b = std::get_if<bool>(&json.v)) return V(*b);
  if (const auto* n = std::get_if<int64_t>(&json.v)) return (*n >= INT32_MIN && *n <= INT32_MAX) ? V(int32_t(*n)) : V(*n);
  if (const auto* d = std::get_if<double>(&json.v)) return V(*d);
  if (const auto* s = std::get_if<std::string>(&json.v)) return V(*s);
  if (const auto* a = std::get_if<e2e::JArray>(&json.v)) { flutter::EncodableList list; for (const auto& item : *a) list.push_back(ToValue(item)); return V(std::move(list)); }
  const auto& o = std::get<e2e::JObject>(json.v);
  // {"$b": base64} is a byte array (Uint8List on the Dart side).
  if (o.size() == 1 && o[0].first == "$b") return V(e2e::unbase64(std::get<std::string>(o[0].second.v)));
  M map; for (const auto& item : o) map[V(item.first)] = ToValue(item.second); return V(std::move(map));
}
JV ToJson(const V& value) {
  if (std::holds_alternative<std::monostate>(value)) return nullptr;
  if (const auto* b = std::get_if<bool>(&value)) return *b;
  if (const auto* n = std::get_if<int32_t>(&value)) return int64_t(*n);
  if (const auto* n = std::get_if<int64_t>(&value)) return *n;
  if (const auto* d = std::get_if<double>(&value)) return *d;
  if (const auto* s = std::get_if<std::string>(&value)) return *s;
  if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) return object({{"$b", e2e::base64(*bytes)}});
  if (const auto* list = std::get_if<flutter::EncodableList>(&value)) { e2e::JArray a; for (const auto& item : *list) a.push_back(ToJson(item)); return JV(std::move(a)); }
  if (const auto* map = std::get_if<M>(&value)) { e2e::JObject o; for (const auto& item : *map) if (const auto* key = std::get_if<std::string>(&item.first)) o.emplace_back(*key, ToJson(item.second)); return JV(std::move(o)); }
  return nullptr;
}

class Reply : public flutter::MethodResult<V> {
 public:
  explicit Reply(int64_t id) : id_(id) {}
 protected:
  void SuccessInternal(const V* result) override { e2e::emitLine(object({{"id", id_}, {"ok", result ? ToJson(*result) : JV(nullptr)}})); }
  void ErrorInternal(const std::string& code, const std::string& message, const V*) override { e2e::emitLine(object({{"id", id_}, {"err", object({{"code", code}, {"message", message}})}})); }
  void NotImplementedInternal() override { e2e::emitLine(object({{"id", id_}, {"err", object({{"code", "not_implemented"}, {"message", ""}})}})); }
 private:
  int64_t id_;
};
void respond(int64_t id, const JV& ok) { e2e::emitLine(object({{"id", id}, {"ok", ok}})); }
void fail(int64_t id, const std::string& why) { e2e::emitLine(object({{"id", id}, {"err", object({{"code", "harness"}, {"message", why}})}})); }

void handle(const JV& command) {
  const int64_t id = command.num("id");
  try {
    if (const auto name = command.str("cmd"); !name.empty()) {
      if (e2e::handleRender(board, command, id)) return;
      if (name == "ping") respond(id, true);
      else if (name == "quit") { respond(id, true); PostQuitMessage(0); }
      else if (name == "setText") { board.userText(e2e::wide(command.str("text"))); respond(id, true); }
      else if (name == "setRich") { const auto html = command.str("html"); board.userRich(e2e::wide(command.str("text")), std::vector<uint8_t>(html.begin(), html.end())); respond(id, true); }
      else if (name == "setImage") { multitap::clipboard::Image image; image.png = e2e::unbase64(command.str("png")); image.width = uint32_t(command.num("width")); image.height = uint32_t(command.num("height")); board.userImage(image); respond(id, true); }
      else if (name == "setScreenshot") { multitap::clipboard::Image image; image.png = e2e::unbase64(command.str("png")); image.width = uint32_t(command.num("width")); image.height = uint32_t(command.num("height")); std::vector<std::filesystem::path> files; if (const auto* list = command.find("paths")) if (const auto* a = std::get_if<e2e::JArray>(&list->v)) for (const auto& item : *a) files.emplace_back(e2e::wide(std::get<std::string>(item.v))); board.userScreenshot(image, files); respond(id, true); }
        else if (name == "setFiles") { std::vector<std::filesystem::path> files; if (const auto* list = command.find("paths")) if (const auto* a = std::get_if<e2e::JArray>(&list->v)) for (const auto& item : *a) files.emplace_back(e2e::wide(std::get<std::string>(item.v))); board.userFiles(files); respond(id, true); }
      else if (name == "setVirtual") { const auto* list = command.find("entries"); board.userVirtual(list ? e2e::virtualItems(*list) : std::vector<vsrc::Item>{}); respond(id, true); }
      else if (name == "clear") { board.userClear(); respond(id, true); }
      else if (name == "renumber") { board.renumber(); respond(id, true); }
      else if (name == "makeEmptyFiles") { e2e::makeEmptyFiles(std::filesystem::path(e2e::wide(command.str("root"))), size_t(command.num("total")), size_t(command.num("perFolder"))); respond(id, true); }
      else if (name == "get") respond(id, board.describe());
        else if (name == "describePaths") { std::vector<std::filesystem::path> tops; if (const auto* list = command.find("paths")) if (const auto* a = std::get_if<e2e::JArray>(&list->v)) for (const auto& item : *a) tops.emplace_back(e2e::wide(std::get<std::string>(item.v))); respond(id, e2e::Board::describePaths(tops)); }
      else if (name == "paste") {
        IDataObject* source = board.pasteSource(); const auto destination = std::filesystem::path(e2e::wide(command.str("dest")));
        if (!source) { fail(id, "no offer on the clipboard"); return; }
        const int slowMs = int(command.num("slowMs"));
        std::thread([id, source, destination, slowMs] {
          const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
          try { const auto bytes = e2e::pasteOffer(source, destination, slowMs); respond(id, object({{"bytes", int64_t(bytes)}})); }
          catch (const std::exception& failure) { fail(id, failure.what()); }
          source->Release();
          if (com) CoUninitialize();
        }).detach();
      }
      else if (name == "makeSparse") { e2e::makeSparse(std::filesystem::path(e2e::wide(command.str("path"))), uint64_t(command.num("size")), command.str("tail")); respond(id, true); }
      else if (name == "pasteTail") {
        IDataObject* source = board.pasteSource(); if (!source) { fail(id, "no offer on the clipboard"); return; }
        const auto entry = LONG(command.num("entry")); const auto length = uint64_t(command.num("length"));
        std::thread([id, source, entry, length] {
          const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
          try { uint64_t size = 0; const auto bytes = e2e::pasteTail(source, entry, length, &size); respond(id, object({{"size", int64_t(size)}, {"tail", std::string(bytes.begin(), bytes.end())}})); }
          catch (const std::exception& failure) { fail(id, failure.what()); }
          source->Release();
          if (com) CoUninitialize();
        }).detach();
      } else fail(id, "unknown command");
      return;
    }
    const auto method = command.str("method");
    const JV* arguments = command.find("args");
    V converted = arguments ? ToValue(*arguments) : V(M{});
    const auto* args = std::get_if<M>(&converted);
    if (!args) { fail(id, "arguments"); return; }
    if (method == "watch") {
      // As flutter_window.cpp: observation is leased to the foreground session.
      const auto owner = args->find(V("owner")), enabled = args->find(V("enabled"));
      const auto* ownerValue = owner == args->end() ? nullptr : std::get_if<std::string>(&owner->second);
      const auto* enabledValue = enabled == args->end() ? nullptr : std::get_if<bool>(&enabled->second);
      if (!ownerValue || !enabledValue) { e2e::emitLine(object({{"id", id}, {"err", object({{"code", "invalid_arguments"}, {"message", ""}})}})); return; }
      try { const auto sequence = channel->Watch(*ownerValue, *enabledValue); respond(id, object({{"sequence", int64_t(sequence)}})); }
      catch (...) { e2e::emitLine(object({{"id", id}, {"err", object({{"code", "clipboard_not_active"}, {"message", ""}})}})); }
      return;
    }
    // --serve-delay-ms N: every 45 KiB chunk read of an offered file for the peer's paste is held back N ms (a slow link from this PC
    // to the VM); one pullRead of a block of chunks waits for each of its chunks.
    if (serveDelayMs > 0 && method == "pullRead") {
      int64_t length = int64_t(multitap::clipboard::formatsChunkBytes);
      if (const auto found = args ? args->find(V("length")) : M::const_iterator{}; args && found != args->end()) {
        if (const auto* n = std::get_if<int32_t>(&found->second)) length = *n; else if (const auto* m = std::get_if<int64_t>(&found->second)) length = *m;
      }
      const int64_t chunk = int64_t(multitap::clipboard::formatsChunkBytes);
      Sleep(DWORD(serveDelayMs * std::max<int64_t>(1, (length + chunk - 1) / chunk)));
    }
    flutter::MethodCall<V> call(method, std::make_unique<V>(std::move(converted)));
    channel->Enqueue(call, std::make_unique<Reply>(id));
  } catch (const std::exception& failure) { fail(id, failure.what()); }
}

LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kCommand) { std::unique_ptr<JV> command(reinterpret_cast<JV*>(lparam)); handle(*command); return 0; }
  if (message == multitap::ClientClipboardChannel::completed_message) { if (channel) channel->Complete(); return 0; }
  if (message == multitap::ClientClipboardChannel::pull_message) { if (channel) channel->DeliverPullEvents(); return 0; }
  if (message == WM_CLIPBOARDUPDATE) { if (channel) channel->ClipboardChanged(); return 0; }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 3 || wcscmp(argv[1], L"--root")) throw std::runtime_error("usage: client_e2e --root <dir> [--serve-delay-ms N]");
    const std::filesystem::path root(argv[2]);
    for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(argv[i], L"--serve-delay-ms")) serveDelayMs = _wtoi(argv[i + 1]);
    WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"Multitap.ClipboardE2E.Client";
    RegisterClassW(&cls);
    window = CreateWindowExW(0, cls.lpszClassName, L"Multitap clipboard e2e", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!window) throw std::runtime_error("window");
    multitap::clipboard::ChangeWatchAccess watch;
    watch.add = [](HWND) { return true; };
    watch.remove = [](HWND) {};
    watch.sequence = [] { return board.sequence(); };
    watch.owner = [] { return board.owner(); };
    multitap::ClientClipboardAccess access;
    namespace clip = multitap::clipboard;
    access.read = [](HWND w, bool t, bool f) { return board.read(w, clip::ReadOptions{t, f, false, false}); };
    access.readFormats = [](HWND w, const clip::ReadOptions& o) { return board.read(w, o); };
    access.sequence = [] { return board.sequence(); };
    access.owner = [] { return board.owner(); };
    access.putText = [](HWND w, const std::wstring& t, std::optional<DWORD> e) { return board.putText(w, t, e); };
    access.putFiles = [](HWND w, const std::vector<std::filesystem::path>& f, std::optional<DWORD> e) { return board.putFiles(w, f, e); };
    access.claim = [](HWND w, std::optional<DWORD> e) { return board.claim(w, e); };
    access.checkPublished = [](const std::vector<std::filesystem::path>&) { multitap::PublishCheck check; check.readable = true; return check; };
    access.publishers.rich = [](HWND w, const std::wstring& t, const std::vector<uint8_t>* h, const std::vector<uint8_t>*, std::optional<DWORD> e) { return board.putRich(w, t, h, e); };
    access.publishers.image = [](HWND w, const clip::Image& i, std::optional<DWORD> e) { return board.putImage(w, i, e); };
    access.publishers.paths = [](HWND w, const std::vector<std::filesystem::path>& p, std::optional<DWORD> e) { return board.putFiles(w, p, e); };
    access.publishers.localTree = [](HWND, const std::vector<clip::TreeEntry>& entries, std::optional<DWORD> e) { return board.publishLocalTree(entries, e); };
    access.localTreeCurrent = [] { return board.localTreeCurrent(); };
    access.publishOffer = [](const std::shared_ptr<clip::PullOffer>& o, std::optional<DWORD> e) { return board.publishOffer(o, e); };
    access.offerCurrent = [](const std::shared_ptr<clip::PullOffer>& o) { return board.offerCurrent(o); };
    access.withdrawOffer = [](const std::shared_ptr<clip::PullOffer>& o) { return board.withdraw(o); };
    board.changed = [] { PostMessageW(window, WM_CLIPBOARDUPDATE, 0, 0); };
    channel = std::make_unique<multitap::ClientClipboardChannel>(window, watch, [] { return true; }, access, root / L"client-receives");
    channel->SetChanged([](const std::string& owner, DWORD sequence) { e2e::emitLine(object({{"event", "changed"}, {"owner", owner}, {"sequence", int64_t(sequence)}})); });
    channel->SetPullHandler([](const multitap::PullEvent& event) {
      e2e::emitLine(object({{"event", "pull"}, {"kind", event.kind}, {"rangeId", int64_t(event.rangeId)}, {"offerId", event.offerId}, {"reason", event.reason},
                            {"index", int64_t(event.index)}, {"offset", int64_t(event.offset)}, {"end", int64_t(event.end)}}));
    });
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    e2e::emitLine(object({{"event", "started"}}));
    std::thread reader([] {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try { PostMessageW(window, kCommand, 0, reinterpret_cast<LPARAM>(new JV(e2e::parse(line)))); } catch (const std::exception& failure) { e2e::emitLine(object({{"event", "badLine"}, {"what", failure.what()}})); }
      }
      PostMessageW(window, WM_QUIT, 0, 0);
    });
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    channel.reset();
    if (com) CoUninitialize();
    reader.detach();
    return 0;
  } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}