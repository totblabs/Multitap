#include "client_print.h"
#include "client_print_fit.h"
#include "client_print_ticket.h"

#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <documenttarget.h>
#include <dxgi.h>
#include <ocidl.h>
#include <shcore.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.data.pdf.interop.h>
#include <winspool.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "ui_language.h"
#include "zone_identifier.h"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace multitap {
using V = flutter::EncodableValue;
using M = flutter::EncodableMap;
using Microsoft::WRL::ComPtr;
namespace {
constexpr int kMaximumCopies = 20;
constexpr uint32_t kMaximumPages = 10000;

std::wstring Wide(const std::string& text) {
  if (text.empty() || text.size() > 65536) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) return {};
  std::wstring out(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), count) != count) return {};
  return out;
}
std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) return {};
  std::string out(static_cast<size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
  return out;
}
const std::string* StringArgument(const M& map, const char* key) {
  const auto found = map.find(V(key));
  return found == map.end() ? nullptr : std::get_if<std::string>(&found->second);
}
int IntArgument(const M& map, const char* key, int fallback) {
  const auto found = map.find(V(key));
  if (found == map.end()) return fallback;
  if (const auto* value = std::get_if<int32_t>(&found->second)) return *value;
  if (const auto* value = std::get_if<int64_t>(&found->second)) return *value > INT32_MAX || *value < INT32_MIN ? fallback : static_cast<int>(*value);
  return fallback;
}
// A plain local file (never a link) that starts as a PDF does.
bool PlainPdf(const std::wstring& path) {
  if (path.empty() || path.size() > 32000) return false;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  char header[5]{}; DWORD read = 0;
  const bool ok = GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
                  ReadFile(file, header, sizeof(header), &read, nullptr) && read == sizeof(header) && std::equal(header, header + 5, "%PDF-");
  CloseHandle(file);
  return ok;
}
// PdfCreateRenderer, loaded when printing: a Windows edition without the PDF
// component still starts the client (printing then reports print_unavailable).
HRESULT CreatePdfRenderer(IDXGIDevice* device, IPdfRendererNative** renderer) {
  static const auto create = [] {
    const HMODULE module = LoadLibraryExW(L"Windows.Data.Pdf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<PFN_PDF_CREATE_RENDERER>(GetProcAddress(module, "PdfCreateRenderer")) : nullptr;
  }();
  return create ? create(device, renderer) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

// The print job's own completion (IPrintDocumentPackageStatusEvent): the
// pages are with the spooler (or, printing to a stream, written there).
class JobCompletion final : public IPrintDocumentPackageStatusEvent {
 public:
  JobCompletion() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~JobCompletion() { if (done_) CloseHandle(done_); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDispatch) || riid == __uuidof(IPrintDocumentPackageStatusEvent)) {
      *out = static_cast<IPrintDocumentPackageStatusEvent*>(this); AddRef(); return S_OK;
    }
    *out = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { const ULONG left = --references_; if (!left) delete this; return left; }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override { if (count) *count = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PackageStatusUpdated(PrintDocumentPackageStatus* status) override {
    if (status && status->Completion != PrintDocumentPackageCompletion_InProgress) {
      completion_ = status->Completion;
      SetEvent(done_);
    }
    return S_OK;
  }
  // The completion, or InProgress when it did not end within [milliseconds].
  PrintDocumentPackageCompletion Wait(DWORD milliseconds) const {
    return done_ && WaitForSingleObject(done_, milliseconds) == WAIT_OBJECT_0 ? completion_.load() : PrintDocumentPackageCompletion_InProgress;
  }
 private:
  std::atomic<ULONG> references_{1};
  std::atomic<PrintDocumentPackageCompletion> completion_{PrintDocumentPackageCompletion_InProgress};
  HANDLE done_;
};

// The printer's default paper and unprintable borders, from its device
// context (device units -> DIPs). Empty (zero) when the driver gives none.
PaperArea PrinterPaper(const std::wstring& printer) {
  PaperArea paper;
  const HDC dc = CreateDCW(L"WINSPOOL", printer.c_str(), nullptr, nullptr);
  if (!dc) return paper;
  const int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);
  const int width = GetDeviceCaps(dc, PHYSICALWIDTH), height = GetDeviceCaps(dc, PHYSICALHEIGHT);
  const int offsetX = GetDeviceCaps(dc, PHYSICALOFFSETX), offsetY = GetDeviceCaps(dc, PHYSICALOFFSETY);
  const int printableX = GetDeviceCaps(dc, HORZRES), printableY = GetDeviceCaps(dc, VERTRES);
  DeleteDC(dc);
  if (dpiX <= 0 || dpiY <= 0 || width <= 0 || height <= 0 || printableX <= 0 || printableY <= 0) return paper;
  const float w = width * 96.0f / dpiX, h = height * 96.0f / dpiY;
  const float left = offsetX * 96.0f / dpiX, top = offsetY * 96.0f / dpiY;
  const float right = w - left - printableX * 96.0f / dpiX, bottom = h - top - printableY * 96.0f / dpiY;
  paper.shortSide = std::min(w, h);
  paper.longSide = std::max(w, h);
  paper.margin = std::max({0.0f, left, top, right, bottom});
  return paper;
}

winrt::Windows::Data::Pdf::PdfDocument OpenPdf(const std::wstring& path) {
  using winrt::Windows::Storage::Streams::IRandomAccessStream;
  if (!PlainPdf(path)) return nullptr;
  IRandomAccessStream stream{nullptr};
  if (FAILED(CreateRandomAccessStreamOnFile(path.c_str(), static_cast<DWORD>(winrt::Windows::Storage::FileAccessMode::Read),
                                            winrt::guid_of<IRandomAccessStream>(), winrt::put_abi(stream))) || !stream) return nullptr;
  try {
    auto document = winrt::Windows::Data::Pdf::PdfDocument::LoadFromStreamAsync(stream).get();
    if (!document || document.PageCount() < 1 || document.PageCount() > kMaximumPages) return nullptr;
    return document;
  } catch (...) {
    return nullptr;
  }
}
}  // namespace

