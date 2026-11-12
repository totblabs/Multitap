// A paste reads the files of a pulled tree one after another (the shell's copy engine does, in descriptor order), and every pull is a
// round trip the relay allows one at a time: a folder of ~700 small files pastes at ~27 ms per file that way. The receiver asks
// for the next small file while the current one is read, and a reader that then asks for exactly that file takes it over. Needs no
// clipboard and no shell: the data object is called directly, the offering side is a fake link in this process.
#include "pull.hpp"
#include "ack_window.hpp"
#include <objbase.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace clip = multitap::clipboard;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class P> bool waitFor(P predicate, std::chrono::milliseconds limit = 3000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) { if (predicate()) return true; std::this_thread::sleep_for(2ms); }
  return predicate();
}
std::vector<uint8_t> pseudo(size_t count, uint32_t seed) {
  std::vector<uint8_t> out(count); uint32_t x = seed * 2654435761u + 7;
  for (auto& b : out) { x = x * 1664525u + 1013904223u; b = uint8_t(x >> 24); }
  return out;
}
// The offering peer and the relay: serves each pull of the SourceOffer through the sliding window, acknowledged by consumption.
struct Link {
  std::shared_ptr<clip::SourceOffer> source; std::weak_ptr<clip::PullOffer> offer;
  std::mutex m; std::condition_variable cv;
  struct Pull { clip::SendWindow window; bool cancelled = false; std::thread thread; };
  std::map<uint64_t, std::unique_ptr<Pull>> pulls; std::vector<std::pair<size_t, uint64_t>> asked; size_t cancels = 0, acks = 0;
  size_t windowUnits = clip::SendWindow::defaultUnits; uint64_t windowBytes = clip::SendWindow::defaultBytes;  // a pull queue: 128 / 4 MiB
  ~Link() {
    { std::lock_guard lock(m); for (auto& pull : pulls) pull.second->cancelled = true; }
    cv.notify_all(); for (auto& pull : pulls) if (pull.second->thread.joinable()) pull.second->thread.join();
  }
  size_t askedFor(size_t index) { std::lock_guard lock(m); size_t n = 0; for (const auto& a : asked) n += a.first == index; return n; }
  size_t requests() { std::lock_guard lock(m); return asked.size(); }
  clip::PullHooks hooks() {
    clip::PullHooks h;
    h.request = [this](uint64_t id, const std::string&, size_t index, uint64_t offset) {
      std::lock_guard lock(m); asked.emplace_back(index, offset);
      auto pull = std::make_unique<Pull>(); pull->window = clip::SendWindow(windowUnits, windowBytes); auto* raw = pull.get(); pulls[id] = std::move(pull);
      raw->thread = std::thread([this, raw, id, index, offset] { serve(*raw, id, index, offset); });
    };
    h.cancel = [this](uint64_t id) { std::lock_guard lock(m); ++cancels; if (auto found = pulls.find(id); found != pulls.end()) found->second->cancelled = true; cv.notify_all(); };
    h.ackChunk = [this](uint64_t id, size_t index, uint64_t end) {
      std::lock_guard lock(m); ++acks; if (auto found = pulls.find(id); found != pulls.end()) { try { found->second->window.ackChunk(index, end); } catch (...) {} }
      cv.notify_all();
    };
    return h;
  }
  void serve(Pull& pull, uint64_t id, size_t index, uint64_t offset) {
    try {
      auto reader = source->open(index, offset); auto target = offer.lock(); if (!target) return;
      while (reader->position() < reader->size()) {
        const size_t n = size_t(std::min<uint64_t>(clip::formatsChunkBytes, reader->size() - reader->position()));
        { std::unique_lock lock(m); cv.wait(lock, [&] { return pull.cancelled || pull.window.canSend(n); }); if (pull.cancelled) return; }
        const uint64_t at = reader->position(); auto bytes = reader->next(n);
        { std::lock_guard lock(m); pull.window.sentChunk(index, reader->position(), bytes.size()); }
        target->deliver(id, at, bytes);
      }
      if (!pull.cancelled) target->finish(id, reader->digest());
    } catch (const std::exception& error) { if (auto target = offer.lock()) target->fail(id, error.what()); }
  }
};
IStream* streamOf(IDataObject* data, LONG lindex) {
  FORMATETC fe{CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILECONTENTS)), nullptr, DVASPECT_CONTENT, lindex, TYMED_ISTREAM}; STGMEDIUM medium{};
  require(SUCCEEDED(data->GetData(&fe, &medium)) && medium.tymed == TYMED_ISTREAM && medium.pstm, "FileContents stream");
  return medium.pstm;
}
std::vector<uint8_t> drain(IStream* stream) {
  std::vector<uint8_t> out, buffer(65536);
  for (;;) { ULONG got = 0; const HRESULT result = stream->Read(buffer.data(), ULONG(buffer.size()), &got); require(SUCCEEDED(result), "read failed"); if (!got) return out; out.insert(out.end(), buffer.begin(), buffer.begin() + got); }
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 2, "usage: clipboard_prefetch_qa <private directory>");
    const fs::path root = fs::path(argv[1]) / L"prefetch"; fs::remove_all(root); fs::create_directories(root / L"Folder");
    require(SUCCEEDED(OleInitialize(nullptr)), "OleInitialize");
    int checks = 0;
    {
      const std::vector<std::pair<std::wstring, std::vector<uint8_t>>> files = {
        {L"1a.bin", pseudo(10000, 1)}, {L"2b.bin", pseudo(20000, 2)}, {L"3c.bin", pseudo(2 * 1024 * 1024 + 5, 3)}, {L"4d.bin", pseudo(5000, 4)}};
      for (const auto& [name, bytes] : files) { std::ofstream out(root / L"Folder" / name, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); }
      auto source = std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("o", {root / L"Folder"}, clip::Bounds::scale()));
      Link link; link.source = source;
      std::vector<int64_t> times; for (size_t i = 0; i < source->items().size(); ++i) times.push_back(source->mtimeMs(i));
      auto offer = clip::PullOffer::create("o", source->items(), times, link.hooks()); link.offer = offer;
      IDataObject* data = offer->dataObject();
      // Entry (descriptor index) and file ordinal of each file.
      std::map<std::wstring, std::pair<LONG, size_t>> at; { size_t ordinal = 0; for (size_t i = 0; i < source->items().size(); ++i) if (source->items()[i].kind == "file") at[fs::path(source->items()[i].name).filename().wstring()] = {LONG(i), ordinal++}; }
      const auto read = [&](const std::wstring& name) { IStream* stream = streamOf(data, at[name].first); auto bytes = drain(stream); stream->Release(); return bytes; };
      const auto expected = [&](const std::wstring& name) { for (const auto& [n, b] : files) if (n == name) return b; throw std::runtime_error("no file"); };

      require(read(L"1a.bin") == expected(L"1a.bin"), "first file"); ++checks;
      // The next small file is asked for while nobody reads it yet.
      require(waitFor([&] { return link.askedFor(at[L"2b.bin"].second) == 1; }), "the next small file was not asked for ahead of its reader"); ++checks;
      require(waitFor([&] { return offer->deliveredBytes() == 30000; }), "the prefetched file arrived"); ++checks;
      // Its reader takes it over: the same bytes, and no second pull of it.
      require(read(L"2b.bin") == expected(L"2b.bin"), "prefetched file content"); ++checks;
      require(link.askedFor(at[L"2b.bin"].second) == 1, "the prefetched file was pulled again"); ++checks;
      // A file above the window is never asked for ahead: it waits for its reader (the sender would wait for acknowledgements).
      std::this_thread::sleep_for(100ms);
      require(link.askedFor(at[L"3c.bin"].second) == 0, "a large file was asked for ahead"); ++checks;
      require(read(L"3c.bin") == expected(L"3c.bin") && link.askedFor(at[L"3c.bin"].second) == 1, "large file read by its reader"); ++checks;
      require(waitFor([&] { return link.askedFor(at[L"4d.bin"].second) == 1; }), "the small file after a large one was not asked for ahead"); ++checks;
      // A reader that asks for another file instead: the guess is dropped, that file is pulled, and nothing chains without readers.
      require(waitFor([&] { return offer->deliveredBytes() == 30000 + files[2].second.size() + 5000; }), "the last prefetch arrived"); ++checks;
      const auto before = link.requests();
      require(read(L"1a.bin") == expected(L"1a.bin"), "a file read again"); ++checks;
      require(link.askedFor(at[L"1a.bin"].second) == 2, "the file read again was pulled again"); ++checks;
      require(waitFor([&] { return link.askedFor(at[L"2b.bin"].second) == 2; }) && link.requests() == before + 2, "one prefetch after the file read again"); ++checks;
      std::this_thread::sleep_for(100ms);
      require(link.requests() == before + 2, "a prefetch nobody took over started another"); ++checks;
      // The user cancels the paste: the prefetched file goes and a stream asking for it right after is refused.
      offer->abortRenders("clipboard_cancelled", true);
      FORMATETC fe{CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILECONTENTS)), nullptr, DVASPECT_CONTENT, at[L"2b.bin"].first, TYMED_ISTREAM}; STGMEDIUM medium{};
      require(SUCCEEDED(data->GetData(&fe, &medium)), "stream after cancel");
      uint8_t buffer[16]; ULONG got = 0; const HRESULT result = medium.pstm->Read(buffer, sizeof(buffer), &got); medium.pstm->Release();
      require(FAILED(result) && got == 0, "a cancelled paste still took the prefetched file"); ++checks;
      data->Release(); offer->shutdown("clipboard_cancelled");
    }
    // ---- A pull-queue pair: up to four files are asked for ahead, while the reader's own file still arrives ----------------
    {
      fs::create_directories(root / L"Many");
      std::vector<std::vector<uint8_t>> files; for (int i = 0; i < 8; ++i) { files.push_back(pseudo(10000 + 1000 * size_t(i), 10 + i)); std::ofstream out(root / L"Many" / (L"f" + std::to_wstring(i) + L".bin"), std::ios::binary); out.write(reinterpret_cast<const char*>(files.back().data()), std::streamsize(files.back().size())); }
      auto source = std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("q", {root / L"Many"}, clip::Bounds::scale()));
      std::vector<int64_t> times; for (size_t i = 0; i < source->items().size(); ++i) times.push_back(source->mtimeMs(i));
      std::map<size_t, LONG> entryOf; { size_t ordinal = 0; for (size_t i = 0; i < source->items().size(); ++i) if (source->items()[i].kind == "file") entryOf[ordinal++] = LONG(i); }
      const auto ordinalOf = [&](size_t file) { for (size_t i = 0, ordinal = 0; i < source->items().size(); ++i) if (source->items()[i].kind == "file") { if (fs::path(source->items()[i].name).filename().wstring() == L"f" + std::to_wstring(file) + L".bin") return ordinal; ++ordinal; } throw std::runtime_error("no file"); };
      clip::PullOptions queue; queue.prefetchDepth = 4;
      {
        Link link; link.source = source; auto offer = clip::PullOffer::create("q", source->items(), times, link.hooks(), {}, queue); link.offer = offer; IDataObject* data = offer->dataObject();
        IStream* first = streamOf(data, entryOf[ordinalOf(0)]); uint8_t one = 0; ULONG got = 0; require(SUCCEEDED(first->Read(&one, 1, &got)) && got == 1, "first read");
        // Reading the first file asks for the next three at once (four outstanding), not one file at a time.
        require(waitFor([&] { return link.askedFor(ordinalOf(1)) == 1 && link.askedFor(ordinalOf(2)) == 1 && link.askedFor(ordinalOf(3)) == 1; }), "a pull queue did not ask for the next files ahead"); ++checks;
        drain(first); first->Release();
        for (size_t i = 1; i < files.size(); ++i) { IStream* stream = streamOf(data, entryOf[ordinalOf(i)]); require(drain(stream) == files[i], "content of a file asked for ahead"); stream->Release(); }
        for (size_t i = 0; i < files.size(); ++i) require(link.askedFor(ordinalOf(i)) == 1, "a file was pulled twice");
        require(link.requests() == files.size() && link.cancels == 0, "eight files, eight pulls, nothing cancelled"); ++checks;
        data->Release(); offer->shutdown("clipboard_cancelled");
      }
      {
        // The reader skips ahead: the files asked for in vain go (those still arriving are cancelled), the reader's file is pulled at once.
        Link link; link.source = source; auto offer = clip::PullOffer::create("q", source->items(), times, link.hooks(), {}, queue); link.offer = offer; IDataObject* data = offer->dataObject();
        IStream* first = streamOf(data, entryOf[ordinalOf(0)]); require(drain(first) == files[0], "first file"); first->Release();
        require(waitFor([&] { return link.askedFor(ordinalOf(4)) == 1; }), "four files ahead");
        IStream* later = streamOf(data, entryOf[ordinalOf(6)]); require(drain(later) == files[6], "a file further on"); later->Release();
        require(link.askedFor(ordinalOf(6)) == 1 && link.askedFor(ordinalOf(7)) == 1, "the skipped-to file and the one after it");
        IStream* again = streamOf(data, entryOf[ordinalOf(1)]); require(drain(again) == files[1], "a dropped prefetch is pulled again when read"); again->Release();
        require(link.askedFor(ordinalOf(1)) == 2, "a dropped prefetch was taken over"); ++checks;
        data->Release(); offer->shutdown("clipboard_cancelled");
      }
    }
    // ---- Acknowledgement rhythm: a pull queue's 4 MiB window is acknowledged every ~1 MiB, a 1 MiB window without a pull queue
    // every 128 KiB. Same bytes either way.
    {
      fs::create_directories(root / L"Big"); const auto big = pseudo(66 * clip::formatsChunkBytes, 99);
      { std::ofstream out(root / L"Big" / L"big.bin", std::ios::binary); out.write(reinterpret_cast<const char*>(big.data()), std::streamsize(big.size())); }
      auto source = std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("a", {root / L"Big" / L"big.bin"}, clip::Bounds::scale()));
      std::vector<int64_t> times; for (size_t i = 0; i < source->items().size(); ++i) times.push_back(source->mtimeMs(i));
      const auto acksOf = [&](size_t depth) {
        Link link; link.source = source; clip::PullOptions options; options.prefetchDepth = depth;
        if (depth > 1) { link.windowUnits = 128; link.windowBytes = 4ull * 1024 * 1024; }
        auto offer = clip::PullOffer::create("a", source->items(), times, link.hooks(), {}, options); link.offer = offer; IDataObject* data = offer->dataObject();
        IStream* stream = streamOf(data, 0); require(drain(stream) == big, "content with the acknowledgement rhythm"); stream->Release();
        data->Release(); offer->shutdown("clipboard_cancelled");
        std::lock_guard lock(link.m); return link.acks;
      };
      const size_t queued = acksOf(4), released = acksOf(1);
      std::cout << "{\"acksPer66Chunks\":{\"pullQueue\":" << queued << ",\"released\":" << released << "}}\n";
      require(queued >= 1 && queued <= 3, "a pull queue's window was acknowledged more often than every ~1 MiB"); ++checks;
      require(released >= 15, "a released window was not acknowledged every 128 KiB"); ++checks;
    }
    fs::remove_all(root); OleUninitialize();
    std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"clipboardTouched\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
