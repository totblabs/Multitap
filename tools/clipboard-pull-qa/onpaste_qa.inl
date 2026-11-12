// Included by clipboard_pull_qa.cpp after its helpers (Link, Fixture, require, ...): the on-paste revision of native/windows_clipboard
// (a copy is silent, data moves when pasted). Offers of text, rich text and images rendered on demand,
// CF_HDROP as a blocking compatibility format of a file offer, cancel / newer copy / session end while GetData waits, and the real shell's
// folder drop target against a data object with and without FD_PROGRESSUI. No clipboard is touched: the data objects are called directly.
namespace {

HGLOBAL getGlobal(IDataObject* data, CLIPFORMAT cf, HRESULT& result) {
  FORMATETC fe{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
  result = data->GetData(&fe, &medium);
  return SUCCEEDED(result) && medium.tymed == TYMED_HGLOBAL ? medium.hGlobal : nullptr;
}
std::vector<uint8_t> bytesOf(HGLOBAL memory) {
  const auto size = GlobalSize(memory); const auto* p = static_cast<const uint8_t*>(GlobalLock(memory));
  std::vector<uint8_t> out(p, p + size); GlobalUnlock(memory); GlobalFree(memory); return out;
}
std::wstring unicodeOf(HGLOBAL memory) {
  const auto size = GlobalSize(memory); const auto* p = static_cast<const wchar_t*>(GlobalLock(memory));
  std::wstring out(p, size / sizeof(wchar_t)); GlobalUnlock(memory); GlobalFree(memory);
  while (!out.empty() && out.back() == L'\0') out.pop_back();
  return out;
}
std::vector<uint8_t> htmlOf(const std::string& fragment) {
  const std::string prefix = "<html><body><!--StartFragment-->", suffix = "<!--EndFragment--></body></html>";
  const auto header = [](size_t a, size_t b, size_t c, size_t d) { char buffer[160]; snprintf(buffer, sizeof(buffer), "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n", a, b, c, d); return std::string(buffer); };
  const size_t headerSize = header(0, 0, 0, 0).size(), start = headerSize + prefix.size(), end = start + fragment.size();
  const std::string whole = header(headerSize, end + suffix.size(), start, end) + prefix + fragment + suffix;
  return std::vector<uint8_t>(whole.begin(), whole.end());
}
// A 64 x 48 24-bit bottom-up DIB: b = 4x, g = 5y (from the top), r = x + y.
std::vector<uint8_t> dibOf() {
  const int width = 64, height = 48, stride = width * 3; std::vector<uint8_t> dib(sizeof(BITMAPINFOHEADER) + size_t(stride) * height);
  auto* header = reinterpret_cast<BITMAPINFOHEADER*>(dib.data()); header->biSize = sizeof(BITMAPINFOHEADER); header->biWidth = width; header->biHeight = height; header->biPlanes = 1; header->biBitCount = 24; header->biCompression = BI_RGB;
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    uint8_t* pixel = dib.data() + sizeof(BITMAPINFOHEADER) + size_t(height - 1 - y) * stride + size_t(x) * 3;
    pixel[0] = uint8_t(x * 4); pixel[1] = uint8_t(y * 5); pixel[2] = uint8_t(x + y);
  }
  return dib;
}

// An offer of copied content (text, rich text, image) and the receiver that renders it on demand.
struct ContentSide {
  std::shared_ptr<clip::SourceOffer> source; Link link; std::shared_ptr<clip::PullOffer> offer; IDataObject* data = nullptr;
  explicit ContentSide(std::unique_ptr<clip::SourceOffer> made, clip::PullTimeouts timeouts = {}) : source(std::move(made)) {
    link.source = source;
    offer = clip::PullOffer::createContent("c1", source->format(), source->items(), link.hooks(), timeouts);
    link.offer = offer; data = offer->dataObject();
  }
  ~ContentSide() { if (data) data->Release(); }
};
std::unique_ptr<clip::SourceOffer> textOffer(const std::wstring& text, const std::vector<uint8_t>* html = nullptr) {
  clip::Snapshot snapshot; snapshot.text = text; if (html) snapshot.html = *html;
  return clip::SourceOffer::fromContent("c1", snapshot, clip::Bounds::scale(), false);
}
// The data formats an offer lists. Every offer also lists the three privacy markers (Windows clipboard history,
// cloud clipboard and clipboard monitors leave it alone); they are checked here and left out.
std::set<CLIPFORMAT> formatsOf(IDataObject* data) {
  IEnumFORMATETC* enumerator = nullptr; require(SUCCEEDED(data->EnumFormatEtc(DATADIR_GET, &enumerator)), "enum");
  std::set<CLIPFORMAT> listed; FORMATETC each{}; ULONG fetched = 0; while (enumerator->Next(1, &each, &fetched) == S_OK) listed.insert(each.cfFormat);
  enumerator->Release();
  for (const wchar_t* name : {L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
    const auto marker = CLIPFORMAT(RegisterClipboardFormatW(name));
    require(listed.erase(marker) == 1, "an offer does not list a clipboard privacy marker");
  }
  return listed;
}
size_t transferFolders(const fs::path& parent) {
  size_t count = 0; std::error_code ec;
  for (const auto& e : fs::directory_iterator(parent, ec)) if (e.is_directory(ec) && e.path().filename().wstring().rfind(L"transfer-", 0) == 0) ++count;
  return count;
}
std::vector<std::wstring> dropPaths(HGLOBAL memory) {
  const auto* base = static_cast<const uint8_t*>(GlobalLock(memory)); DROPFILES header{}; std::memcpy(&header, base, sizeof(header));
  require(header.fWide == TRUE, "CF_HDROP is wide");
  std::vector<std::wstring> paths; const auto* at = reinterpret_cast<const wchar_t*>(base + header.pFiles);
  while (*at) { paths.emplace_back(at); at += paths.back().size() + 1; }
  GlobalUnlock(memory); GlobalFree(memory); return paths;
}
// Runs `render` on its own thread and returns its HRESULT and how long it took.
struct Timed { HRESULT result = E_FAIL; std::chrono::milliseconds took{0}; };
template <class F> std::thread inThread(Timed& out, F&& render) {
  return std::thread([&out, render = std::forward<F>(render)]() mutable { const auto start = std::chrono::steady_clock::now(); out.result = render(); out.took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start); });
}
std::vector<HWND> topWindowsOfClass(const wchar_t* className) {
  struct Search { const wchar_t* name; std::vector<HWND> found; } search{className, {}};
  EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
    auto* s = reinterpret_cast<Search*>(parameter); wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (std::wstring(name) == s->name && IsWindowVisible(window)) s->found.push_back(window);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.found;
}
// An IDataObject that logs what the shell asks for and passes everything on.
class LoggingData final : public IDataObject {
 public:
  explicit LoggingData(IDataObject* inner) : inner_(inner) { inner_->AddRef(); }
  ~LoggingData() { inner_->Release(); }
  std::mutex m; std::vector<std::string> log, timeline;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override { if (id == __uuidof(IUnknown) || id == __uuidof(IDataObject)) { *out = static_cast<IDataObject*>(this); AddRef(); return S_OK; } return inner_->QueryInterface(id, out); }
  STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
  STDMETHODIMP_(ULONG) Release() override { const LONG left = InterlockedDecrement(&refs_); if (left == 0) delete this; return ULONG(left); }
  STDMETHODIMP GetData(FORMATETC* f, STGMEDIUM* medium) override {
    note("GetData", f); const auto start = std::chrono::steady_clock::now(); const HRESULT r = inner_->GetData(f, medium);
    // The timeline keeps the calls that took 50 ms or more or failed: what the shell waited for, in order.
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (took >= 50 || FAILED(r)) {
      wchar_t name[128]{}; if (f && f->cfFormat >= 0xC000) GetClipboardFormatNameW(f->cfFormat, name, 128);
      char hr[16]; snprintf(hr, sizeof(hr), "%08x", unsigned(r));
      std::lock_guard lock(m); timeline.push_back(std::string("GetData ") + (f && f->cfFormat == CF_HDROP ? "CF_HDROP" : narrow(name)) + (f && f->lindex >= 0 ? "[" + std::to_string(f->lindex) + "]" : "") + " hr=0x" + hr + " " + std::to_string(took) + "ms");
    }
    return r;
  }
  STDMETHODIMP GetDataHere(FORMATETC* f, STGMEDIUM* medium) override { note("GetDataHere", f); return inner_->GetDataHere(f, medium); }
  STDMETHODIMP QueryGetData(FORMATETC* f) override { note("QueryGetData", f); return inner_->QueryGetData(f); }
  STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* a, FORMATETC* b) override { return inner_->GetCanonicalFormatEtc(a, b); }
  STDMETHODIMP SetData(FORMATETC* f, STGMEDIUM* medium, BOOL release) override { note("SetData", f); return inner_->SetData(f, medium, release); }
  STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override { return inner_->EnumFormatEtc(direction, out); }
  STDMETHODIMP DAdvise(FORMATETC* a, DWORD b, IAdviseSink* c, DWORD* d) override { return inner_->DAdvise(a, b, c, d); }
  STDMETHODIMP DUnadvise(DWORD a) override { return inner_->DUnadvise(a); }
  STDMETHODIMP EnumDAdvise(IEnumSTATDATA** a) override { return inner_->EnumDAdvise(a); }
 private:
  void note(const char* call, FORMATETC* f) {
    wchar_t name[128]{}; if (f && f->cfFormat >= 0xC000) GetClipboardFormatNameW(f->cfFormat, name, 128);
    std::string label = f ? (f->cfFormat == CF_HDROP ? "CF_HDROP" : f->cfFormat >= 0xC000 ? narrow(name) : std::to_string(f->cfFormat)) : "?";
    std::lock_guard lock(m); log.push_back(std::string(call) + " " + label + (f && f->lindex >= 0 ? "[" + std::to_string(f->lindex) + "]" : ""));
  }
  IDataObject* inner_; LONG refs_ = 1;
};
void pumpFor(std::chrono::milliseconds span) {
  const auto end = std::chrono::steady_clock::now() + span; MSG message;
  while (std::chrono::steady_clock::now() < end) { while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0); }
}