std::vector<LocalPrinterInfo> LocalPrinters() {
  std::vector<LocalPrinterInfo> out;
  std::wstring fallback(512, L'\0'); DWORD length = static_cast<DWORD>(fallback.size());
  const std::wstring defaultName = GetDefaultPrinterW(fallback.data(), &length) ? std::wstring(fallback.c_str()) : std::wstring();
  DWORD needed = 0, count = 0;
  EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &count);
  if (!needed || needed > 8 * 1024 * 1024) return out;
  std::vector<BYTE> buffer(needed);
  if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buffer.data(), needed, &needed, &count)) return out;
  const auto* printers = reinterpret_cast<const PRINTER_INFO_4W*>(buffer.data());
  for (DWORD i = 0; i < count && out.size() < 256; ++i) {
    if (!printers[i].pPrinterName || !*printers[i].pPrinterName) continue;
    const std::wstring name = printers[i].pPrinterName;
    out.push_back({name, CompareStringOrdinal(name.c_str(), -1, defaultName.c_str(), -1, TRUE) == CSTR_EQUAL});
  }
  return out;
}

const char* PrinterStatusCode(DWORD status, DWORD attributes) {
  if ((status & PRINTER_STATUS_OFFLINE) || (attributes & PRINTER_ATTRIBUTE_WORK_OFFLINE)) return "print_printer_offline";
  if (status & PRINTER_STATUS_PAUSED) return "print_printer_paused";
  if (status & (PRINTER_STATUS_ERROR | PRINTER_STATUS_NOT_AVAILABLE | PRINTER_STATUS_OUT_OF_MEMORY)) return "print_printer_error";
  if (status & (PRINTER_STATUS_PAPER_OUT | PRINTER_STATUS_PAPER_JAM | PRINTER_STATUS_PAPER_PROBLEM | PRINTER_STATUS_DOOR_OPEN | PRINTER_STATUS_NO_TONER |
                PRINTER_STATUS_OUTPUT_BIN_FULL | PRINTER_STATUS_USER_INTERVENTION | PRINTER_STATUS_MANUAL_FEED)) return "print_printer_attention";
  return "";
}

