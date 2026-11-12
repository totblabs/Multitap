#pragma once
// Shared by the two helper processes of the clipboard end-to-end harness (host_e2e.cpp, client_e2e.cpp): a small JSON
// reader/writer for the line protocol with the Dart test, an in-memory stand-in for the Windows clipboard (the real
// clipboard of this desktop is never touched), and the "paste" of a published paste-pull offer.
#include "clipboard.hpp"
#include "pull.hpp"
#include "virtual_source.hpp"
#include <winioctl.h>
#include <bcrypt.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace e2e {
namespace clip = multitap::clipboard;
namespace fs = std::filesystem;

// ---- JSON ----------------------------------------------------------------------------------------------------
struct JV;
using JArray = std::vector<JV>;
using JObject = std::vector<std::pair<std::string, JV>>;
struct JV {
  std::variant<std::nullptr_t, bool, int64_t, double, std::string, JArray, JObject> v = nullptr;
  JV() = default;
  JV(std::nullptr_t) {}
  JV(bool b) : v(b) {}
  JV(int64_t n) : v(n) {}
  JV(int n) : v(int64_t(n)) {}
  JV(uint64_t n) : v(int64_t(n)) {}
  JV(double d) : v(d) {}
  JV(const char* s) : v(std::string(s)) {}
  JV(std::string s) : v(std::move(s)) {}
  JV(JArray a) : v(std::move(a)) {}
  JV(JObject o) : v(std::move(o)) {}
  const JV* find(const std::string& key) const {
    if (const auto* o = std::get_if<JObject>(&v)) for (const auto& item : *o) if (item.first == key) return &item.second;
    return nullptr;
  }
  std::string str(const std::string& key, const std::string& fallback = {}) const { const auto* f = find(key); const auto* s = f ? std::get_if<std::string>(&f->v) : nullptr; return s ? *s : fallback; }
  int64_t num(const std::string& key, int64_t fallback = 0) const { const auto* f = find(key); if (!f) return fallback; if (const auto* n = std::get_if<int64_t>(&f->v)) return *n; if (const auto* d = std::get_if<double>(&f->v)) return int64_t(*d); return fallback; }
  bool flag(const std::string& key, bool fallback = false) const { const auto* f = find(key); const auto* b = f ? std::get_if<bool>(&f->v) : nullptr; return b ? *b : fallback; }
};
inline std::string utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(count), '\0'); WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), count, nullptr, nullptr); return out;
}
inline std::wstring wide(const std::string& text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(count), L'\0'); MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), count); return out;
}
inline void putUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
  else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
}
struct Parser {
  const std::string& s; size_t at = 0;
  explicit Parser(const std::string& text) : s(text) {}
  void ws() { while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) ++at; }
  [[noreturn]] void fail(const char* why) { throw std::runtime_error(std::string("json: ") + why); }
  JV value() {
    ws(); if (at >= s.size()) fail("eof");
    const char c = s[at];
    if (c == '{') { ++at; JObject o; ws(); if (s[at] == '}') { ++at; return o; } for (;;) { ws(); auto key = string(); ws(); if (s[at++] != ':') fail("colon"); o.emplace_back(std::move(key), value()); ws(); if (s[at] == ',') { ++at; continue; } if (s[at++] != '}') fail("object"); return o; } }
    if (c == '[') { ++at; JArray a; ws(); if (s[at] == ']') { ++at; return a; } for (;;) { a.push_back(value()); ws(); if (s[at] == ',') { ++at; continue; } if (s[at++] != ']') fail("array"); return a; } }
    if (c == '"') return string();
    if (s.compare(at, 4, "true") == 0) { at += 4; return true; }
    if (s.compare(at, 5, "false") == 0) { at += 5; return false; }
    if (s.compare(at, 4, "null") == 0) { at += 4; return nullptr; }
    const size_t start = at; bool real = false;
    while (at < s.size() && (std::isdigit(uint8_t(s[at])) || s[at] == '-' || s[at] == '+' || s[at] == '.' || s[at] == 'e' || s[at] == 'E')) { if (!std::isdigit(uint8_t(s[at])) && s[at] != '-') real = true; ++at; }
    if (start == at) fail("value");
    return real ? JV(std::stod(s.substr(start, at - start))) : JV(int64_t(std::stoll(s.substr(start, at - start))));
  }
  std::string string() {
    if (s[at++] != '"') fail("string"); std::string out;
    for (;;) {
      if (at >= s.size()) fail("string end"); const char c = s[at++];
      if (c == '"') return out;
      if (c != '\\') { out += c; continue; }
      const char e = s[at++];
      switch (e) {
        case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break;
        case 'u': { uint32_t cp = uint32_t(std::stoul(s.substr(at, 4), nullptr, 16)); at += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && s.compare(at, 2, "\\u") == 0) { const uint32_t low = uint32_t(std::stoul(s.substr(at + 2, 4), nullptr, 16)); at += 6; cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00); }
          putUtf8(out, cp); break; }
        default: out += e;
      }
    }
  }
};
inline JV parse(const std::string& text) { Parser p(text); auto v = p.value(); return v; }
inline void write(const JV& value, std::string& out) {
  struct Visit {
    std::string& out;
    void operator()(std::nullptr_t) { out += "null"; }
    void operator()(bool b) { out += b ? "true" : "false"; }
    void operator()(int64_t n) { out += std::to_string(n); }
    void operator()(double d) { out += std::to_string(d); }
    void operator()(const std::string& s) {
      out += '"';
      for (const unsigned char c : s) { if (c == '"' || c == '\\') { out += '\\'; out += char(c); } else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; } else out += char(c); }
      out += '"';
    }
    void operator()(const JArray& a) { out += '['; bool first = true; for (const auto& i : a) { if (!first) out += ','; first = false; write(i, out); } out += ']'; }
    void operator()(const JObject& o) { out += '{'; bool first = true; for (const auto& i : o) { if (!first) out += ','; first = false; (*this)(i.first); out += ':'; write(i.second, out); } out += '}'; }
  };
  std::visit(Visit{out}, value.v);
}
inline std::string dump(const JV& value) { std::string out; write(value, out); return out; }
inline JV object(std::initializer_list<std::pair<std::string, JV>> items) { return JV(JObject(items.begin(), items.end())); }

