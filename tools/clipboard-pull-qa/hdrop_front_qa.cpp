// A copy in the remote desktop is published on this PC with CF_HDROP for a small set (PullOptions::stagingParent). A clipboard monitor
// that ignores ExcludeClipboardContentFromMonitorProcessing asks for CF_HDROP right after the copy, while the user is still in the
// application's window; answering it would pull every file and show the transfer card on a copy alone. PullOptions::refuseHdrop
// refuses it then, without pulling anything, and a paste from another window still gets the files. Needs no clipboard and no shell:
// the data object is called directly, the offering side is a fake link in this process.
#include "pull.hpp"
#include "ack_window.hpp"
#include "storage.hpp"
#include <objbase.h>
#include <shellapi.h>
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
  std::map<uint64_t, std::unique_ptr<Pull>> pulls; size_t asked = 0;
  ~Link() {
    { std::lock_guard lock(m); for (auto& pull : pulls) pull.second->cancelled = true; }
    cv.notify_all(); for (auto& pull : pulls) if (pull.second->thread.joinable()) pull.second->thread.join();
  }
  size_t requests() { std::lock_guard lock(m); return asked; }
  clip::PullHooks hooks() {
    clip::PullHooks h;
    h.request = [this](uint64_t id, const std::string&, size_t index, uint64_t offset) {
      std::lock_guard lock(m); ++asked;
      auto pull = std::make_unique<Pull>(); pull->window = clip::SendWindow(clip::SendWindow::defaultUnits, clip::SendWindow::defaultBytes); auto* raw = pull.get(); pulls[id] = std::move(pull);
      raw->thread = std::thread([this, raw, id, index, offset] { serve(*raw, id, index, offset); });
    };
    h.cancel = [this](uint64_t id) { std::lock_guard lock(m); if (auto found = pulls.find(id); found != pulls.end()) found->second->cancelled = true; cv.notify_all(); };
    h.ackChunk = [this](uint64_t id, size_t index, uint64_t end) {
      std::lock_guard lock(m); if (auto found = pulls.find(id); found != pulls.end()) { try { found->second->window.ackChunk(index, end); } catch (...) {} }
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
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 2, "usage: clipboard_hdrop_front_qa <private directory>");
    const fs::path root = fs::absolute(fs::path(argv[1]) / L"hdrop-front"); fs::remove_all(root); fs::create_directories(root / L"Folder"); fs::create_directories(root / L"staging");
    require(SUCCEEDED(OleInitialize(nullptr)), "OleInitialize");
    int checks = 0;
    {
      for (int i = 0; i < 3; ++i) { const auto bytes = pseudo(4000 + size_t(i) * 1000, uint32_t(i + 1)); std::ofstream out(root / L"Folder" / (L"f" + std::to_wstring(i) + L".bin"), std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); }
      auto source = std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("h", {root / L"Folder"}, clip::Bounds::scale()));
      std::vector<int64_t> times; for (size_t i = 0; i < source->items().size(); ++i) times.push_back(source->mtimeMs(i));
      std::atomic<bool> front{true};
      clip::PullOptions options; options.explorerProgress = false;
      options.storage = std::make_shared<clip::Storage>(root / L"staging"); options.stagingParent = options.storage->parent();
      options.refuseHdrop = [&front] { return front.load(); };
      Link link; link.source = source;
      auto offer = clip::PullOffer::create("h", source->items(), times, link.hooks(), {}, options); link.offer = offer;
      IDataObject* data = offer->dataObject();
      FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      require(data->QueryGetData(&hdrop) == S_OK, "a small set is offered as CF_HDROP"); ++checks;

      // The application's window is in front: refused at once, nothing pulled.
      STGMEDIUM refused{};
      require(FAILED(data->GetData(&hdrop, &refused)), "CF_HDROP was rendered while the application's window is in front"); ++checks;
      std::this_thread::sleep_for(100ms);
      require(link.requests() == 0, "a refused CF_HDROP pulled files"); ++checks;
      require(data->QueryGetData(&hdrop) == S_OK, "the offer still offers CF_HDROP after a refusal"); ++checks;

      // A paste from another window: the files are pulled and CF_HDROP names the staged folder.
      front = false;
      STGMEDIUM medium{};
      require(SUCCEEDED(data->GetData(&hdrop, &medium)) && medium.tymed == TYMED_HGLOBAL && medium.hGlobal, "CF_HDROP for a paste from another window"); ++checks;
      const auto drop = static_cast<HDROP>(medium.hGlobal);
      require(DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1, "one top-level item"); ++checks;
      wchar_t name[MAX_PATH]{}; require(DragQueryFileW(drop, 0, name, MAX_PATH) > 0 && fs::is_directory(name) && fs::path(name).filename() == L"Folder", "the staged folder"); ++checks;
      size_t files = 0; for (const auto& entry : fs::directory_iterator(name)) files += entry.is_regular_file();
      require(files == 3 && link.requests() == 3, "three files, three pulls"); ++checks;
      ReleaseStgMedium(&medium);

      // In front again: refused, even though a staged copy exists (no pull either way).
      front = true;
      STGMEDIUM again{};
      require(FAILED(data->GetData(&hdrop, &again)) && link.requests() == 3, "CF_HDROP was answered while the application's window is in front"); ++checks;
      data->Release(); offer->shutdown("clipboard_cancelled");
    }
    OleUninitialize();
    std::error_code ignored; fs::remove_all(root, ignored);
    std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"clipboardTouched\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