std::string LocalPrinterStatus(const std::wstring& printer) {
  if (printer.empty() || printer.size() > 512) return {};
  std::wstring name = printer;
  PRINTER_DEFAULTSW defaults{nullptr, nullptr, PRINTER_ACCESS_USE};
  HANDLE handle = nullptr;
  if (!OpenPrinterW(name.data(), &handle, &defaults)) return {};
  std::string code;
  DWORD needed = 0;
  GetPrinterW(handle, 2, nullptr, 0, &needed);
  if (needed && needed <= 1024 * 1024) {
    std::vector<BYTE> buffer(needed);
    if (GetPrinterW(handle, 2, buffer.data(), needed, &needed)) {
      const auto* info = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
      code = PrinterStatusCode(info->Status, info->Attributes);
    }
  }
  ClosePrinter(handle);
  return code;
}

int LocalPdfPages(const std::wstring& path) {
  const auto document = OpenPdf(path);
  return document ? static_cast<int>(document.PageCount()) : 0;
}

// One bounded, in-memory image of the original PDF page. Never opens a print
// queue or writes a preview file. The Dart presenter requests one at a time.
LocalPdfPreview PreviewLocalPdf(const std::wstring& path, int index) {
  constexpr uint64_t maximumBytes = 5 * 1024 * 1024;
  const auto document = OpenPdf(path);
  if (!document || index < 0 || static_cast<uint32_t>(index) >= document.PageCount()) return {};
  const auto page = document.GetPage(static_cast<uint32_t>(index));
  struct ClosePage { winrt::Windows::Data::Pdf::PdfPage page; ~ClosePage() { try { page.Close(); } catch (...) {} } } close{page};
  const auto size = page.Size();  // Includes the PDF's own rotation/crop box.
  if (!std::isfinite(size.Width) || !std::isfinite(size.Height) ||
      size.Width <= 1 || size.Height <= 1 || size.Width >= 100000 || size.Height >= 100000) return {};
  winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
  winrt::Windows::Data::Pdf::PdfPageRenderOptions options;
  if (size.Width >= size.Height) options.DestinationWidth(1024);
  else options.DestinationHeight(1024);
  options.IsIgnoringHighContrast(true);  // Preview the document, not the local theme.
  page.RenderToStreamAsync(stream, options).get();
  const uint64_t length = stream.Size();
  if (!length || length > maximumBytes) return {};
  winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
  if (reader.LoadAsync(static_cast<uint32_t>(length)).get() != length) return {};
  LocalPdfPreview preview;
  preview.width = size.Width;
  preview.height = size.Height;
  preview.page = index;
  preview.pages = static_cast<int>(document.PageCount());
  preview.png.resize(static_cast<size_t>(length));
  reader.ReadBytes(preview.png);
  return preview;
}

LocalPrintOutcome PrintLocalPdf(const std::wstring& path, const std::wstring& printer, int copies, const std::wstring& title, const PrintProgressFn& progress) {
  return PrintLocalPdfToStream(path, printer, copies, title, nullptr, progress);
}