// One JSON line per message on stdout; any thread.
inline std::mutex& outputMutex() { static std::mutex m; return m; }
inline void emitLine(const JV& value) { const auto line = dump(value) + "\n"; std::lock_guard lock(outputMutex()); fwrite(line.data(), 1, line.size(), stdout); fflush(stdout); }

// ---- bytes ---------------------------------------------------------------------------------------------------
inline std::string base64(const std::vector<uint8_t>& bytes) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out; size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) { const uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2]; out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += t[n & 63]; }
  if (i + 1 == bytes.size()) { const uint32_t n = bytes[i] << 16; out += t[n >> 18]; out += t[(n >> 12) & 63]; out += "=="; }
  else if (i + 2 == bytes.size()) { const uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8); out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += '='; }
  return out;
}
inline std::vector<uint8_t> unbase64(const std::string& text) {
  std::vector<uint8_t> out; uint32_t acc = 0; int bits = 0;
  for (const char c : text) {
    int v; if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26; else if (c >= '0' && c <= '9') v = c - '0' + 52; else if (c == '+') v = 62; else if (c == '/') v = 63; else continue;
    acc = (acc << 6) | uint32_t(v); bits += 6; if (bits >= 8) { bits -= 8; out.push_back(uint8_t(acc >> bits)); }
  }
  return out;
}
class Sha256 {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("sha256");
    DWORD length = 0, got = 0; BCryptGetProperty(alg_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &got, 0);
    object_.resize(length); if (BCryptCreateHash(alg_, &hash_, object_.data(), length, nullptr, 0, 0) < 0) throw std::runtime_error("sha256");
  }
  ~Sha256() { if (hash_) BCryptDestroyHash(hash_); if (alg_) BCryptCloseAlgorithmProvider(alg_, 0); }
  Sha256(const Sha256&) = delete;
  void add(const uint8_t* data, size_t size) { if (size) BCryptHashData(hash_, const_cast<PUCHAR>(data), ULONG(size), 0); }
  std::string finish() { uint8_t d[32]; BCryptFinishHash(hash_, d, 32, 0); std::string out; for (const auto b : d) { char h[3]; snprintf(h, sizeof(h), "%02x", b); out += h; } return out; }
 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr; BCRYPT_HASH_HANDLE hash_ = nullptr; std::vector<BYTE> object_;
};
inline std::string sha256(const std::vector<uint8_t>& bytes) { Sha256 h; h.add(bytes.data(), bytes.size()); return h.finish(); }
inline std::string sha256File(const fs::path& path) {
  std::ifstream in(clip::longPath(path), std::ios::binary); Sha256 h; std::vector<char> buffer(1 << 16);
  while (in) { in.read(buffer.data(), std::streamsize(buffer.size())); h.add(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(in.gcount())); }
  return h.finish();
}

