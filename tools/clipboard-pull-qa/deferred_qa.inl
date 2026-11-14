// Deferred manifest (manifest-on-paste-v1, pull.hpp PullOffer::createDeferred and OfferRegistry deferred entries): a copy that is not
// small is published without its list; the first GetData of the descriptor asks for it and waits. Included by clipboard_pull_qa.cpp.

namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
// Reads the descriptor of a data object (blocking GetData). Returns the HRESULT and the entry count.
HRESULT descriptorOf(IDataObject* data, UINT& count) {
  FORMATETC fe{format(CFSTR_FILEDESCRIPTORW), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
  const HRESULT result = data->GetData(&fe, &medium); count = 0;
  if (SUCCEEDED(result)) { if (auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal))) { count = group->cItems; GlobalUnlock(medium.hGlobal); } ReleaseStgMedium(&medium); }
  return result;
}
}

void deferredChecks(const fs::path& root, int& checks, bool useClipboard) {
  Fixture f(root / L"deferred");
  const auto ordinalOfTop = [&] { size_t n = 0; for (const auto& item : f.offer->items()) { if (item.kind == "file") { if (item.name == L"top.bin") return n; ++n; } } throw std::runtime_error("top"); }();

  // ---- The data object: one request, the descriptor once the list is there, then the usual pulls ----------------------------
  {
    Link link; link.source = f.offer; std::atomic<int> asked{0}, cancelled{0};
    auto hooks = link.hooks(); hooks.manifest = [&](const std::string& id) { require(id == "deferred1", "the request names the offer"); ++asked; };
    hooks.manifestCancel = [&](const std::string&, const std::string&) { ++cancelled; };
    auto offer = clip::PullOffer::createDeferred("deferred1", hooks); link.offer = offer;
    require(offer->deferredPending() && offer->fileCount() == 0, "deferred: no list when published");
    IDataObject* data = offer->dataObject();
    std::thread answer([&] { waitFor([&] { return asked.load() == 1; }); std::this_thread::sleep_for(300ms); offer->manifestProgress(); offer->deliverManifest(f.offer->items(), f.mtimes()); });
    UINT count = 0; const auto started = std::chrono::steady_clock::now();
    const HRESULT first = descriptorOf(data, count); answer.join();
    require(SUCCEEDED(first) && count == f.offer->items().size() && asked == 1, "the descriptor comes once the list arrived");
    require(std::chrono::steady_clock::now() - started >= 250ms, "GetData waited for the list");
    require(SUCCEEDED(descriptorOf(data, count)) && asked == 1 && !offer->deferredPending(), "the list is asked for once");
    HRESULT read = S_OK; IStream* stream = nullptr;
    { size_t entry = 0; for (; entry < f.offer->items().size(); ++entry) if (f.offer->items()[entry].name == L"top.bin") break;
      stream = streamOf(data, LONG(entry)); }
    const auto bytes = drain(stream, read); stream->Release();
    require(SUCCEEDED(read) && bytes == f.top, "a file of the deferred offer streams as usual");
    require(link.requests >= 1 && cancelled == 0, "pulled after the list");
    (void)ordinalOfTop;
    data->Release(); checks += 5;
  }

  // ---- A refused request fails that paste with its reason; the next paste asks again ---------------------------------------
  {
    Link link; link.source = f.offer; std::atomic<int> asked{0};
    auto hooks = link.hooks(); hooks.manifest = [&](const std::string&) { ++asked; };
    auto offer = clip::PullOffer::createDeferred("deferred2", hooks); link.offer = offer;
    IDataObject* data = offer->dataObject();
    std::thread refuse([&] { waitFor([&] { return asked.load() == 1; }); offer->failManifest("clipboard_entry_count_limit"); });
    UINT count = 0; const HRESULT failed = descriptorOf(data, count); refuse.join();
    require(FAILED(failed) && failed != E_ABORT && asked == 1 && offer->deferredPending(), "a refused list fails the paste with its own error");
    std::thread deliver([&] { waitFor([&] { return asked.load() == 2; }); offer->deliverManifest(f.offer->items(), f.mtimes()); });
    const HRESULT again = descriptorOf(data, count); deliver.join();
    require(SUCCEEDED(again) && asked == 2 && count == f.offer->items().size(), "the next paste asks again and gets the list");
    // An invalid list (a child before its folder) fails the request instead of being published.
    auto broken = clip::PullOffer::createDeferred("deferred3", hooks); IDataObject* other = broken->dataObject();
    const int before = asked.load();
    std::thread bad([&] { waitFor([&] { return asked.load() == before + 1; }); auto items = f.offer->items(); std::swap(items[0], items[1]); broken->deliverManifest(items, f.mtimes()); });
    require(FAILED(descriptorOf(other, count)) && broken->deferredPending(), "an invalid list is refused"); bad.join();
    other->Release(); data->Release(); checks += 3;
  }

  // ---- A paste given up while it waits (a newer copy, the user's cancel) ends the wait and withdraws the request ---------------
  {
    Link link; link.source = f.offer; std::atomic<int> asked{0}, cancelled{0};
    auto hooks = link.hooks(); hooks.manifest = [&](const std::string&) { ++asked; }; hooks.manifestCancel = [&](const std::string&, const std::string&) { ++cancelled; };
    auto offer = clip::PullOffer::createDeferred("deferred4", hooks); link.offer = offer;
    IDataObject* data = offer->dataObject();
    std::thread abort([&] { waitFor([&] { return asked.load() == 1; }); std::this_thread::sleep_for(100ms); offer->abortRenders("clipboard_offer_expired", true); });
    UINT count = 0; const HRESULT aborted = descriptorOf(data, count); abort.join();
    require(aborted == E_ABORT, "a superseded wait ends with E_ABORT");
    require(waitFor([&] { return cancelled.load() == 1; }), "the abandoned request is withdrawn (clipboard.cancel)");
    offer->shutdown("clipboard_transport_closed");
    require(FAILED(descriptorOf(data, count)) && asked == 1, "a closed channel refuses at once, without asking");
    data->Release(); checks += 3;
  }

  // ---- A newer copy while a paste waits for the list: the paste has begun and keeps waiting; the user's cancel still ends it ----------
  // (Until 1.1.6 a newer copy on the offering side ended the wait: the paste of a large folder failed with no descriptor.)
  {
    Link link; link.source = f.offer; std::atomic<int> asked{0}, cancelled{0};
    auto hooks = link.hooks(); hooks.manifest = [&](const std::string&) { ++asked; }; hooks.manifestCancel = [&](const std::string&, const std::string&) { ++cancelled; };
    auto offer = clip::PullOffer::createDeferred("deferred7", hooks); link.offer = offer;
    std::atomic<bool> current{true}; offer->setCurrentCheck([&] { return current.load(); });
    IDataObject* data = offer->dataObject();
    std::thread newer([&] {
      waitFor([&] { return asked.load() == 1; });
      offer->supersedeRenders("clipboard_offer_expired"); current = false;  // the newer copy replaces this offer on the clipboard
      std::this_thread::sleep_for(600ms); offer->deliverManifest(f.offer->items(), f.mtimes());
    });
    UINT count = 0; const HRESULT kept = descriptorOf(data, count); newer.join();
    require(SUCCEEDED(kept) && count == f.offer->items().size() && asked == 1 && cancelled == 0, "a paste waiting for its list outlives a newer copy");
    auto other = clip::PullOffer::createDeferred("deferred8", hooks); IDataObject* second = other->dataObject();
    const int before = asked.load();
    std::thread cancel([&] { waitFor([&] { return asked.load() == before + 1; }); other->supersedeRenders("clipboard_offer_expired"); std::this_thread::sleep_for(300ms); other->abortRenders("clipboard_cancelled", true); });
    require(descriptorOf(second, count) == E_ABORT, "the user's cancel still ends a list wait"); cancel.join();
    require(waitFor([&] { return cancelled.load() == 1; }), "and withdraws its request");
    second->Release(); data->Release(); checks += 3;
  }

  // ---- A paste given up, and the next paste at once: the older attempt's cancel never lands on the newer request ------------------
  // (Until 1.1.6 the older render thread, winding down for up to 250 ms, cancelled by offer id whatever request was out then: the
  // newer paste waited 30 s for an answer nobody sent.) One request at a time: the newer attempt takes the older request over, or asks
  // anew only after the older cancel went out.
  for (int round = 0; round < 6; ++round) {
    Link link; link.source = f.offer; std::mutex m; std::vector<std::string> events; std::atomic<int> asked{0};
    auto hooks = link.hooks();
    hooks.manifest = [&](const std::string&) { { std::lock_guard lock(m); events.push_back("ask"); } ++asked; };
    hooks.manifestCancel = [&](const std::string&, const std::string& why) { std::lock_guard lock(m); events.push_back("cancel:" + why); };
    auto offer = clip::PullOffer::createDeferred("deferred6", hooks); link.offer = offer;
    IDataObject* data = offer->dataObject();
    std::thread abort([&] { waitFor([&] { return asked.load() == 1; }); offer->abortRenders("clipboard_cancelled", true); });
    UINT count = 0; require(descriptorOf(data, count) == E_ABORT, "the first paste was given up"); abort.join();
    // The second paste right away (round 0..2), or once the older thread had time to wind down (3..5).
    if (round >= 3) std::this_thread::sleep_for(400ms);
    std::atomic<bool> second{false};
    std::thread deliver([&] { waitFor([&] { return second.load(); }); std::this_thread::sleep_for(300ms); offer->deliverManifest(f.offer->items(), f.mtimes()); });
    second = true; const HRESULT again = descriptorOf(data, count); deliver.join();
    std::vector<std::string> seen; { std::lock_guard lock(m); seen = events; }
    std::string trace; for (const auto& e : seen) trace += e + ' ';
    require(SUCCEEDED(again) && count == f.offer->items().size(), "the next paste gets the list (" + trace + ")");
    require(!seen.empty() && seen.back() == "ask", "no cancel after the request the paste waits for (" + trace + ")");
    for (const auto& e : seen) require(e == "ask" || e == "cancel:clipboard_cancelled", "a given-up paste cancels as cancelled (" + trace + ")");
    data->Release(); checks += 3;
  }

  // ---- The source side: a deferred copy is a registry entry until the paste's walk replaces it -----------------------------------
  {
    clip::OfferRegistry registry;
    registry.addDeferred("d1", f.roots, 41);
    require(!registry.find("d1") && registry.deferred("d1") && registry.deferred("d1")->sequence == 41, "a deferred copy has no list");
    require(registry.ids().size() == 1 && registry.ids()[0] == "d1", "a deferred copy counts among the kept offers");
    std::shared_ptr<clip::SourceOffer> walked(clip::SourceOffer::capture("d1", f.roots, clip::Bounds{}));
    // The paste's request marks the copy pasting before its walk ends: a newer copy meanwhile does not drop it.
    require(!registry.pasting("d1") && registry.listing("d1") && registry.pasting("d1"), "a copy being walked for a paste is pasting");
    require(registry.dropIdle().empty() && registry.deferred("d1"), "a copy being walked stays after the clipboard changed");
    require(registry.materialize("d1", walked) && registry.find("d1") == walked && registry.deferred("d1") && registry.deferred("d1")->listed == walked &&
            registry.pasting("d1"), "materialized and pasting, its list kept for a request that comes again");
    std::shared_ptr<clip::SourceOffer> again(clip::SourceOffer::capture("d1", f.roots, clip::Bounds{}));
    require(registry.materialize("d1", again) && registry.find("d1") == walked, "a second walk of one copy keeps the first list");
    require(!registry.listing("nope") && !registry.materialize("nope", std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("nope", f.roots, clip::Bounds{}))),
            "only a deferred copy is listed or materialized");
    registry.add(std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("e1", f.roots, clip::Bounds{})));
    require(!registry.deferred("e1") && !registry.listing("e1"), "a copy offered with its list is not deferred");
    registry.remove("e1");
    // A pasting offer is admitted whatever the clipboard says now.
    require(registry.openCurrent("d1", ordinalOfTop, 0, [] { return DWORD(99); }) != nullptr, "a paste under way goes on after the number moved");
    // Two newer copies retire a deferred one that nothing pastes.
    registry.addDeferred("d2", f.roots, 42); registry.addDeferred("d3", f.roots, 43);
    require(!registry.deferred("d2") && registry.deferred("d3") && registry.find("d1"), "the latest two stay, the pasting one first");
    checks += 11;
  }

  // ---- "Still the same copy": the number moved, the same paths are on the clipboard -------------------------------------------------
  {
    clip::OfferRegistry registry;
    registry.add(std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("s1", f.roots, clip::Bounds{})), DWORD(10));
    expectReason(reason([&] { registry.openCurrent("s1", ordinalOfTop, 0, [] { return DWORD(11); }); }), "clipboard_offer_expired", "moved number, no selection check");
    auto reordered = f.roots; std::reverse(reordered.begin(), reordered.end());
    for (auto& path : reordered) path = fs::path(path.wstring() + L"\\");  // a trailing separator does not matter
    require(registry.openCurrent("s1", ordinalOfTop, 0, [] { return DWORD(12); }, [&] { return reordered; }) != nullptr, "the same paths: still the copy");
    registry.add(std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("s2", {f.source / L"top.bin"}, clip::Bounds{})), DWORD(20));
    expectReason(reason([&] { registry.openCurrent("s2", 0, 0, [] { return DWORD(21); }, [&] { return f.roots; }); }), "clipboard_offer_expired", "other paths: a new copy");
    require(clip::sameSelection({L"C:\\A\\b.txt", L"C:\\A\\c"}, {L"c:\\a\\C\\", L"C:\\a\\B.TXT"}) && !clip::sameSelection({L"C:\\A"}, {L"C:\\A", L"C:\\B"}) && !clip::sameSelection({}, {}), "sameSelection");
    checks += 4;
  }

  // ---- The walk: a copy is probed without walking it; progress and cancel ------------------------------------------------------------
  {
    const fs::path many = root / L"deferred-many";
    for (int i = 0; i < 300; ++i) write(many / L"Many" / (L"f" + std::to_wstring(i)), {uint8_t(i)});
    require(!clip::treeFitsWithin({many / L"Many"}, clip::Bounds::scale(), clip::eagerManifestEntries, clip::eagerManifestBytes), "300 entries is not small");
    require(clip::treeFitsWithin({f.source / L"Folder"}, clip::Bounds::scale(), clip::eagerManifestEntries, clip::eagerManifestBytes), "a small folder is small");
    write(many / L"big.bin", pseudo(17 * 1024 * 1024, 5));
    require(!clip::treeFitsWithin({many / L"big.bin"}, clip::Bounds::scale(), clip::eagerManifestEntries, clip::eagerManifestBytes), "17 MiB is not small");
    size_t reported = 0; clip::TreeScanControl control; control.progress = [&](size_t entries) { reported = std::max(reported, entries); };
    const auto scan = clip::scanTree({many / L"Many"}, clip::Bounds::scale(), control);
    require(scan.entries.size() == 301 && reported >= 256, "progress while walking");
    control.progress = [](size_t entries) { if (entries >= 256) throw std::runtime_error("clipboard_cancelled"); };
    expectReason(reason([&] { clip::scanTree({many / L"Many"}, clip::Bounds::scale(), control); }), "clipboard_cancelled", "a walk stops when its progress callback throws");
    checks += 5;
  }

  // ---- The real clipboard and the shell's folder drop target (an Explorer paste) -------------------------------------------------------
  if (useClipboard) {
    const HWND owner = CreateWindowExW(0, L"STATIC", L"QA", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::optional<std::wstring> savedText; std::vector<fs::path> savedFiles;
    try { auto saved = clip::read(owner, true, true); savedText = saved.text; savedFiles = saved.files; } catch (...) {}
    Link link; link.source = f.offer; std::atomic<int> asked{0};
    auto hooks = link.hooks(); hooks.manifest = [&](const std::string&) { ++asked; };
    auto offer = clip::PullOffer::createDeferred("deferred-real", hooks); link.offer = offer;
    clip::publishPullOffer(offer, std::nullopt);
    require(IsClipboardFormatAvailable(format(CFSTR_FILEDESCRIPTORW)) && !IsClipboardFormatAvailable(CF_HDROP), "published as virtual files without a list");
    const auto pump = [](std::chrono::milliseconds span) { const auto end = std::chrono::steady_clock::now() + span; MSG message; while (std::chrono::steady_clock::now() < end) { while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0); } };
    pump(400ms); require(asked == 0, "nothing asks for the list before a paste");
    // The list arrives two seconds after the paste asked for it (a walk that takes a while).
    std::thread answer([&] { waitFor([&] { return asked.load() >= 1; }, 30s); std::this_thread::sleep_for(2s); offer->manifestProgress(); offer->deliverManifest(f.offer->items(), f.mtimes()); });
    const fs::path destination = root / L"deferred-pasted"; fs::create_directories(destination);
    std::atomic<bool> finished{false}; HRESULT dropResult = E_FAIL; std::string workerError;
    std::thread paste([&] {
      const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      try {
        IDataObject* data = nullptr; require(SUCCEEDED(OleGetClipboard(&data)) && data, "OleGetClipboard");
        IShellItem* item = nullptr; require(SUCCEEDED(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&item))), "destination item");
        IDropTarget* target = nullptr; require(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))), "folder drop target");
        POINTL where{0, 0}; DWORD effect = DROPEFFECT_COPY;
        require(SUCCEEDED(target->DragEnter(data, MK_LBUTTON, where, &effect)), "DragEnter");
        effect = DROPEFFECT_COPY; dropResult = target->Drop(data, 0, where, &effect);
        const auto end = std::chrono::steady_clock::now() + 60s; const auto expected = f.offer->totalBytes();
        const auto sizeOf = [&] { uint64_t sum = 0; std::error_code ec; for (const auto& e : fs::recursive_directory_iterator(destination, ec)) if (e.is_regular_file(ec)) sum += e.file_size(ec); return sum; };
        while (std::chrono::steady_clock::now() < end && sizeOf() < expected) std::this_thread::sleep_for(50ms);
        std::this_thread::sleep_for(500ms);
        target->Release(); item->Release(); data->Release();
      } catch (const std::exception& error) { workerError = error.what(); }
      if (SUCCEEDED(com)) CoUninitialize();
      finished = true;
    });
    while (!finished) pump(50ms);
    paste.join(); answer.join();
    require(workerError.empty(), workerError.c_str());
    require(SUCCEEDED(dropResult) && asked == 1, "the shell's paste asked for the list once and succeeded");
    size_t compared = 0;
    for (const auto& item : f.offer->items()) {
      const fs::path target = destination / item.name;
      if (item.kind == "dir") { require(fs::is_directory(target), "deferred paste: folder"); continue; }
      require(fs::is_regular_file(target) && sha256(slurp(target)) == sha256(slurp(f.source / item.name)), "deferred paste: a file differs from its source");
      ++compared;
    }
    require(compared == 5, "deferred paste: every file");
    std::cout << "{\"deferredRealClipboard\":{\"asked\":" << asked.load() << ",\"filesExtracted\":" << compared << ",\"pulled\":" << link.requests.load() << "}}\n";
    clip::withdrawPullOffer(offer);
    // The folder's own "paste" verb (what Explorer's Paste command invokes) on a new deferred copy whose list comes 2 s late.
    {
      Link verbLink; verbLink.source = f.offer; std::atomic<int> verbAsked{0};
      auto verbHooks = verbLink.hooks(); verbHooks.manifest = [&](const std::string&) { ++verbAsked; };
      auto verbOffer = clip::PullOffer::createDeferred("deferred-verb", verbHooks); verbLink.offer = verbOffer;
      clip::publishPullOffer(verbOffer, std::nullopt);
      std::thread verbAnswer([&] { waitFor([&] { return verbAsked.load() >= 1; }, 30s); std::this_thread::sleep_for(2s); verbOffer->deliverManifest(f.offer->items(), f.mtimes()); });
      const fs::path second = root / L"deferred-verb"; fs::create_directories(second); std::atomic<bool> verbDone{false}; HRESULT verbResult = E_FAIL; std::string verbError;
      std::thread verb([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        try {
          IShellItem* item = nullptr; require(SUCCEEDED(SHCreateItemFromParsingName(second.c_str(), nullptr, IID_PPV_ARGS(&item))), "verb destination");
          IContextMenu* menu = nullptr; require(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu))), "folder context menu");
          HMENU hmenu = CreatePopupMenu(); require(SUCCEEDED(menu->QueryContextMenu(hmenu, 0, 1, 0x7fff, CMF_NORMAL)), "QueryContextMenu");
          CMINVOKECOMMANDINFO invoke{}; invoke.cbSize = sizeof(invoke); invoke.lpVerb = "paste"; invoke.nShow = SW_HIDE; invoke.fMask = CMIC_MASK_FLAG_NO_UI | CMIC_MASK_NOASYNC;
          verbResult = menu->InvokeCommand(&invoke);
          const auto end = std::chrono::steady_clock::now() + 60s;
          const auto size = [&] { uint64_t sum = 0; std::error_code ec; for (const auto& e : fs::recursive_directory_iterator(second, ec)) if (e.is_regular_file(ec)) sum += e.file_size(ec); return sum; };
          while (std::chrono::steady_clock::now() < end && size() < f.offer->totalBytes()) std::this_thread::sleep_for(50ms);
          std::this_thread::sleep_for(500ms); DestroyMenu(hmenu); menu->Release(); item->Release();
        } catch (const std::exception& error) { verbError = error.what(); }
        if (SUCCEEDED(com)) CoUninitialize();
        verbDone = true;
      });
      while (!verbDone) pump(50ms);
      verb.join(); verbAnswer.join(); require(verbError.empty(), verbError.c_str());
      size_t verbCompared = 0;
      for (const auto& item : f.offer->items()) {
        if (item.kind == "dir") { require(fs::is_directory(second / item.name), "deferred paste verb: folder missing"); continue; }
        require(fs::is_regular_file(second / item.name) && sha256(slurp(second / item.name)) == sha256(slurp(f.source / item.name)), "deferred paste verb: a file differs"); ++verbCompared;
      }
      std::cout << "{\"deferredPasteVerb\":{\"invokeResult\":" << long(verbResult) << ",\"asked\":" << verbAsked.load() << ",\"filesExtracted\":" << verbCompared << "}}\n";
      require(verbCompared == 5 && SUCCEEDED(verbResult) && verbAsked == 1, "the paste verb waited for the list and extracted every file");
      clip::withdrawPullOffer(verbOffer); ++checks;
    }
    if (!savedFiles.empty()) { try { clip::putPaths(owner, savedFiles, std::nullopt); } catch (...) {} }
    else if (savedText && !savedText->empty()) { try { clip::putText(owner, *savedText); } catch (...) {} }
    DestroyWindow(owner); checks += 3;
  }
}
