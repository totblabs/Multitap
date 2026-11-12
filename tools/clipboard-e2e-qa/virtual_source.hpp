#pragma once
// A synthetic OLE data object of virtual files (FileGroupDescriptorW + FileContents), the shape a .zip folder, a mail attachment
// or a phone namespace publishes. Used by the clipboard end-to-end harness. Thread-safe enough
// for the library's worker thread to call it while the test thread waits.
#include <windows.h>
#include <objidl.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace vsrc {
struct Item {
  std::wstring name;              // relative path as the descriptor carries it ('\' or '/'); a folder may end in a separator
  bool directory = false;
  std::vector<uint8_t> data;
  enum Mode { Stream, Global, NoContents } mode = Stream;
  bool declareSize = true;        // FD_FILESIZE in the descriptor
  bool declareAttributes = true;  // FD_ATTRIBUTES
  DWORD attributes = FILE_ATTRIBUTE_ARCHIVE;
  bool writeTime = true;          // FD_WRITESTIME (a fixed 2024-05-06 time)
  int slowMs = 0;                 // every IStream::Read sleeps this long
  int failAfterBytes = -1;        // Read fails with E_FAIL once this many bytes were handed out
  int delayRenderMs = 0;          // GetData(FileContents) sleeps first (a source rendering on demand)
  bool oversizeGlobal = false;    // the HGLOBAL is rounded up (allocation granularity); the declared size is the truth
};

class Stream : public IStream {
 public:
  Stream(std::vector<uint8_t> data, int slowMs, int failAfter) : data_(std::move(data)), slowMs_(slowMs), failAfter_(failAfter) {}
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (id == IID_IUnknown || id == IID_IStream || id == IID_ISequentialStream) { *out = static_cast<IStream*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
  STDMETHODIMP Read(void* buffer, ULONG count, ULONG* got) override {
    if (slowMs_) std::this_thread::sleep_for(std::chrono::milliseconds(slowMs_));
    if (failAfter_ >= 0 && at_ >= size_t(failAfter_)) { if (got) *got = 0; return E_FAIL; }
    size_t take = std::min<size_t>(count, data_.size() - at_);
    if (failAfter_ >= 0) take = std::min<size_t>(take, size_t(failAfter_) - at_);
    if (take) std::memcpy(buffer, data_.data() + at_, take);
    at_ += take; if (got) *got = ULONG(take);
    return take == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
  STDMETHODIMP Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) override { return E_NOTIMPL; }
  STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
  STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
  STDMETHODIMP Commit(DWORD) override { return S_OK; }
  STDMETHODIMP Revert() override { return E_NOTIMPL; }
  STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP Stat(STATSTG*, DWORD) override { return E_NOTIMPL; }  // a streaming source often cannot tell its size
  STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }
 private:
  std::atomic<ULONG> refs_{1}; std::vector<uint8_t> data_; size_t at_ = 0; int slowMs_, failAfter_;
};

class Source : public IDataObject {
 public:
  explicit Source(std::vector<Item> items) : items_(std::move(items)) {
    descriptorFormat_ = CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    contentsFormat_ = CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
  }
  std::atomic<int> contentCalls{0};
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (id == IID_IUnknown || id == IID_IDataObject) { *out = static_cast<IDataObject*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
  STDMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (!format || !medium) return E_INVALIDARG;
    *medium = STGMEDIUM{};
    if (format->cfFormat == descriptorFormat_) {
      if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
      const size_t bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + items_.size() * sizeof(FILEDESCRIPTORW);
      HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes); if (!global) return E_OUTOFMEMORY;
      auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(global));
      group->cItems = UINT(items_.size());
      for (size_t i = 0; i < items_.size(); ++i) {
        const Item& item = items_[i]; FILEDESCRIPTORW& d = group->fgd[i];
        wcsncpy_s(d.cFileName, item.name.c_str(), _TRUNCATE);
        if (item.name.size() >= MAX_PATH) std::wmemcpy(d.cFileName, item.name.c_str(), MAX_PATH);  // no terminator: a hostile descriptor
        if (item.declareAttributes) { d.dwFlags |= FD_ATTRIBUTES; d.dwFileAttributes = item.directory ? FILE_ATTRIBUTE_DIRECTORY : item.attributes; }
        if (item.declareSize && !item.directory) { d.dwFlags |= FD_FILESIZE; d.nFileSizeLow = DWORD(item.data.size()); d.nFileSizeHigh = DWORD(uint64_t(item.data.size()) >> 32); }
        if (item.writeTime) { d.dwFlags |= FD_WRITESTIME; SYSTEMTIME t{2024, 5, 0, 6, 12, 30, 15, 0}; SystemTimeToFileTime(&t, &d.ftLastWriteTime); }
      }
      GlobalUnlock(global);
      medium->tymed = TYMED_HGLOBAL; medium->hGlobal = global; return S_OK;
    }
    if (format->cfFormat == contentsFormat_) {
      ++contentCalls;
      const LONG index = format->lindex;
      if (index < 0 || size_t(index) >= items_.size()) return DV_E_LINDEX;
      const Item& item = items_[size_t(index)];
      if (item.mode == Item::NoContents || item.directory) return DV_E_FORMATETC;
      if (item.delayRenderMs) std::this_thread::sleep_for(std::chrono::milliseconds(item.delayRenderMs));
      if (item.mode == Item::Stream) {
        if (!(format->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
        medium->tymed = TYMED_ISTREAM; medium->pstm = new Stream(item.data, item.slowMs, item.failAfterBytes); return S_OK;
      }
      if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
      HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, item.data.size() + (item.oversizeGlobal ? 4093 : 0));
      if (!global) return E_OUTOFMEMORY;
      if (!item.data.empty()) { std::memcpy(GlobalLock(global), item.data.data(), item.data.size()); GlobalUnlock(global); }
      medium->tymed = TYMED_HGLOBAL; medium->hGlobal = global; return S_OK;
    }
    return DV_E_FORMATETC;
  }
  STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  STDMETHODIMP QueryGetData(FORMATETC* format) override {
    if (format && (format->cfFormat == descriptorFormat_ || format->cfFormat == contentsFormat_)) return S_OK;
    return DV_E_FORMATETC;
  }
  STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override { out->ptd = nullptr; return DATA_S_SAMEFORMATETC; }
  STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC** out) override {
    // Both formats, so OleSetClipboard publishes them as clipboard formats.
    FORMATETC formats[2] = {{descriptorFormat_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}, {contentsFormat_, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM | TYMED_HGLOBAL}};
    return SHCreateStdEnumFmtEtc(2, formats, out);
  }
  STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
  STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
 private:
  std::atomic<ULONG> refs_{1}; std::vector<Item> items_; CLIPFORMAT descriptorFormat_, contentsFormat_;
};
}  // namespace vsrc