// ---- stand-in clipboard --------------------------------------------------------------------------------------
// Mirrors what the clipboard library reads and publishes. Whoever publishes passes its window, which becomes the
// owner (the echo checks compare it); a copy by "the user" has the owner `user()`.
class Board {
 public:
  static HWND user() { return reinterpret_cast<HWND>(uintptr_t(0x7001)); }
  std::function<void()> changed;  // called after every modification, outside the lock (the Windows listener)
  clip::Snapshot read(HWND, const clip::ReadOptions& options) {
    // Virtual files (a zip folder, a mail attachment): like clip::read, they are staged when the caller gave a staging folder and
    // refused as unsupported otherwise. The slow part runs outside the lock.
    IDataObject* data = nullptr; clip::Snapshot staged;
    {
      std::lock_guard lock(m_);
      if (virtual_ && !offer_ && options.files) {
        if (!options.virtualFiles) throw std::runtime_error("unsupported_clipboard_format");
        data = virtual_; data->AddRef(); staged.sequence = seq_; staged.owner = owner_;
      }
    }
    if (data) {
      struct Release { IDataObject* d; ~Release() { d->Release(); } } release{data};
      auto copy = clip::stageVirtualFiles(data, *options.virtualFiles);
      if (!copy) throw std::runtime_error("unsupported_clipboard_format");
      staged.files = copy->tops; staged.staged = std::move(copy); return staged;
    }
    std::lock_guard lock(m_); clip::Snapshot s; s.sequence = seq_; s.owner = owner_;
    if (offer_ || (seq_ == offerSeq_ && offerSeq_ != 0)) return s;  // virtual files: no CF_HDROP, nothing readable by the library
    // The decisions of clip::read, over the stand-in's content (the same shared rule for bitmap + image files).
    if (options.files && !files_.empty() && !(options.images && image_ && clip::preferImageOverFiles(files_, true))) s.files = files_;
    else {
      if (options.text && text_) s.text = text_;
      if (options.rich && text_ && html_) s.html = html_;
      if (options.images && options.files && image_ && !text_) s.image = image_;
    }
    if (options.reportDenied && s.files.empty() && !s.text && !s.image) {
      if (!options.files && (image_ || !files_.empty())) throw std::runtime_error("clipboard_policy_denied");
      if (!options.text && text_) throw std::runtime_error("clipboard_policy_denied");
      if (options.files && !options.images && image_) throw std::runtime_error("unsupported_clipboard_format");
    }
    return s;
  }
  DWORD sequence() { std::lock_guard lock(m_); return seq_; }
  HWND owner() { std::lock_guard lock(m_); return owner_; }
  DWORD putText(HWND window, const std::wstring& text, std::optional<DWORD> expected) {
    return set(window, expected, [&] { text_ = text; });
  }
  DWORD putRich(HWND window, const std::wstring& text, const std::vector<uint8_t>* html, std::optional<DWORD> expected) {
    return set(window, expected, [&] { text_ = text; if (html) html_ = *html; });
  }
  DWORD putImage(HWND window, const clip::Image& image, std::optional<DWORD> expected) {
    return set(window, expected, [&] { image_ = image; });
  }
  DWORD putFiles(HWND window, const std::vector<fs::path>& files, std::optional<DWORD> expected) {
    return set(window, expected, [&] { files_ = files; });
  }
  // The client's claim for a copy that is still arriving: emptied under window while it still holds what it held at expected.
  DWORD claim(HWND window, std::optional<DWORD> expected) { return set(window, expected, [&] {}); }
  DWORD publishOffer(const std::shared_ptr<clip::PullOffer>& offer, std::optional<DWORD> expected) {
    const DWORD sequence = set(nullptr, expected, [&] { offer_ = offer; }, true);
    // A blocking render (GetData) gives up when another copy replaced the offer, as the real clipboard check does.
    offer->setCurrentCheck([this, weak = std::weak_ptr<clip::PullOffer>(offer)] { const auto live = weak.lock(); return live && offerCurrent(live); });
    return sequence;
  }
  // A received tree served from its local copy as virtual files (paths of MAX_PATH or more): the stand-in of publishLocalTree.
  DWORD publishLocalTree(const std::vector<clip::TreeEntry>& entries, std::optional<DWORD> expected) {
    IDataObject* object = clip::createLocalTreeObject(entries);
    try { return set(nullptr, expected, [&] { tree_ = object; }, true); } catch (...) { object->Release(); throw; }
  }
  bool localTreeCurrent() { std::lock_guard lock(m_); return tree_ && seq_ == offerSeq_; }
  // The data object of the content a paste reads: a paste-pull offer or a locally served tree (AddRef'd), nullptr when neither.
  IDataObject* pasteSource() {
    std::lock_guard lock(m_);
    if (seq_ != offerSeq_) return nullptr;
    if (offer_) return offer_->dataObject();
    if (tree_) { tree_->AddRef(); return tree_; }
    return nullptr;
  }
  bool offerCurrent(const std::shared_ptr<clip::PullOffer>& offer) { std::lock_guard lock(m_); return offer_ == offer && seq_ == offerSeq_; }
  DWORD withdraw(const std::shared_ptr<clip::PullOffer>& offer) { DWORD now; { std::lock_guard lock(m_); if (!(offer_ == offer && seq_ == offerSeq_)) return 0; clearLocked(); ++seq_; owner_ = nullptr; offerSeq_ = 0; now = seq_; } notify(); return now; }
  std::shared_ptr<clip::PullOffer> currentOffer() { std::lock_guard lock(m_); return offer_ && seq_ == offerSeq_ ? offer_ : nullptr; }
  // What the test user copies.
  void userText(const std::wstring& text) { set(user(), std::nullopt, [&] { text_ = text; }); }
  void userRich(const std::wstring& text, const std::vector<uint8_t>& html) { set(user(), std::nullopt, [&] { text_ = text; html_ = html; }); }
  void userImage(const clip::Image& image) { set(user(), std::nullopt, [&] { image_ = image; }); }
  // A screenshot tool's copy: the bitmap and its auto-saved image file together (Snipping Tool).
  void userScreenshot(const clip::Image& image, const std::vector<fs::path>& files) { set(user(), std::nullopt, [&] { image_ = image; files_ = files; }); }
  void userFiles(const std::vector<fs::path>& files) { set(user(), std::nullopt, [&] { files_ = files; }); }
  // A copy of virtual files only (no CF_HDROP): a synthetic OLE data object, as a .zip folder or a mail attachment publishes.
  void userVirtual(std::vector<vsrc::Item> items) { set(user(), std::nullopt, [&] { virtual_ = new vsrc::Source(std::move(items)); }); }
  void userClear() { set(user(), std::nullopt, [&] {}); }
  // The entries of the given top-level paths (folders walked) in the form describe() reports them, without touching the clipboard.
  static JV describePaths(const std::vector<fs::path>& tops) { JArray list; for (const auto& top : tops) listInto(top, top.filename().wstring(), list); return object({{"entries", std::move(list)}}); }
  // The content as the test checks it: kind, text, html and image digests, and every file (folders walked) with SHA-256.
  JV describe() {
    std::lock_guard lock(m_); JObject o;
    o.emplace_back("sequence", int64_t(seq_));
    if (virtual_) { o.emplace_back("kind", "virtual"); return JV(std::move(o)); }
    if (tree_ && seq_ == offerSeq_) { o.emplace_back("kind", "localTree"); return JV(std::move(o)); }
    if (offer_ && seq_ == offerSeq_) { o.emplace_back("kind", "offer"); o.emplace_back("offerFormat", offer_->format()); o.emplace_back("files", int64_t(offer_->fileCount())); o.emplace_back("bytes", int64_t(offer_->totalBytes())); return JV(std::move(o)); }
    if (!files_.empty()) { o.emplace_back("kind", "files"); JArray list; for (const auto& top : files_) listInto(top, top.filename().wstring(), list); o.emplace_back("entries", std::move(list)); return JV(std::move(o)); }
    if (image_) { o.emplace_back("kind", "image"); o.emplace_back("pngSha", sha256(image_->png)); o.emplace_back("width", int64_t(image_->width)); o.emplace_back("height", int64_t(image_->height)); return JV(std::move(o)); }
    if (text_) {
      o.emplace_back("kind", html_ ? "richtext" : "text");
      // A long text is reported by its length and digest, not by its body (64 MiB per answer would be absurd).
      const auto body = utf8(*text_);
      if (body.size() > (1u << 20)) { o.emplace_back("textLength", int64_t(text_->size())); o.emplace_back("textSha", sha256(std::vector<uint8_t>(body.begin(), body.end()))); }
      else o.emplace_back("text", body);
      if (html_) o.emplace_back("html", std::string(html_->begin(), html_->end()));
      return JV(std::move(o));
    }
    o.emplace_back("kind", "empty"); return JV(std::move(o));
  }
 private:
  template <class F> DWORD set(HWND window, std::optional<DWORD> expected, F&& fill, bool asOffer = false) {
    DWORD now;
    {
      std::lock_guard lock(m_);
      clip::requireUnchangedSequence(expected, seq_);
      clearLocked(); fill(); ++seq_; owner_ = window; now = seq_; offerSeq_ = asOffer ? seq_ : 0;
    }
    notify(); return now;
  }
  void clearLocked() { text_.reset(); html_.reset(); image_.reset(); files_.clear(); offer_.reset(); if (virtual_) { virtual_->Release(); virtual_ = nullptr; } if (tree_) { tree_->Release(); tree_ = nullptr; } }
  void notify() { if (changed) changed(); }
  static void listInto(const fs::path& path, const std::wstring& relative, JArray& out) {
    std::error_code ec;
    if (fs::is_directory(clip::longPath(path), ec)) {
      out.push_back(object({{"path", utf8(relative)}, {"dir", true}}));
      std::vector<fs::path> children; for (const auto& e : fs::directory_iterator(clip::longPath(path), ec)) children.push_back(path / e.path().filename());
      std::sort(children.begin(), children.end());
      for (const auto& child : children) listInto(child, relative + L"/" + child.filename().wstring(), out);
    } else out.push_back(object({{"path", utf8(relative)}, {"size", int64_t(fs::file_size(clip::longPath(path), ec))}, {"sha", sha256File(path)}}));
  }
  std::mutex m_; DWORD seq_ = 1; HWND owner_ = nullptr; DWORD offerSeq_ = 0;
  std::optional<std::wstring> text_; std::optional<std::vector<uint8_t>> html_; std::optional<clip::Image> image_; std::vector<fs::path> files_;
  std::shared_ptr<clip::PullOffer> offer_;
  IDataObject* virtual_ = nullptr;
  IDataObject* tree_ = nullptr;
};

