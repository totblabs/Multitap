// The PC cancels a pull this Host serves (a field case): the worker records the peer's cancel once. The cancel itself
// stays queued behind the pull it stopped; once that pull has ended it is not a second failure of it (no pullFailed record and no
// clipboard.error for an id the relay has already retired). No clipboard is touched: every clipboard access of the worker is a
// stand-in, the copied file is this test's own and the "VM copy" is a fake snapshot.
#include "clipboard_worker.hpp"
#include <condition_variable>
#include <fstream>
#include <iostream>
using namespace host;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Peer {
  std::mutex m; std::condition_variable cv; std::vector<Json> packets, diagnostics;
  void output(const std::string& text) {
    const auto value = JsonParser::parse(text);
    std::lock_guard lock(m);
    if (value.get("type").string() == "clip.diag") diagnostics.push_back(value);
    else if (value.get("type").string() == "clip.packet") packets.push_back(value.get("payload"));
    cv.notify_all();
  }
  template <class P> bool waitFor(P predicate, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    std::unique_lock lock(m); return cv.wait_for(lock, limit, [&] { return predicate(); });
  }
  size_t count(const char* type, const std::string& id) { size_t n = 0; for (const auto& p : packets) n += p.get("type").string() == type && p.get("requestId").string() == id; return n; }
  size_t stage(uint32_t value) { size_t n = 0; for (const auto& d : diagnostics) n += d.get("stage").number(-1) == double(value); return n; }
};
std::string number(uint64_t value) { return std::to_string(value); }
// The released implementations, as references for the fast paths (same text, byte for byte).
std::string quoteReference(const std::string& text) { std::string out = "\""; for (unsigned char c : text) { if (c == '\\' || c == '"') { out += '\\'; out += char(c); } else if (c < 32) { const char hex[] = "0123456789abcdef"; out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; } else out += char(c); } return out + '"'; }
std::string base64Reference(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return {};
  DWORD length = 0; CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length);
  std::string out(length, 0); CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &length); out.resize(length); return out;
}
void encodingChecks() {
  uint32_t seed = 12345; const auto next = [&] { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 24); };
  for (size_t size = 0; size <= 300; ++size) {
    std::vector<uint8_t> bytes(size); for (auto& b : bytes) b = next();
    const auto text = clipboardBase64(bytes); require(text == base64Reference(bytes), "fast base64 differs from CryptBinaryToStringA");
    if (size) require(clipboardChunk(text, 512) == bytes, "fast base64 decode does not round-trip");
  }
  std::vector<uint8_t> chunk(clip::formatsChunkBytes); for (auto& b : chunk) b = next();
  require(clipboardBase64(chunk) == base64Reference(chunk) && clipboardChunk(clipboardBase64(chunk), clip::formatsChunkBytes) == chunk, "a whole chunk");
  for (const char* bad : {"QR==", "QQ=A", "Q===", "QUJ", "QU J", "QUJD\n", "QUJ*", "=QUJ"}) {
    bool refused = false; try { clipboardChunk(bad, 512); } catch (...) { refused = true; } require(refused, "a noncanonical or malformed base64 text was accepted");
  }
  std::string every; for (int c = 0; c < 256; ++c) every += char(c); every += "plain run \"quoted\" back\\slash";
  require(jsonQuote(every) == quoteReference(every) && jsonQuote("") == "\"\"" && jsonQuote("abc") == "\"abc\"", "fast jsonQuote differs");
  const auto parsed = JsonParser::parse("{\"a\":" + jsonQuote(every) + ",\"b\":\"x\\u00e9\\ud83d\\ude00y\",\"c\":\"\"}");
  require(parsed.get("a").string() == every && parsed.get("b").string() == "x\xc3\xa9\xf0\x9f\x98\x80y" && parsed.get("c").string().empty(), "JSON string round trip");
  bool refused = false; try { JsonParser::parse(std::string("{\"a\":\"x") + char(10) + "\"}"); } catch (...) { refused = true; } require(refused, "a raw control character in a JSON string was accepted");
}
// The plain decoder (decode, then re-encode and compare), the reference for the canonical-bits check.
std::vector<uint8_t> chunkReference(const std::string& text, size_t maxBytes) {
  if (text.empty() || text.size() > (maxBytes + 2) / 3 * 4 || text.size() % 4) throw std::runtime_error("size");
  static const auto values = [] { std::array<int8_t, 256> v{}; v.fill(-1); const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; ++i) v[uint8_t(table[i])] = int8_t(i); return v; }();
  const size_t pad = text.back() != '=' ? 0 : text[text.size() - 2] == '=' ? 2 : 1; const size_t length = text.size() / 4 * 3 - pad;
  if (!length || length > maxBytes) throw std::runtime_error("size");
  std::vector<uint8_t> out(length); size_t to = 0;
  for (size_t at = 0; at < text.size(); at += 4) {
    const bool last = at + 4 == text.size(); int8_t q[4];
    for (int i = 0; i < 4; ++i) { const char c = text[at + i]; q[i] = last && c == '=' && i >= 4 - int(pad) ? 0 : values[uint8_t(c)]; if (q[i] < 0) throw std::runtime_error("noncanonical"); }
    const uint32_t v = (uint32_t(q[0]) << 18) | (uint32_t(q[1]) << 12) | (uint32_t(q[2]) << 6) | uint32_t(q[3]);
    out[to++] = uint8_t(v >> 16); if (to < length) out[to++] = uint8_t(v >> 8); if (to < length) out[to++] = uint8_t(v);
  }
  if (clipboardBase64(out) != text) throw std::runtime_error("noncanonical"); return out;
}
// The fast paths of a chunk's hops give exactly what the reference gives: the decoder accepts and refuses the same
// texts, jsonQuote and JsonParser agree with the byte-by-byte reference on special characters at every position of a 16-byte block.
void fastPathChecks() {
  uint32_t seed = 777; const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=*-_ \n\x80";
  size_t accepted = 0, refused = 0;
  for (int round = 0; round < 20000; ++round) {
    std::vector<uint8_t> bytes(1 + next() % 200); for (auto& b : bytes) b = uint8_t(next());
    std::string text = clipboardBase64(bytes);
    if (round % 2) for (int edits = 1 + int(next() % 2); edits; --edits) text[next() % text.size()] = alphabet[next() % alphabet.size()];
    std::optional<std::vector<uint8_t>> fast, reference;
    try { fast = clipboardChunk(text, 512); } catch (...) {}
    try { reference = chunkReference(text, 512); } catch (...) {}
    require(fast == reference, "the fast base64 decoder disagrees with the released one");
    (fast ? accepted : refused)++;
  }
  std::cout << "{\"decoderFuzz\":{\"accepted\":" << accepted << ",\"refused\":" << refused << "}}\n";
  require(accepted > 8000 && refused > 500, "the decoder fuzz did not cover both outcomes");
  for (int round = 0; round < 3000; ++round) {
    std::string text(next() % 100, 'a'); for (auto& c : text) c = char('a' + next() % 26);
    for (int specials = int(next() % 4); specials; --specials) if (!text.empty()) { const char choices[] = {'"', '\\', '\n', '\x01', '\x1f', ' ', '\x7f', '\x80', '\xff'}; text[next() % text.size()] = choices[next() % sizeof(choices)]; }
    require(jsonQuote(text) == quoteReference(text), "the fast jsonQuote differs from the byte-by-byte one");
    require(JsonParser::parse("{\"v\":" + jsonQuote(text) + "}").get("v").string() == text, "a quoted text does not parse back");
  }
}
// The read-ahead of a served file: one open per block of chunks, the same bytes and hash, and a file changed after a
// block was read is still reported (at the next block, or at the digest).
void readAheadChecks(const std::filesystem::path& root) {
  const auto file = root / L"src" / L"ahead.bin";
  const size_t block = 22 * clip::formatsChunkBytes, size = 2 * block + 12345;
  std::vector<uint8_t> content(size); for (size_t i = 0; i < size; ++i) content[i] = uint8_t(i * 97 % 251);
  const auto write = [&](const std::vector<uint8_t>& bytes) { std::ofstream out(file, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); };
  write(content);
  const auto offer = clip::SourceOffer::capture("ahead", {file}, clip::Bounds{});
  {
    auto reader = offer->open(0, 0); std::vector<uint8_t> got;
    while (reader->position() < reader->size()) { const auto piece = reader->next(clip::formatsChunkBytes); got.insert(got.end(), piece.begin(), piece.end()); }
    require(got == content && reader->digest() == clipboardDigest(content), "read-ahead changed the served bytes or their hash");
    auto resumed = offer->open(0, block + 7); const auto tail = resumed->next(clip::formatsChunkBytes);
    require(std::equal(tail.begin(), tail.end(), content.begin() + std::ptrdiff_t(block + 7)), "a pull from an offset served other bytes");
  }
  const auto changed = [&](size_t chunksBefore) {
    write(content); const auto fresh = clip::SourceOffer::capture("changed", {file}, clip::Bounds{});
    auto reader = fresh->open(0, 0); for (size_t i = 0; i < chunksBefore; ++i) (void)reader->next(clip::formatsChunkBytes);
    auto edited = content; edited[size - 1] ^= 1; write(edited);
    FILETIME later{}; GetSystemTimeAsFileTime(&later); ULARGE_INTEGER t{}; t.LowPart = later.dwLowDateTime; t.HighPart = later.dwHighDateTime; t.QuadPart += 50000000; later.dwLowDateTime = t.LowPart; later.dwHighDateTime = t.HighPart;
    const HANDLE h = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); SetFileTime(h, nullptr, nullptr, &later); CloseHandle(h);
    std::string reason;
    try { while (reader->position() < reader->size()) (void)reader->next(clip::formatsChunkBytes); (void)reader->digest(); } catch (const std::exception& failure) { reason = failure.what(); }
    return reason;
  };
  require(changed(1) == "clipboard_selected_file_changed", "a file changed inside the first block was not reported");
  require(changed(23) == "clipboard_selected_file_changed", "a file changed inside a later block was not reported");
}
std::string readyJson(const std::string& session, bool queue) {
  return "{\"type\":\"clipboard.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"clipboard-v1\",\"clipboardSync\":true,"
         "\"clipboardPolicy\":{\"textToRemote\":true,\"filesToRemote\":true,\"textToLocal\":true,\"filesToLocal\":true},\"limits\":{"
         "\"scaleProfile\":1,\"limitsProfile\":2,\"formatsProfile\":2,\"pullProfile\":1,\"maxOffers\":" + number(clip::maxLiveOffers) +
         ",\"chunkBytes\":" + number(clip::formatsChunkBytes) + ",\"windowUnits\":" + number(clip::SendWindow::defaultUnits) + ",\"windowBytes\":" + number(clip::SendWindow::defaultBytes) +
         ",\"maxFiles\":" + number(clip::maxEntries) + ",\"maxFileBytes\":" + number(clip::scaleBytes) + ",\"maxTotalBytes\":" + number(clip::scaleBytes) +
         ",\"maxTextBytes\":" + number(clip::maxTextBytes) + ",\"maxTextUnits\":" + number(clip::maxTextUnits) + ",\"maxEntries\":" + number(clip::maxEntries) +
         ",\"maxDepth\":" + number(clip::maxDepth) + ",\"maxPathChars\":" + number(clip::maxPathChars) + ",\"maxRichBytes\":" + number(clip::maxRichBytes) +
         ",\"onPasteProfile\":1,\"pushTextBytes\":" + number(clip::pushTextBytes) + ",\"pushImageBytes\":" + number(clip::pushImageBytes) + "}" +
         (queue ? ",\"clipboardPullQueue\":\"pull-queue-4-v1\"" : "") + "}";
}
ClipboardWorkerAccess standIn(const std::filesystem::path& file) {
  ClipboardWorkerAccess access;
  const auto refuse = [](const char* what) { return std::runtime_error(std::string("the test must not touch the clipboard: ") + what); };
  access.read = [refuse](HWND, bool, bool) -> clip::Snapshot { throw refuse("read"); };
  access.putText = [refuse](HWND, const std::wstring&, std::optional<DWORD>) -> DWORD { throw refuse("putText"); };
  access.putFiles = [refuse](HWND, const std::vector<std::filesystem::path>&, std::optional<DWORD>) -> DWORD { throw refuse("putFiles"); };
  access.publishOffer = [refuse](const std::shared_ptr<clip::PullOffer>&, std::optional<DWORD>) -> DWORD { throw refuse("publishOffer"); };
  access.offerCurrent = [](const std::shared_ptr<clip::PullOffer>&) { return false; };
  access.withdrawOffer = [](const std::shared_ptr<clip::PullOffer>&) -> DWORD { return 0; };
  access.formats = [] { return uint32_t(1); };
  access.watch.add = [](HWND) { return true; }; access.watch.remove = [](HWND) {};
  access.watch.sequence = [] { return DWORD(7); }; access.watch.owner = []() -> HWND { return nullptr; };
  access.readFormats = [file](HWND, const clip::ReadOptions&) { clip::Snapshot snapshot; snapshot.sequence = 7; snapshot.files = {file}; return snapshot; };
  return access;
}
// The relay's pull queue: four pulls of the PC at once, served in order with a 4 MiB window; one the PC cancels before its turn is skipped.
void pullQueueChecks(const std::filesystem::path& root) {
  const auto file = root / L"src" / L"queued.bin";
  { std::vector<char> bytes(6 * 1024 * 1024); for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = char(i * 7 % 253); std::ofstream out(file, std::ios::binary); out.write(bytes.data(), std::streamsize(bytes.size())); }
  Peer peer;
  ClipboardWorker worker("qa-pull-queue", [&](const std::string& text) { peer.output(text); }, [] { return true; }, root / L"transfers-queue", standIn(file));
  const auto send = [&](const std::string& json) { worker.message(JsonParser::parse(json)); };
  send(readyJson("qa-pull-queue", true));
  const std::string read = "0123456789abcdef0123456789abcdef-0000000000000001";
  send("{\"type\":\"clipboard.read\",\"requestId\":\"" + read + "\"}");
  require(peer.waitFor([&] { return peer.count("clipboard.entries", read) == 1; }), "queue: the copy was not offered");
  send("{\"type\":\"clipboard.complete\",\"requestId\":\"" + read + "\"}");
  const auto pull = [&](const char* id) { send(std::string("{\"type\":\"clipboard.pull\",\"requestId\":\"") + id + "\",\"offerId\":\"" + read + "\",\"index\":0,\"offset\":0}"); };
  pull("pullc-1"); pull("pullc-2");
  // 4 MiB cross without an acknowledgement (91 chunks; a released pair stops at 22).
  require(peer.waitFor([&] { return peer.count("clipboard.chunk", "pullc-1") >= 80; }), "queue: the pull's window is not 4 MiB");
  // The PC no longer wants the second file, which waits behind the first; then it ends the first.
  send("{\"type\":\"clipboard.cancel\",\"requestId\":\"pullc-2\"}");
  send("{\"type\":\"clipboard.cancel\",\"requestId\":\"pullc-1\"}");
  require(peer.waitFor([&] { return peer.stage(ClipboardDiagnostic::peerCancelled) == 1; }), "queue: the first pull did not end");
  pull("pullc-3");
  require(peer.waitFor([&] { return peer.count("clipboard.chunk", "pullc-3") >= 1; }), "queue: the next pull was not served");
  {
    std::lock_guard lock(peer.m);
    require(peer.count("clipboard.chunk", "pullc-2") == 0 && peer.count("clipboard.item.end", "pullc-2") == 0, "queue: a pull the PC cancelled before its turn was served");
    require(peer.count("clipboard.error", "pullc-2") == 0 && peer.stage(ClipboardDiagnostic::pullFailed) == 0, "queue: a skipped pull was reported");
  }
  send("{\"type\":\"clipboard.cancel\",\"requestId\":\"pullc-3\"}");
  require(peer.waitFor([&] { return peer.stage(ClipboardDiagnostic::peerCancelled) == 2; }), "queue: the third pull did not end");
  worker.stop();
  // The item end of a queued pull needs a free unit of its 128-unit window, not of a 32-unit one (with that a file of 32 or more
  // chunks waits for acknowledgements before its end, a stall at the end of every file).
  const auto forty = root / L"src" / L"forty.bin";
  { std::vector<char> bytes(40 * clip::formatsChunkBytes, 'x'); std::ofstream out(forty, std::ios::binary); out.write(bytes.data(), std::streamsize(bytes.size())); }
  Peer endPeer;
  ClipboardWorker endWorker("qa-pull-end", [&](const std::string& text) { endPeer.output(text); }, [] { return true; }, root / L"transfers-end", standIn(forty));
  const auto sendEnd = [&](const std::string& json) { endWorker.message(JsonParser::parse(json)); };
  sendEnd(readyJson("qa-pull-end", true));
  sendEnd("{\"type\":\"clipboard.read\",\"requestId\":\"" + read + "\"}");
  require(endPeer.waitFor([&] { return endPeer.count("clipboard.entries", read) == 1; }), "end: the copy was not offered");
  sendEnd("{\"type\":\"clipboard.complete\",\"requestId\":\"" + read + "\"}");
  sendEnd("{\"type\":\"clipboard.pull\",\"requestId\":\"pullc-1\",\"offerId\":\"" + read + "\",\"index\":0,\"offset\":0}");
  require(endPeer.waitFor([&] { return endPeer.count("clipboard.item.end", "pullc-1") == 1; }), "end: a 40-chunk queued pull waited for acknowledgements before its item end");
  endWorker.stop();
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 2, "Pass a new own QA directory");
    encodingChecks();
    const auto root = std::filesystem::absolute(argv[1]);
    require(!std::filesystem::exists(root) && root.filename().wstring().rfind(L"run-", 0) == 0, "Unsafe QA directory");
    std::filesystem::create_directories(root / L"src");
    fastPathChecks();
    readAheadChecks(root);
    const auto file = root / L"src" / L"copied.bin";
    { std::vector<char> bytes(3 * 1024 * 1024); for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = char(i * 131 % 251); std::ofstream out(file, std::ios::binary); out.write(bytes.data(), std::streamsize(bytes.size())); }

    ClipboardWorkerAccess access;
    const auto refuse = [](const char* what) { return std::runtime_error(std::string("the test must not touch the clipboard: ") + what); };
    access.read = [&](HWND, bool, bool) -> clip::Snapshot { throw refuse("read"); };
    access.putText = [&](HWND, const std::wstring&, std::optional<DWORD>) -> DWORD { throw refuse("putText"); };
    access.putFiles = [&](HWND, const std::vector<std::filesystem::path>&, std::optional<DWORD>) -> DWORD { throw refuse("putFiles"); };
    access.publishOffer = [&](const std::shared_ptr<clip::PullOffer>&, std::optional<DWORD>) -> DWORD { throw refuse("publishOffer"); };
    access.offerCurrent = [](const std::shared_ptr<clip::PullOffer>&) { return false; };
    access.withdrawOffer = [](const std::shared_ptr<clip::PullOffer>&) -> DWORD { return 0; };
    std::atomic<uint32_t> formats{1}; std::atomic<DWORD> sequence{7}; std::atomic<bool> listening{false};
    access.formats = [&] { return formats.load(); };
    access.watch.add = [&](HWND) { listening = true; return true; }; access.watch.remove = [](HWND) {};
    access.watch.sequence = [&] { return sequence.load(); }; access.watch.owner = []() -> HWND { return nullptr; };
    // The VM's copy: this test's own file, as a clipboard read would have found it.
    access.readFormats = [&](HWND, const clip::ReadOptions&) { clip::Snapshot snapshot; snapshot.sequence = 7; snapshot.files = {file}; return snapshot; };

    Peer peer;
    ClipboardWorker worker("qa-pull-cancel", [&](const std::string& text) { peer.output(text); }, [] { return true; }, root / L"transfers", access);
    const auto send = [&](const std::string& json) { worker.message(JsonParser::parse(json)); };
    // An on-paste pair with the sequence-numbered request ids of clipboard sync (the field pair).
    send("{\"type\":\"clipboard.ready\",\"sessionId\":\"qa-pull-cancel\",\"protocol\":\"clipboard-v1\",\"clipboardSync\":true,"
         "\"clipboardPolicy\":{\"textToRemote\":true,\"filesToRemote\":true,\"textToLocal\":true,\"filesToLocal\":true},\"limits\":{"
         "\"scaleProfile\":1,\"limitsProfile\":2,\"formatsProfile\":2,\"pullProfile\":1,\"maxOffers\":" + number(clip::maxLiveOffers) +
         ",\"chunkBytes\":" + number(clip::formatsChunkBytes) + ",\"windowUnits\":" + number(clip::SendWindow::defaultUnits) + ",\"windowBytes\":" + number(clip::SendWindow::defaultBytes) +
         ",\"maxFiles\":" + number(clip::maxEntries) + ",\"maxFileBytes\":" + number(clip::scaleBytes) + ",\"maxTotalBytes\":" + number(clip::scaleBytes) +
         ",\"maxTextBytes\":" + number(clip::maxTextBytes) + ",\"maxTextUnits\":" + number(clip::maxTextUnits) + ",\"maxEntries\":" + number(clip::maxEntries) +
         ",\"maxDepth\":" + number(clip::maxDepth) + ",\"maxPathChars\":" + number(clip::maxPathChars) + ",\"maxRichBytes\":" + number(clip::maxRichBytes) +
         ",\"onPasteProfile\":1,\"pushTextBytes\":" + number(clip::pushTextBytes) + ",\"pushImageBytes\":" + number(clip::pushImageBytes) + "}}");
    // The PC reads the VM's copy: it is offered (manifest only) and kept for pulls.
    const std::string read = "0123456789abcdef0123456789abcdef-0000000000000001";
    send("{\"type\":\"clipboard.read\",\"requestId\":\"" + read + "\"}");
    require(peer.waitFor([&] { return peer.count("clipboard.entries", read) == 1; }), "the copy was not offered");
    send("{\"type\":\"clipboard.complete\",\"requestId\":\"" + read + "\"}");
    // A paste on the PC pulls the file; the window fills (no acknowledgements) and the Host waits.
    send("{\"type\":\"clipboard.pull\",\"requestId\":\"pullc-1\",\"offerId\":\"" + read + "\",\"index\":0,\"offset\":0}");
    require(peer.waitFor([&] { return peer.count("clipboard.chunk", "pullc-1") >= 20; }), "the pull was not served");
    // The user cancels on the PC.
    send("{\"type\":\"clipboard.cancel\",\"requestId\":\"pullc-1\"}");
    require(peer.waitFor([&] { return peer.stage(ClipboardDiagnostic::peerCancelled) == 1; }), "the peer's cancel was not recorded");
    // The same cancel, queued behind the pull it stopped, is not a second failure of that pull.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
      std::lock_guard lock(peer.m);
      require(peer.stage(ClipboardDiagnostic::pullFailed) == 0, "an ended pull's queued cancel was recorded as a failed pull");
      require(peer.count("clipboard.error", "pullc-1") == 0, "an ended pull's queued cancel was answered with clipboard.error");
    }
    // The offer is still served: the next paste pulls it again.
    send("{\"type\":\"clipboard.pull\",\"requestId\":\"pullc-2\",\"offerId\":\"" + read + "\",\"index\":0,\"offset\":0}");
    require(peer.waitFor([&] { return peer.count("clipboard.chunk", "pullc-2") >= 1; }), "the offer was not served again");
    {
      // The chunk built by the emit fast path is the file's bytes, canonical base64 in an ordinary chunk message.
      std::lock_guard lock(peer.m); std::ifstream in(file, std::ios::binary); std::vector<uint8_t> first(clip::formatsChunkBytes); in.read(reinterpret_cast<char*>(first.data()), std::streamsize(first.size()));
      for (const auto& p : peer.packets) if (p.get("type").string() == "clipboard.chunk" && p.get("requestId").string() == "pullc-2" && p.get("offset").number(-1) == 0) {
        require(p.get("index").number(-1) == 0 && clipboardChunk(p.get("data").string(), clip::formatsChunkBytes) == first, "a served chunk does not carry the file's bytes"); }
    }
    {
      // A copy of files is announced after 80 ms of quiet, text after the 250 ms the settled hint promises. The change notification
      // is posted to the worker's own message window; the "clipboard" is the stand-in sequence and formats above.
      send("{\"type\":\"clipboard.cancel\",\"requestId\":\"pullc-2\"}");  // the second paste ends (the worker serves one thing at a time)
      require(peer.waitFor([&] { return peer.stage(ClipboardDiagnostic::peerCancelled) == 2; }), "the second pull did not end");
      send("{\"type\":\"clipboard.watch\",\"sessionId\":\"qa-pull-cancel\",\"enabled\":true}");
      require(peer.waitFor([&] { return listening.load(); }), "the watch did not start");
      const HWND window = FindWindowExW(HWND_MESSAGE, nullptr, L"Multitap.Clipboard.Worker.v1", nullptr); require(window != nullptr, "worker window");
      const auto announced = [&](DWORD value, uint32_t kinds) {
        sequence = value; formats = kinds; const auto start = std::chrono::steady_clock::now();
        require(PostMessageW(window, WM_CLIPBOARDUPDATE, 0, 0) != FALSE, "post change");
        require(peer.waitFor([&] { for (const auto& p : peer.packets) if (p.get("type").string() == "clipboard.changed" && p.get("sequence").number(-1) == double(value)) return true; return false; }), "the change was not announced");
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      };
      const auto files = announced(8, 1), text = announced(9, 8);
      std::cout << "{\"announceMs\":{\"files\":" << files << ",\"text\":" << text << "}}\n";
      require(files < 200, "a file copy waited the text quiet before it was announced");
      require(text >= 240, "a text copy was announced before its settled quiet");
    }
    worker.stop();
    pullQueueChecks(root);
    std::filesystem::remove_all(root);
    std::cout << "{\"passed\":true,\"clipboardTouched\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