LocalPrintOutcome PrintLocalPdfToStream(const std::wstring& path, const std::wstring& printer, int copies, const std::wstring& title, IStream* output, const PrintProgressFn& progress) {
  if (copies < 1 || copies > kMaximumCopies || printer.empty() || printer.size() > 512) return {false, "print_failed"};
  const auto printers = LocalPrinters();
  if (std::none_of(printers.begin(), printers.end(), [&](const LocalPrinterInfo& value) { return value.name == printer; })) return {false, "print_printer_unavailable"};
  const auto document = OpenPdf(path);
  if (!document) return {false, "print_document_invalid"};
  const uint32_t pages = document.PageCount();

  ComPtr<ID3D11Device> d3d;
  const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
  HRESULT hr = E_FAIL;
  for (const auto driver : drivers) {
    hr = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (SUCCEEDED(hr)) break;
  }
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<ID2D1Factory1> factory;
  ComPtr<ID2D1Device> device;
  ComPtr<ID2D1DeviceContext> context;
  ComPtr<IPdfRendererNative> renderer;
  ComPtr<IWICImagingFactory> wic;
  D2D1_FACTORY_OPTIONS options{};
  if (FAILED(hr) || FAILED(d3d.As(&dxgi)) ||
      FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, reinterpret_cast<void**>(factory.GetAddressOf()))) ||
      FAILED(factory->CreateDevice(dxgi.Get(), &device)) || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) ||
      FAILED(CreatePdfRenderer(dxgi.Get(), &renderer)) ||
      FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return {false, "print_unavailable"};

  // Keep printer defaults except orientation, which follows each PDF page.
  ComPtr<IPrintDocumentPackageTargetFactory> targets;
  ComPtr<IPrintDocumentPackageTarget> target;
  if (FAILED(CoCreateInstance(__uuidof(PrintDocumentPackageTargetFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&targets)))) return {false, "print_unavailable"};
  const std::wstring job = title.empty() ? L"Multitap" : title.substr(0, 200);
  if (FAILED(targets->CreateDocumentPackageTargetForPrintJob(printer.c_str(), job.c_str(), output, nullptr, &target))) return {false, "print_printer_unavailable"};
  // Told when the job has been handed over (or failed) before letting go.
  ComPtr<JobCompletion> completion;
  completion.Attach(new JobCompletion());
  ComPtr<IConnectionPointContainer> container;
  ComPtr<IConnectionPoint> point;
  DWORD cookie = 0;
  if (SUCCEEDED(target.As(&container)) && SUCCEEDED(container->FindConnectionPoint(__uuidof(IPrintDocumentPackageStatusEvent), &point)) &&
      FAILED(point->Advise(completion.Get(), &cookie))) cookie = 0;
  struct Unadvise { IConnectionPoint* point; DWORD cookie; ~Unadvise() { if (point && cookie) point->Unadvise(cookie); } } unadvise{point.Get(), cookie};
  const PaperArea paper = PrinterPaper(printer);
  D2D1_PRINT_CONTROL_PROPERTIES properties{D2D1_PRINT_FONT_SUBSET_MODE_DEFAULT, 300.0f, D2D1_COLOR_SPACE_SRGB};
  ComPtr<ID2D1PrintControl> control;
  if (FAILED(device->CreatePrintControl(wic.Get(), target.Get(), &properties, &control))) { target->Cancel(); return {false, "print_unavailable"}; }
  const int totalPages = static_cast<int>(pages) * copies;
  if (progress) progress(0, totalPages);
  // Collated copies: the whole document again for each copy.
  for (int copy = 0; copy < copies; ++copy) {
    for (uint32_t index = 0; index < pages; ++index) {
      try {
        const auto page = document.GetPage(index);
        const auto size = page.Size();
        if (!(size.Width > 1 && size.Height > 1 && size.Width < 100000 && size.Height < 100000)) throw std::exception();
        // Fit to the printable area: scaled down only, centred, the paper
        // turned to the page (no usable paper: 1:1 as before).
        const PageFit fit = FitPage(size.Width, size.Height, paper);
        ComPtr<ID2D1CommandList> list;
        if (FAILED(context->CreateCommandList(&list))) throw std::exception();
        context->SetTarget(list.Get());
        context->BeginDraw();
        context->SetTransform(D2D1::Matrix3x2F::Scale(fit.scale, fit.scale) * D2D1::Matrix3x2F::Translation(fit.offsetX, fit.offsetY));
        const HRESULT drawn = renderer->RenderPageToDeviceContext(reinterpret_cast<IUnknown*>(winrt::get_abi(page)), context.Get(), nullptr);
        const HRESULT ended = context->EndDraw();
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetTarget(nullptr);
        if (FAILED(drawn) || FAILED(ended) || FAILED(list->Close())) throw std::exception();
        const std::string ticketXml = PageOrientationTicket(fit.landscape);
        ComPtr<IStream> ticket;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &ticket))) throw std::exception();
        ULONG written = 0;
        if (FAILED(ticket->Write(ticketXml.data(), static_cast<ULONG>(ticketXml.size()), &written)) || written != ticketXml.size() ||
            FAILED(ticket->Seek({}, STREAM_SEEK_SET, nullptr))) throw std::exception();
        if (FAILED(control->AddPage(list.Get(), D2D1::SizeF(fit.paperWidth, fit.paperHeight), ticket.Get()))) throw std::exception();
        if (progress) progress(copy * static_cast<int>(pages) + static_cast<int>(index) + 1, totalPages);
      } catch (...) {
        target->Cancel();
        return {false, "print_failed"};
      }
    }
  }
  if (FAILED(control->Close())) { target->Cancel(); return {false, "print_failed"}; }
  // Still in progress after two minutes: the job is the spooler's now.
  const auto finished = cookie ? completion->Wait(120000) : PrintDocumentPackageCompletion_InProgress;
  // A printer that asks for its own dialog (Microsoft Print to PDF asks where
  // to save) and is closed by the person: a cancel, not a failure.
  if (finished == PrintDocumentPackageCompletion_Canceled) return {false, "print_cancelled"};
  if (finished == PrintDocumentPackageCompletion_Failed) return {false, "print_failed"};
  return {true, finished == PrintDocumentPackageCompletion_Completed ? "" : "print_in_progress", static_cast<int>(pages)};
}