// A sparse file of `size` bytes whose last bytes are `tail` (fixtures above 4 GiB without writing them).
inline void makeSparse(const fs::path& path, uint64_t size, const std::string& tail) {
  fs::create_directories(path.parent_path());
  const HANDLE file = CreateFileW(clip::longPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("sparse: create failed");
  DWORD returned = 0; DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
  LARGE_INTEGER at{}; at.QuadPart = LONGLONG(size - tail.size());
  if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) { CloseHandle(file); throw std::runtime_error("sparse: seek failed"); }
  DWORD written = 0; WriteFile(file, tail.data(), DWORD(tail.size()), &written, nullptr); CloseHandle(file);
}

// Reads the last `length` bytes of file `entry` of a paste source through its FileContents stream (a Seek beyond 4 GiB, like a resumed paste).
inline std::vector<uint8_t> pasteTail(IDataObject* data, LONG entry, uint64_t length, uint64_t* sizeOut) {
  const UINT contentsFormat = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
  FORMATETC contents{CLIPFORMAT(contentsFormat), nullptr, DVASPECT_CONTENT, entry, TYMED_ISTREAM}; STGMEDIUM stream{};
  if (FAILED(data->GetData(&contents, &stream)) || stream.tymed != TYMED_ISTREAM) throw std::runtime_error("tail: contents unavailable");
  STATSTG stat{}; stream.pstm->Stat(&stat, STATFLAG_NONAME); const uint64_t size = stat.cbSize.QuadPart; if (sizeOut) *sizeOut = size;
  LARGE_INTEGER to{}; to.QuadPart = LONGLONG(size - length); ULARGE_INTEGER now{};
  if (FAILED(stream.pstm->Seek(to, STREAM_SEEK_SET, &now))) { ReleaseStgMedium(&stream); throw std::runtime_error("tail: seek failed"); }
  std::vector<uint8_t> out; std::vector<uint8_t> buffer(1 << 16); ULONG got = 0;
  for (;;) {
    const HRESULT read = stream.pstm->Read(buffer.data(), ULONG(buffer.size()), &got);
    if (FAILED(read)) { ReleaseStgMedium(&stream); throw std::runtime_error("tail: stream read failed 0x" + std::to_string(unsigned(read))); }
    if (got == 0) break;
    out.insert(out.end(), buffer.begin(), buffer.begin() + got);
  }
  ReleaseStgMedium(&stream); return out;
}
// {"name", "dir", "data" (base64), "mode" ("stream" | "global" | "none"), "declare" (size in the descriptor, default true)} per entry.
inline std::vector<vsrc::Item> virtualItems(const JV& list) {
  std::vector<vsrc::Item> items;
  if (const auto* array = std::get_if<JArray>(&list.v)) for (const auto& entry : *array) {
    vsrc::Item item; item.name = wide(entry.str("name")); item.directory = entry.flag("dir"); item.data = unbase64(entry.str("data"));
    const auto mode = entry.str("mode", "stream");
    item.mode = mode == "global" ? vsrc::Item::Global : mode == "none" ? vsrc::Item::NoContents : vsrc::Item::Stream;
    item.declareSize = entry.flag("declare", true);
    items.push_back(std::move(item));
  }
  return items;
}