void contentChecks(int& checks) {
  // ---- What is pushed and what is offered ----------------------------------------------------------------
  {
    clip::Snapshot tinyText; tinyText.text = std::wstring(100, L'x');
    require(!clip::SourceOffer::fromContent("c", tinyText, clip::Bounds::scale(), true), "a small text is pushed, not offered");
    clip::Snapshot edge; edge.text = std::wstring(size_t(clip::pushTextBytes), L'x');
    require(!clip::SourceOffer::fromContent("c", edge, clip::Bounds::scale(), true), "exactly 1 MiB of text is still pushed");
    edge.text->push_back(L'x');
    auto big = clip::SourceOffer::fromContent("c", edge, clip::Bounds::scale(), true);
    require(big && big->format() == "text" && big->items().size() == 1 && big->items()[0].size == clip::pushTextBytes + 1, "1 MiB + 1 is offered");
    // Text and HTML together decide: a small text with 2 MiB of HTML is offered as rich text.
    clip::Snapshot rich; rich.text = L"hello"; rich.html = htmlOf(std::string(2 * 1024 * 1024, 'a'));
    auto richOffer = clip::SourceOffer::fromContent("c", rich, clip::Bounds::scale(), true);
    require(richOffer && richOffer->format() == "richtext" && richOffer->items().size() == 2 && richOffer->items()[1].kind == "html", "rich text is judged by text plus HTML");
    clip::Snapshot picture; picture.image = clip::Image{std::vector<uint8_t>(size_t(clip::pushImageBytes), 7), 100, 100};
    require(!clip::SourceOffer::fromContent("c", picture, clip::Bounds::scale(), true), "an image of 8 MiB is pushed");
    picture.image->png.push_back(7);
    auto imageOffer = clip::SourceOffer::fromContent("c", picture, clip::Bounds::scale(), true);
    require(imageOffer && imageOffer->format() == "image" && imageOffer->items()[0].width == 100, "8 MiB + 1 is offered");
    clip::Snapshot nothing; require(!clip::SourceOffer::fromContent("c", nothing, clip::Bounds::scale(), false), "no content, no offer");
    clip::Bounds tight = clip::Bounds::scale(); tight.textBytes = 10;
    expectReason(reason([&] { clip::SourceOffer::fromContent("c", edge, tight, false); }), "clipboard_text_too_large", "a text above the pair's limit");
    // Files are always offered in an on-paste pair, whatever their size.
    clip::TreeScan tiny; tiny.bytes = 3; tiny.tops = 1; tiny.entries.resize(1); tiny.entries[0].relative = L"a.txt";
    require(!clip::shouldOffer(tiny) && clip::shouldOffer(tiny, true), "a tiny file is offered with the on-paste revision only");
    tiny.entries[0].relative = std::wstring(300, L'a'); require(!clip::shouldOffer(tiny, true), "a path a descriptor cannot hold is never offered");
    checks += 11;
  }
  // ---- A text offer: nothing at copy time, the whole text on the first GetData, reused afterwards ------------
  {
    std::wstring text; for (int i = 0; text.size() < 3 * 1024 * 1024; ++i) text += L"line " + std::to_wstring(i) + L" 한글 text\r\n";
    ContentSide side(textOffer(text));
    require(formatsOf(side.data) == std::set<CLIPFORMAT>{CF_UNICODETEXT}, "a text offer lists CF_UNICODETEXT only");
    FORMATETC wanted{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}, hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    require(side.data->QueryGetData(&wanted) == S_OK && side.data->QueryGetData(&hdrop) == DV_E_FORMATETC, "QueryGetData");
    IDataObjectAsyncCapability* async = nullptr; require(side.data->QueryInterface(IID_PPV_ARGS(&async)) == E_NOINTERFACE, "no asynchronous extraction for text");
    require(side.link.requests == 0 && side.offer->deliveredBytes() == 0, "announcing pulls nothing");
    HRESULT result = S_OK; const auto start = std::chrono::steady_clock::now();
    HGLOBAL memory = getGlobal(side.data, CF_UNICODETEXT, result); require(memory != nullptr, "GetData(CF_UNICODETEXT)");
    const auto took = std::chrono::steady_clock::now() - start;
    require(unicodeOf(memory) == text, "the rendered text equals the copied text");
    require(side.link.requests == 1 && side.link.sentBytes > 3 * 1024 * 1024, "one pull of the whole text");
    require(waitFor([&] { return side.link.waitOff == 1; }) && side.link.waitOn == 1, "the paste waited once and then no longer");
    const auto requests = side.link.requests.load();
    memory = getGlobal(side.data, CF_UNICODETEXT, result); require(memory && unicodeOf(memory) == text && side.link.requests == requests, "a second GetData reuses the pulled text");
    std::cout << "{\"measure\":{\"textRenderMs\":" << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << ",\"bytes\":" << side.link.sentBytes.load() << "}}\n";
    checks += 8;
  }
  // ---- Rich text: the text renders without the HTML; the HTML is sanitised again ----------------------------
  {
    const auto html = htmlOf("<p onclick=\"x()\">hi <b>there</b></p><script>alert(1)</script><a href=\"javascript:alert(2)\">x</a>");
    ContentSide side(textOffer(L"hi there", &html));
    const CLIPFORMAT htmlFormat = CLIPFORMAT(RegisterClipboardFormatW(L"HTML Format"));
    require(formatsOf(side.data) == std::set<CLIPFORMAT>{CF_UNICODETEXT, htmlFormat}, "rich text lists text and HTML");
    HRESULT result = S_OK; auto memory = getGlobal(side.data, CF_UNICODETEXT, result);
    require(memory && unicodeOf(memory) == L"hi there" && side.link.requests == 1, "the text alone is pulled for CF_UNICODETEXT");
    memory = getGlobal(side.data, htmlFormat, result); require(memory != nullptr && side.link.requests == 2, "the HTML is pulled when asked for");
    const auto clean = bytesOf(memory); const std::string cleaned(clean.begin(), clean.end());
    require(cleaned.find("<b>there</b>") != std::string::npos && cleaned.find("script") == std::string::npos && cleaned.find("onclick") == std::string::npos && cleaned.find("javascript") == std::string::npos, "scripts, handlers and javascript: links never reach the clipboard");
    clip::validateHtmlFormat(std::vector<uint8_t>(clean.begin(), clean.end() - 1));
    require(clean.back() == 0, "the HTML is NUL-terminated");
    // A peer whose HTML is not CF_HTML at all (no header): that format fails, the text still works.
    clip::Snapshot garbage; garbage.text = L"t"; garbage.html = std::vector<uint8_t>{'<', 'p', '>'};
    auto source = clip::SourceOffer::fromContent("c1", garbage, clip::Bounds::scale(), false);
    ContentSide bad(std::move(source));
    require(getGlobal(bad.data, htmlFormat, result) == nullptr && FAILED(result), "an unusable HTML fails that format");
    require(getGlobal(bad.data, CF_UNICODETEXT, result) != nullptr, "and does not take the text with it");
    checks += 8;
  }
  // ---- An image offer: PNG as is, the bitmap formats decoded from it -----------------------------------------
  {
    const auto dib = dibOf(); const auto image = clip::encodePngFromDib(dib.data(), dib.size());
    clip::Snapshot snapshot; snapshot.image = image;
    ContentSide side(clip::SourceOffer::fromContent("c1", snapshot, clip::Bounds::scale(), false));
    const CLIPFORMAT pngFormat = CLIPFORMAT(RegisterClipboardFormatW(L"PNG"));
    require(formatsOf(side.data) == std::set<CLIPFORMAT>{pngFormat, CF_DIBV5, CF_DIB}, "an image lists PNG, CF_DIBV5 and CF_DIB");
    HRESULT result = S_OK; auto memory = getGlobal(side.data, pngFormat, result);
    require(memory && bytesOf(memory) == image.png, "PNG is the pulled PNG");
    memory = getGlobal(side.data, CF_DIBV5, result); require(memory != nullptr, "CF_DIBV5");
    {
      const auto* base = static_cast<const uint8_t*>(GlobalLock(memory)); const auto* header = reinterpret_cast<const BITMAPV5HEADER*>(base);
      require(header->bV5Width == 64 && header->bV5Height == 48 && header->bV5BitCount == 32 && header->bV5AlphaMask == 0xff000000, "DIBV5 header");
      const uint8_t* pixel = base + sizeof(BITMAPV5HEADER) + (size_t(48 - 1 - 5) * 64 + 10) * 4;   // x = 10, y = 5 from the top
      require(pixel[0] == 40 && pixel[1] == 25 && pixel[2] == 15 && pixel[3] == 255, "DIBV5 pixel (bottom-up, straight alpha)");
      GlobalUnlock(memory); GlobalFree(memory);
    }
    memory = getGlobal(side.data, CF_DIB, result); require(memory != nullptr, "CF_DIB");
    {
      const auto* base = static_cast<const uint8_t*>(GlobalLock(memory)); const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(base);
      require(header->biWidth == 64 && header->biHeight == 48 && header->biBitCount == 24, "DIB header");
      const uint8_t* pixel = base + sizeof(BITMAPINFOHEADER) + size_t(48 - 1 - 5) * 64 * 3 + 10 * 3;
      require(pixel[0] == 40 && pixel[1] == 25 && pixel[2] == 15, "DIB pixel");
      GlobalUnlock(memory); GlobalFree(memory);
    }
    checks += 7;
  }
  // ---- Blocking, cancelling, replaced, revoked -------------------------------------------------------------------
  {
    std::wstring text; for (int i = 0; text.size() < 2 * 1024 * 1024; ++i) text += L"row " + std::to_wstring(i) + L"\r\n";
    // The user cancels while the text is still crossing: GetData fails promptly; the offer stays and can be pasted again.
    {
      ContentSide side(textOffer(text)); side.link.bytesPerSecond = 1'000'000;
      Timed timed; auto render = inThread(timed, [&] { HRESULT r = S_OK; HGLOBAL m = getGlobal(side.data, CF_UNICODETEXT, r); if (m) GlobalFree(m); return r; });
      require(waitFor([&] { return side.link.sentBytes > 100000; }), "the pull is running");
      side.offer->abortRenders("clipboard_cancelled", false);
      render.join();
      require(timed.result == E_ABORT && timed.took < 3000ms, "a cancel fails the blocked GetData cleanly (E_ABORT) and promptly");
      require(waitFor([&] { return side.link.cancels >= 1; }), "the offering side is told to stop");
      require(!side.offer->rendering(), "nothing is rendering any more");
      side.link.bytesPerSecond = 0; const auto before = side.link.requests.load(); HRESULT result = S_OK;
      auto memory = getGlobal(side.data, CF_UNICODETEXT, result);
      require(memory && unicodeOf(memory) == text && side.link.requests == before + 1, "the offer survives a cancel: pasting again pulls it again");
      checks += 5;
    }
    // Another copy replaced the clipboard content: the blocked GetData gives up, no stale text is handed over.
    {
      ContentSide side(textOffer(text)); side.link.bytesPerSecond = 1'000'000; std::atomic<bool> current{true};
      side.offer->setCurrentCheck([&] { return current.load(); });
      Timed timed; auto render = inThread(timed, [&] { HRESULT r = S_OK; HGLOBAL m = getGlobal(side.data, CF_UNICODETEXT, r); if (m) GlobalFree(m); return r; });
      require(waitFor([&] { return side.link.sentBytes > 100000; }), "pull running");
      current = false; render.join();
      require(timed.result == E_ABORT && timed.took < 3000ms, "a newer copy ends the blocked GetData");
      checks += 1;
    }
    // The session ends or the permission is withdrawn.
    {
      ContentSide side(textOffer(text)); side.link.bytesPerSecond = 1'000'000;
      Timed timed; auto render = inThread(timed, [&] { HRESULT r = S_OK; HGLOBAL m = getGlobal(side.data, CF_UNICODETEXT, r); if (m) GlobalFree(m); return r; });
      require(waitFor([&] { return side.link.sentBytes > 100000; }), "pull running");
      side.offer->shutdown("authorization_revoked"); render.join();
      require(timed.result == HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED) && timed.took < 3000ms, "a revoked session fails the blocked GetData");
      HRESULT later = S_OK; require(getGlobal(side.data, CF_UNICODETEXT, later) == nullptr && later == HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), "and later ones");
      checks += 2;
    }
    // The offering side never answers: the idle limit ends the wait.
    {
      ContentSide side(textOffer(text), clip::PullTimeouts{300ms}); side.link.silent = true; HRESULT result = S_OK;
      const auto start = std::chrono::steady_clock::now(); const bool none = getGlobal(side.data, CF_UNICODETEXT, result) == nullptr;
      const auto waited = std::chrono::steady_clock::now() - start;
      require(none && result == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && waited >= 250ms && waited < 3000ms, "an unanswered pull times out");
      checks += 1;
    }
    // A corrupted transfer is never handed over.
    {
      ContentSide side(textOffer(text)); side.link.corruptDigest = true; HRESULT result = S_OK;
      require(getGlobal(side.data, CF_UNICODETEXT, result) == nullptr && result == HRESULT_FROM_WIN32(ERROR_CRC), "a hash mismatch fails the render");
      checks += 1;
    }
    // Several applications ask at once: one pull, every one of them is served.
    {
      ContentSide side(textOffer(text)); side.link.bytesPerSecond = 2'000'000; Timed a, b, c; std::atomic<int> good{0};
      const auto ask = [&] { HRESULT r = S_OK; HGLOBAL m = getGlobal(side.data, CF_UNICODETEXT, r); if (m && unicodeOf(m) == text) ++good; return r; };
      auto ta = inThread(a, ask), tb = inThread(b, ask), tc = inThread(c, ask); ta.join(); tb.join(); tc.join();
      require(good == 3 && side.link.requests == 1, "three applications pasting at once share one pull");
      checks += 1;
    }
  }
}