LocalPrintOutcome SaveLocalPdf(HWND owner, const std::wstring& path, const std::wstring& suggestedName) {
  if (!PlainPdf(path)) return {false, "print_document_invalid"};
  ComPtr<IFileSaveDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {false, "print_save_failed"};
  const COMDLG_FILTERSPEC filter{multitap::Tr(L"PDF 문서 (*.pdf)", L"PDF document (*.pdf)"), L"*.pdf"};
  FILEOPENDIALOGOPTIONS flags = 0;
  dialog->GetOptions(&flags);
  dialog->SetOptions(flags | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST);
  dialog->SetFileTypes(1, &filter);
  dialog->SetDefaultExtension(L"pdf");
  dialog->SetTitle(multitap::Tr(L"PDF로 저장", L"Save as PDF"));
  const std::wstring name = suggestedName.empty() || suggestedName.size() > 200 ? std::wstring(multitap::Tr(L"Multitap 인쇄.pdf", L"Multitap print.pdf")) : suggestedName;
  dialog->SetFileName(name.c_str());
  PWSTR downloads = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &downloads))) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(downloads, nullptr, IID_PPV_ARGS(&folder)))) dialog->SetDefaultFolder(folder.Get());
    CoTaskMemFree(downloads);
  }
  const HRESULT shown = dialog->Show(owner);
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {false, "print_cancelled"};
  ComPtr<IShellItem> item;
  PWSTR chosen = nullptr;
  if (FAILED(shown) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) || !chosen) return {false, "print_save_failed"};
  const std::wstring destination(chosen);
  CoTaskMemFree(chosen);
  // Open the chosen file as this user, without
  // truncating, refuse a link or a file with other names (hard links), and
  // only then replace its content.
  const HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (output == INVALID_HANDLE_VALUE) return {false, "print_save_failed"};
  BY_HANDLE_FILE_INFORMATION info{};
  bool ok = GetFileInformationByHandle(output, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && info.nNumberOfLinks == 1 && SetEndOfFile(output);
  // The PDF came from the remote desktop: an Internet zone file (Mark-of-the-
  // Web) before any of its content lands, as shared-folder files are.
  ok = ok && MarkInternetZone(output, destination) == ERROR_SUCCESS;
  const HANDLE input = ok ? CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr) : INVALID_HANDLE_VALUE;
  ok = ok && input != INVALID_HANDLE_VALUE;
  std::vector<BYTE> buffer(1 << 20);
  while (ok) {
    DWORD read = 0, written = 0;
    if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
    if (!read) break;
    ok = WriteFile(output, buffer.data(), read, &written, nullptr) && written == read;
  }
  if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
  ok = ok && FlushFileBuffers(output);
  CloseHandle(output);
  return ok ? LocalPrintOutcome{true} : LocalPrintOutcome{false, "print_save_failed"};
}