// Explorer's paste of virtual files: reads the descriptors and each FILECONTENTS stream of the offer's data object into
// `destination`. Returns the number of file bytes written. Throws on any failure. `discard` reads every stream the same way and
// writes nothing (the throughput benchmark separates the transfer from the disk).
inline uint64_t pasteOffer(IDataObject* data, const fs::path& destination, int slowMs = 0, bool discard = false) {
  const UINT descriptorFormat = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW), contentsFormat = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
  FORMATETC format{CLIPFORMAT(descriptorFormat), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format, &medium))) throw std::runtime_error("paste: no file descriptors");
  uint64_t written = 0;
  const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
  if (!group) throw std::runtime_error("paste: descriptors unreadable");
  std::vector<FILEDESCRIPTORW> items(group->fgd, group->fgd + group->cItems);
  GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
  for (size_t at = 0; at < items.size(); ++at) {
    // (Explorer fails at a destination path of MAX_PATH or more, see docs; the harness is long-path aware so that the content of a deep paste can be compared.)
    const fs::path target = destination / items[at].cFileName;
    if ((items[at].dwFlags & FD_ATTRIBUTES) && (items[at].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { if (!discard) fs::create_directories(clip::longPath(target)); continue; }
    if (!discard) fs::create_directories(clip::longPath(target.parent_path()));
    FORMATETC contents{CLIPFORMAT(contentsFormat), nullptr, DVASPECT_CONTENT, LONG(at), TYMED_ISTREAM};
    STGMEDIUM stream{};
    const HRESULT hr = data->GetData(&contents, &stream);
    if (FAILED(hr) || stream.tymed != TYMED_ISTREAM) throw std::runtime_error("paste: contents unavailable");
    std::ofstream out;
    if (!discard) out.open(clip::longPath(target), std::ios::binary);
    std::vector<char> buffer(1 << 16); ULONG got = 0;
    for (;;) {
      const HRESULT read = stream.pstm->Read(buffer.data(), ULONG(buffer.size()), &got);
      if (FAILED(read)) { ReleaseStgMedium(&stream); throw std::runtime_error("paste: stream read failed 0x" + std::to_string(unsigned(read))); }
      if (got == 0) break;
      if (!discard) out.write(buffer.data(), got);
      written += got;
      if (slowMs > 0) Sleep(DWORD(slowMs));   // a slow consumer (a copy dialog of a big folder): the sender's window fills and waits for it
    }
    ReleaseStgMedium(&stream);
  }
  return written;
}

// ---- Blocking renders of an on-paste offer (text, HTML, an image, CF_HDROP): GetData waits until the data is there ------------------
inline std::string hexOf(HRESULT result) { char buffer[16]; snprintf(buffer, sizeof(buffer), "%08x", unsigned(result)); return buffer; }
inline std::string formatLabel(CLIPFORMAT format) {
  if (format == CF_HDROP) return "CF_HDROP"; if (format == CF_UNICODETEXT) return "CF_UNICODETEXT"; if (format == CF_DIB) return "CF_DIB"; if (format == CF_DIBV5) return "CF_DIBV5";
  wchar_t name[128]{}; GetClipboardFormatNameW(format, name, 128); return utf8(name);
}
inline HGLOBAL getGlobal(IDataObject* data, CLIPFORMAT format) {
  FORMATETC wanted{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
  const HRESULT result = data->GetData(&wanted, &medium);
  if (FAILED(result)) throw std::runtime_error("GetData(" + formatLabel(format) + ") failed 0x" + hexOf(result));
  return medium.hGlobal;
}
inline std::vector<uint8_t> takeBytes(HGLOBAL memory) {
  const auto size = GlobalSize(memory); const auto* p = static_cast<const uint8_t*>(GlobalLock(memory)); std::vector<uint8_t> out(p, p + size); GlobalUnlock(memory); GlobalFree(memory); return out;
}
// The commands formats / renderText / renderHtml / renderImage / renderHdrop, run on a thread of their own (GetData blocks):
// the application that pastes in the other peer's VM or on this PC. Returns false for any other command.
inline bool handleRender(Board& board, const JV& command, int64_t id) {
  const auto name = command.str("cmd");
  if (name != "formats" && name != "renderText" && name != "renderHtml" && name != "renderImage" && name != "renderHdrop") return false;
  IDataObject* source = board.pasteSource();
  if (!source) { emitLine(object({{"id", id}, {"err", "no offer on the clipboard"}})); return true; }
  std::thread([id, name, source] {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    const auto start = std::chrono::steady_clock::now();
    const auto took = [&] { return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); };
    try {
      if (name == "formats") {
        IEnumFORMATETC* enumerator = nullptr; if (FAILED(source->EnumFormatEtc(DATADIR_GET, &enumerator))) throw std::runtime_error("EnumFormatEtc failed");
        JArray list; FORMATETC each{}; ULONG fetched = 0; while (enumerator->Next(1, &each, &fetched) == S_OK) list.push_back(formatLabel(each.cfFormat));
        enumerator->Release();
        bool progress = false;
        FORMATETC descriptor{CLIPFORMAT(RegisterClipboardFormatW(L"FileGroupDescriptorW")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (SUCCEEDED(source->GetData(&descriptor, &medium))) {
          const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
          if (group) { for (UINT i = 0; i < group->cItems; ++i) progress = progress || (group->fgd[i].dwFlags & FD_PROGRESSUI) != 0; GlobalUnlock(medium.hGlobal); }
          ReleaseStgMedium(&medium);
        }
        emitLine(object({{"id", id}, {"ok", object({{"formats", JV(std::move(list))}, {"explorerProgress", progress}})}}));
      } else if (name == "renderText") {
        const auto memory = getGlobal(source, CF_UNICODETEXT); const auto size = GlobalSize(memory); const auto* p = static_cast<const wchar_t*>(GlobalLock(memory));
        std::wstring text(p, size / sizeof(wchar_t)); GlobalUnlock(memory); GlobalFree(memory); while (!text.empty() && text.back() == L'\0') text.pop_back();
        const auto body = utf8(text); JObject o; o.emplace_back("textLength", int64_t(text.size())); o.emplace_back("textSha", sha256(std::vector<uint8_t>(body.begin(), body.end())));
        if (body.size() <= (1u << 20)) o.emplace_back("text", body); o.emplace_back("ms", took()); emitLine(object({{"id", id}, {"ok", JV(std::move(o))}}));
      } else if (name == "renderHtml") {
        auto bytes = takeBytes(getGlobal(source, CLIPFORMAT(RegisterClipboardFormatW(L"HTML Format")))); while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
        emitLine(object({{"id", id}, {"ok", object({{"html", std::string(bytes.begin(), bytes.end())}, {"ms", took()}})}}));
      } else if (name == "renderImage") {
        const auto png = takeBytes(getGlobal(source, CLIPFORMAT(RegisterClipboardFormatW(L"PNG"))));
        const auto memory = getGlobal(source, CF_DIBV5); const auto* header = static_cast<const BITMAPV5HEADER*>(GlobalLock(memory));
        const int64_t width = header->bV5Width, height = header->bV5Height; GlobalUnlock(memory); GlobalFree(memory);
        emitLine(object({{"id", id}, {"ok", object({{"pngSha", sha256(png)}, {"width", width}, {"height", height}, {"ms", took()}})}}));
      } else {
        const auto memory = getGlobal(source, CF_HDROP); const auto* base = static_cast<const uint8_t*>(GlobalLock(memory)); DROPFILES header{}; std::memcpy(&header, base, sizeof(header));
        std::vector<fs::path> tops; JArray paths; const auto* at = reinterpret_cast<const wchar_t*>(base + header.pFiles);
        while (*at) { tops.emplace_back(at); paths.push_back(utf8(tops.back().wstring())); at += tops.back().wstring().size() + 1; }
        GlobalUnlock(memory); GlobalFree(memory);
        emitLine(object({{"id", id}, {"ok", object({{"tops", JV(std::move(paths))}, {"board", Board::describePaths(tops)}, {"ms", took()}})}}));
      }
    } catch (const std::exception& failure) { emitLine(object({{"id", id}, {"err", std::string(failure.what())}})); }
    source->Release();
    if (com) CoUninitialize();
  }).detach();
  return true;
}
}  // namespace e2e