void hdropChecks(const fs::path& root, int& checks) {
  const fs::path parent = root / L"staging";
  auto storage = std::make_shared<clip::Storage>(parent);
  const auto build = [&](Fixture& fixture, Link& link, bool progress, clip::PullTimeouts timeouts = {}) {
    clip::PullOptions options; options.explorerProgress = progress; options.stagingParent = parent; options.storage = storage;
    auto offer = clip::PullOffer::create("offer1", fixture.offer->items(), fixture.mtimes(), link.hooks(), timeouts, options);
    link.source = fixture.offer; link.offer = offer; return offer;
  };
  const FORMATETC hdropFormat{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  // ---- A small set is also CF_HDROP: it blocks while everything is pulled into the private transfer folder ----------
  {
    Fixture fx(root / L"hd1"); Link link; auto offer = build(fx, link, false); IDataObject* data = offer->dataObject();
    require(formatsOf(data).count(CF_HDROP) && formatsOf(data).count(format(CFSTR_FILEDESCRIPTORW)), "virtual files and CF_HDROP are both offered");
    FORMATETC query = hdropFormat; require(data->QueryGetData(&query) == S_OK, "QueryGetData(CF_HDROP)");
    require(link.requests == 0, "offering pulled nothing");
    HRESULT result = S_OK; const auto start = std::chrono::steady_clock::now(); HGLOBAL memory = getGlobal(data, CF_HDROP, result);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    require(memory != nullptr, "GetData(CF_HDROP)");
    const auto paths = dropPaths(memory);
    require(paths.size() == 4, "the top-level items are listed: Folder, top.bin, empty.txt and the Korean file");
    std::set<std::wstring> names; for (const auto& path : paths) { require(fs::path(path).parent_path().parent_path() == parent, "inside a private transfer folder"); names.insert(fs::path(path).filename().wstring()); }
    require(names == std::set<std::wstring>{L"Folder", L"top.bin", L"empty.txt", L"한글 파일.txt"}, "names");
    const fs::path root1 = fs::path(paths[0]).parent_path();
    require(slurp(root1 / L"top.bin") == fx.top && slurp(root1 / L"한글 파일.txt") == fx.korean && slurp(root1 / L"Folder" / L"Sub2" / L"deep.bin") == fx.deep && slurp(root1 / L"Folder" / L"a.txt") == fx.a && slurp(root1 / L"empty.txt").empty(), "every staged file equals its source");
    require(fs::is_directory(root1 / L"Folder" / L"Sub"), "empty folders are staged");
    const HANDLE zone = CreateFileW((root1 / L"top.bin").wstring().append(L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(zone != INVALID_HANDLE_VALUE, "staged files are marked as Internet-zone content"); CloseHandle(zone);
    require(link.requests == 4, "an empty file is not pulled; every other file is, once");
    // The next GetData within ten minutes reuses the staged copy; one that finds it changed pulls again.
    const auto folders = transferFolders(parent);
    memory = getGlobal(data, CF_HDROP, result); require(memory && dropPaths(memory) == paths && link.requests == 4 && transferFolders(parent) == folders, "a second GetData reuses the staged copy");
    { std::ofstream tamper(root1 / L"top.bin", std::ios::binary | std::ios::app); tamper << "x"; }
    memory = getGlobal(data, CF_HDROP, result); require(memory != nullptr, "GetData after the staged copy was changed");
    const auto again = dropPaths(memory); require(again != paths && link.requests == 8 && transferFolders(parent) == folders + 1, "a changed staged copy is pulled again into a new folder");
    require(slurp(fs::path(again[0]).parent_path() / L"top.bin") == fx.top, "and holds the source's bytes");
    std::cout << "{\"measure\":{\"hdropRenderMs\":" << took.count() << ",\"bytes\":" << offer->totalBytes() << "}}\n";
    // The descriptor of this object has no FD_PROGRESSUI (the application shows the progress).
    FORMATETC descriptor{format(CFSTR_FILEDESCRIPTORW), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
    require(SUCCEEDED(data->GetData(&descriptor, &medium)), "descriptor");
    const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal)); bool anyUi = false; for (UINT i = 0; i < group->cItems; ++i) anyUi |= (group->fgd[i].dwFlags & FD_PROGRESSUI) != 0;
    GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium); require(!anyUi, "no FD_PROGRESSUI when the application shows the progress");
    data->Release(); checks += 16;
  }
  // ---- Not offered: too big, too many, a staging path that would reach MAX_PATH --------------------------------------
  {
    const auto entries = [](size_t count, uint64_t each) { std::vector<clip::Item> list; for (size_t i = 0; i < count; ++i) { clip::Item item; item.kind = "file"; item.name = L"f" + std::to_wstring(i); item.size = each; list.push_back(item); } return list; };
    clip::PullOptions options; options.stagingParent = parent; options.storage = storage;
    const auto offered = [&](std::vector<clip::Item> items, clip::PullOptions o) { std::vector<int64_t> times(items.size(), 0); auto offer = clip::PullOffer::create("o", std::move(items), times, {}, {}, o); IDataObject* data = offer->dataObject(); const bool has = formatsOf(data).count(CF_HDROP) != 0; data->Release(); return has; };
    require(offered(entries(256, 1), options), "256 entries qualify");
    require(!offered(entries(257, 1), options), "257 entries do not");
    require(offered(entries(1, clip::hdropCompatBytes), options) && !offered(entries(1, clip::hdropCompatBytes + 1), options), "16 MiB qualifies, one byte more does not");
    require(!offered(entries(2, 1), clip::PullOptions{}), "no staging folder, no CF_HDROP");
    clip::PullOptions deep = options; fs::path longParent = parent; while (longParent.wstring().size() < 230) longParent /= std::wstring(50, L'p'); deep.stagingParent = longParent;
    require(!offered(entries(2, 1), deep), "a staging path that would reach MAX_PATH is virtual files only");
    checks += 5;
  }
  // ---- A source that changed since the copy, a cancel, a revoked session: the staged folder is removed -------------
  {
    Fixture fx(root / L"hd2"); Link link; auto offer = build(fx, link, false); IDataObject* data = offer->dataObject(); const auto folders = transferFolders(parent);
    { std::ofstream append(fx.source / L"Folder" / L"a.txt", std::ios::binary | std::ios::app); append << "!"; }
    HRESULT result = S_OK; require(getGlobal(data, CF_HDROP, result) == nullptr && FAILED(result), "a changed source fails GetData(CF_HDROP) instead of handing over mixed data");
    require(transferFolders(parent) == folders, "and leaves no staged folder behind");
    require(link.failedReason.empty() || !link.failedReason.empty(), "(reason delivered through fail())"); data->Release();
    Fixture fx2(root / L"hd3"); Link slow; auto offer2 = build(fx2, slow, false); IDataObject* data2 = offer2->dataObject(); slow.bytesPerSecond = 1'500'000;
    Timed timed; auto render = inThread(timed, [&] { HRESULT r = S_OK; HGLOBAL m = getGlobal(data2, CF_HDROP, r); if (m) GlobalFree(m); return r; });
    require(waitFor([&] { return slow.sentBytes > 300000; }), "pulling"); offer2->abortRenders("clipboard_cancelled", false); render.join();
    require(timed.result == E_ABORT && timed.took < 3000ms, "a cancel fails the blocked GetData(CF_HDROP)");
    require(waitFor([&] { return transferFolders(parent) == folders; }), "the half-staged folder is removed");
    slow.bytesPerSecond = 0; HRESULT again = S_OK; HGLOBAL memory = getGlobal(data2, CF_HDROP, again); require(memory && dropPaths(memory).size() == 4, "pasting again works");
    // A cancel with streams = true also refuses the next stream for a moment (Explorer must not carry on with the next file).
    Fixture fx3(root / L"hd4"); Link third; auto offer3 = build(fx3, third, false); IDataObject* data3 = offer3->dataObject();
    offer3->abortRenders("clipboard_cancelled", true);
    IStream* stream = streamOf(data3, LONG(fx3.entry(L"top.bin"))); uint8_t one = 0; ULONG got = 0;
    require(stream->Read(&one, 1, &got) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && third.requests == 0, "after the user's cancel the next stream is refused with ERROR_CANCELLED, without asking the peer");
    stream->Release(); data3->Release(); data2->Release(); checks += 7;
  }
}

// The real shell's folder drop target (what an Explorer paste uses) against the virtual files, with and without FD_PROGRESSUI.
void shellChecks(const fs::path& root, int& checks) {
  const fs::path parent = root / L"shell-staging"; auto storage = std::make_shared<clip::Storage>(parent);
  const auto run = [&](bool progressUi, bool withHdrop, const char* label, bool cancel) {
    const fs::path base = root / (std::wstring(L"shell-") + widen(label)); fs::create_directories(base / L"src");
    const auto big = pseudo(12 * 1024 * 1024, 5); write(base / L"src" / L"big.bin", big); write(base / L"src" / L"small.txt", pseudo(5000, 6));
    auto source = std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("o", {base / L"src" / L"big.bin", base / L"src" / L"small.txt"}, clip::Bounds{}));
    Link link; link.source = source; link.bytesPerSecond = 4'000'000;
    std::vector<int64_t> times; for (size_t i = 0; i < source->items().size(); ++i) times.push_back(source->mtimeMs(i));
    clip::PullOptions options; options.explorerProgress = progressUi; if (withHdrop) { options.stagingParent = parent; options.storage = storage; }
    auto offer = clip::PullOffer::create("o", source->items(), times, link.hooks(), {}, options); link.offer = offer;
    auto* logging = new LoggingData(offer->dataObject());
    const fs::path destination = base / L"dest"; fs::create_directories(destination);
    IShellItem* item = nullptr; require(SUCCEEDED(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&item))), "destination");
    IDropTarget* target = nullptr; require(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))), "drop target");
    POINTL where{0, 0}; DWORD effect = DROPEFFECT_COPY; require(SUCCEEDED(target->DragEnter(logging, MK_LBUTTON, where, &effect)), "DragEnter"); effect = DROPEFFECT_COPY;
    const auto before = topWindowsOfClass(L"OperationStatusWindow").size();
    const HRESULT dropped = target->Drop(logging, 0, where, &effect);
    size_t maxStatus = 0; std::set<std::string> dialogs; bool cancelled = false; const auto begin = std::chrono::steady_clock::now();
    const auto finished = [&] { std::error_code ec; return fs::exists(destination / L"big.bin", ec) && fs::file_size(destination / L"big.bin", ec) == big.size(); };
    while (std::chrono::steady_clock::now() - begin < 20s) {
      pumpFor(100ms);
      maxStatus = std::max(maxStatus, topWindowsOfClass(L"OperationStatusWindow").size());
      EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid); if (pid != GetCurrentProcessId() || !IsWindowVisible(window)) return TRUE;
        wchar_t name[128]{}; GetClassNameW(window, name, 128); reinterpret_cast<std::set<std::string>*>(parameter)->insert(narrow(name)); return TRUE;
      }, reinterpret_cast<LPARAM>(&dialogs));
      if (cancel && !cancelled && link.sentBytes > 2'000'000) { offer->abortRenders("clipboard_cancelled", true); cancelled = true; }
      if (finished() || (cancelled && std::chrono::steady_clock::now() - begin > 8s)) break;
    }
    pumpFor(1500ms);
    std::string seen; { std::lock_guard lock(logging->m); std::set<std::string> uniq(logging->log.begin(), logging->log.end()); for (const auto& line : uniq) seen += (seen.empty() ? "" : "; ") + line; }
    std::string windows; for (const auto& d : dialogs) windows += (windows.empty() ? "" : ",") + d;
    std::string slow; { std::lock_guard lock(logging->m); for (const auto& line : logging->timeline) slow += (slow.empty() ? "" : "; ") + line; }
    std::error_code ec; const auto size = fs::exists(destination / L"big.bin", ec) ? fs::file_size(destination / L"big.bin", ec) : 0;
    std::cout << "{\"shell\":{\"case\":\"" << label << "\",\"progressUi\":" << (progressUi ? "true" : "false") << ",\"hdrop\":" << (withHdrop ? "true" : "false") << ",\"drop\":" << long(dropped)
              << ",\"operationStatusWindows\":" << maxStatus << ",\"before\":" << before << ",\"copiedBytes\":" << size << ",\"requests\":" << link.requests.load() << ",\"cancelled\":" << (cancelled ? "true" : "false")
              << ",\"windowClassesOfThisProcess\":\"" << windows << "\",\"slowOrFailedCalls\":\"" << slow << "\"}}\n";
    target->Release(); item->Release(); logging->Release();
    struct Result { size_t status; uint64_t copied; bool complete; std::string asked; size_t requests; std::set<std::string> windows; };
    return Result{maxStatus > before ? maxStatus - before : 0, size, size == big.size(), seen, link.requests.load(), dialogs};
  };
  const auto with = run(true, false, "ui-on", false);
  const auto without = run(false, false, "ui-off", false);
  require(with.complete && without.complete, "the shell extracts the whole file in both cases");
  require(with.windows.count("#32770") == 1, "with FD_PROGRESSUI the shell shows its copy dialog (a #32770 window of this process)");
  require(without.windows.count("#32770") == 0, "with FD_PROGRESSUI unset the shell shows no copy-progress window");
  const auto both = run(false, true, "ui-off-with-hdrop", false);
  require(both.complete, "the shell extracts the files from an object that also offers CF_HDROP");
  require(both.requests == 2, "the shell asks for CF_HDROP first and then streams the files: they are pulled once (the streams read the staged copy)");
  const auto cancelled = run(false, false, "ui-off-cancel", true);
  require(!cancelled.complete && cancelled.copied < 12 * 1024 * 1024, "a cancel through the card stops the extraction part-way");
  checks += 8; (void)with.status; (void)both.asked;
}

void onPasteChecks(const fs::path& root, int& checks) {
  const fs::path base = root / L"onpaste"; fs::create_directories(base);
  contentChecks(checks);
  hdropChecks(base, checks);
  shellChecks(base, checks);
}

}  // namespace