struct ClientPrintChannel::State {
  struct Job { std::string method; M args; std::unique_ptr<flutter::MethodResult<V>> result; V value; };
  explicit State(HWND window) : window(window), thread([this] { Work(); }) {}
  HWND window;
  std::mutex mutex; std::condition_variable changed; std::deque<Job> jobs, complete; std::atomic<bool> stopped{false};
  // Pages of the running print job, read by the "progress" poll on the UI thread.
  std::atomic<int> pagesDone{0}, pagesTotal{0};
  std::thread thread;
  void Work() {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return stopped || !jobs.empty(); });
        if (stopped && jobs.empty()) break;
        job = std::move(jobs.front()); jobs.pop_front();
      }
      try {
        if (job.method == "printers") {
          flutter::EncodableList list;
          for (const auto& printer : LocalPrinters()) list.push_back(V(M{{V("name"), V(Utf8(printer.name))}, {V("default"), V(printer.isDefault)}}));
          job.value = V(std::move(list));
        } else if (job.method == "status") {
          const auto* printer = StringArgument(job.args, "printer");
          job.value = V(printer ? LocalPrinterStatus(Wide(*printer)) : std::string());
        } else if (job.method == "pages") {
          const auto* path = StringArgument(job.args, "path");
          job.value = V(static_cast<int32_t>(path ? LocalPdfPages(Wide(*path)) : 0));
        } else if (job.method == "preview") {
          const auto* path = StringArgument(job.args, "path");
          const auto preview = path ? PreviewLocalPdf(Wide(*path), IntArgument(job.args, "page", -1)) : LocalPdfPreview{};
          job.value = preview.png.empty() ? V() : V(M{
              {V("png"), V(preview.png)}, {V("width"), V(static_cast<double>(preview.width))},
              {V("height"), V(static_cast<double>(preview.height))}, {V("page"), V(preview.page)}, {V("pages"), V(preview.pages)}});
        } else {
          const auto* path = StringArgument(job.args, "path");
          const auto* printer = StringArgument(job.args, "printer");
          const auto* title = StringArgument(job.args, "title");
          pagesDone = 0; pagesTotal = 0;
          const auto outcome = path && printer ? PrintLocalPdf(Wide(*path), Wide(*printer), IntArgument(job.args, "copies", 0), title ? Wide(*title) : std::wstring(),
                                                               [this](int done, int total) { pagesTotal = total; pagesDone = done; })
                                               : LocalPrintOutcome{false, "print_failed"};
          pagesDone = 0; pagesTotal = 0;
          job.value = V(M{{V("ok"), V(outcome.ok)}, {V("code"), V(outcome.code)}, {V("pages"), V(static_cast<int32_t>(outcome.pages))}});
        }
      } catch (...) {
        job.value = job.method == "print" ? V(M{{V("ok"), V(false)}, {V("code"), V(std::string("print_failed"))}, {V("pages"), V(0)}}) : V();
      }
      { std::lock_guard lock(mutex); complete.push_back(std::move(job)); }
      PostMessageW(window, completed_message, 0, 0);
    }
    winrt::uninit_apartment();
  }
};

ClientPrintChannel::ClientPrintChannel(HWND window) : state_(std::make_unique<State>(window)) {}
ClientPrintChannel::~ClientPrintChannel() {
  { std::lock_guard lock(state_->mutex); state_->stopped = true; }
  state_->changed.notify_all();
  if (state_->thread.joinable()) state_->thread.join();
  Complete();
}
void ClientPrintChannel::Handle(const flutter::MethodCall<V>& call, std::unique_ptr<flutter::MethodResult<V>> result) {
  const auto& method = call.method_name();
  const auto* args = std::get_if<M>(call.arguments());
  if (method == "save") {
    const auto* path = args ? StringArgument(*args, "path") : nullptr;
    const auto* name = args ? StringArgument(*args, "name") : nullptr;
    if (!path) { result->Error("print_failed", "A document is required."); return; }
    const auto outcome = SaveLocalPdf(GetAncestor(state_->window, GA_ROOT), Wide(*path), name ? Wide(*name) : std::wstring());
    result->Success(V(M{{V("ok"), V(outcome.ok)}, {V("code"), V(outcome.code)}, {V("pages"), V(0)}}));
    return;
  }
  if (method == "progress") {
    // Answered at once on the UI thread from the printing thread's counters.
    int done = 0, total = 0;
    Progress(done, total);
    result->Success(V(M{{V("done"), V(static_cast<int32_t>(done))}, {V("total"), V(static_cast<int32_t>(total))}}));
    return;
  }
  if (method != "printers" && method != "pages" && method != "print" && method != "status" && method != "preview") { result->NotImplemented(); return; }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || state_->jobs.size() >= 8) { result->Error("print_busy", "The printing worker is busy."); return; }
    state_->jobs.push_back({method, args ? *args : M{}, std::move(result), V()});
  }
  state_->changed.notify_one();
}
void ClientPrintChannel::Progress(int& done, int& total) const {
  total = state_->pagesTotal.load();
  done = std::min(state_->pagesDone.load(), total);
}
void ClientPrintChannel::Complete() {
  std::deque<State::Job> done;
  { std::lock_guard lock(state_->mutex); done.swap(state_->complete); }
  for (auto& job : done) if (job.result) job.result->Success(job.value);
}
}  // namespace multitap
