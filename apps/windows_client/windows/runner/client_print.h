#pragma once
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <windows.h>
#include <objidl.h>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multitap {
// print-v1 on this PC (lib/data/native_print.dart): the PDF a VM printed to
// its Multitap printer is opened with Windows' own PDF renderer
// (Windows.Data.Pdf) and printed through the Direct2D print control on the XPS
// print path (vector pages, the printer's own driver and defaults), or copied
// where the user saves it. No third-party PDF engine. Only fixed result codes
// leave this file; document names and paths are never logged.
struct LocalPrinterInfo { std::wstring name; bool isDefault = false; };
std::vector<LocalPrinterInfo> LocalPrinters();
struct LocalPrintOutcome { bool ok = false; std::string code; int pages = 0; };
// What the spooler says is wrong with a printer, as a fixed code (a printer
// that is offline or paused still takes the job; it waits in its queue):
// print_printer_offline, print_printer_paused, print_printer_attention (paper,
// toner, cover), print_printer_error, or "" when nothing is wrong.
const char* PrinterStatusCode(DWORD status, DWORD attributes);
// The same for the named printer, asked of the spooler; "" when it cannot be
// asked.
std::string LocalPrinterStatus(const std::wstring& printer);
// The page count, or 0 when Windows cannot open the document as a PDF.
int LocalPdfPages(const std::wstring& path);
struct LocalPdfPreview {
  float width = 0, height = 0;
  int page = 0, pages = 0;
  std::vector<uint8_t> png;
};
LocalPdfPreview PreviewLocalPdf(const std::wstring& path, int page);
// [progress] (optional) is told after every page handed to the print control:
// (pages done, pages in all, copies included); called on the printing thread.
using PrintProgressFn = std::function<void(int done, int total)>;
LocalPrintOutcome PrintLocalPdf(const std::wstring& path, const std::wstring& printer, int copies, const std::wstring& title, const PrintProgressFn& progress = nullptr);
// The same, with the printer driver's output written to [output] (print to
// file) instead of the printer's port; for the native smoke test.
LocalPrintOutcome PrintLocalPdfToStream(const std::wstring& path, const std::wstring& printer, int copies, const std::wstring& title, IStream* output, const PrintProgressFn& progress = nullptr);
// Asks where to save (Windows' save dialog over [owner]) and writes a copy
// there, never through a link or into a file with other names (hard links).
LocalPrintOutcome SaveLocalPdf(HWND owner, const std::wstring& path, const std::wstring& suggestedName);

class ClientPrintChannel {
 public:
  static constexpr UINT completed_message = WM_APP + 191;
  explicit ClientPrintChannel(HWND reply_window);
  ~ClientPrintChannel();
  // printers / pages / preview / print run on a worker thread (MTA); save shows its
  // dialog on the calling (UI) thread.
  void Handle(const flutter::MethodCall<flutter::EncodableValue>& call,
              std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void Complete();
  // The running print job's progress (pages done / in all), or 0/0 when none.
  void Progress(int& done, int& total) const;
 private:
  struct State; std::unique_ptr<State> state_;
};
}
