// print-v1 client printing on this PC's real Windows APIs, without paper or a
// dialog: a generated two-page PDF is opened with Windows.Data.Pdf, rendered
// through the Direct2D print control onto the in-box "Microsoft Print to PDF"
// printer with the job output written to a file (print to file), and the
// result is opened again and counted. Also the save path's link refusal and
// the printers listing. Prints nothing; installs and changes no printer.
#include "client_print.h"
#include <winspool.h>
#include "client_print_fit.h"

#include <shlwapi.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.Streams.h>
#include <shcore.h>

#include <chrono>
#include <cmath>
#include <regex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace {
int failures = 0;
void Require(bool ok, const char* what) {
  if (!ok) { ++failures; std::cerr << "FAILED: " << what << "\n"; }
}

// A small valid PDF: [pages] Letter pages, each with a line of text and a box.
std::string MakePdf(int pages) {
  std::vector<std::string> objects;
  objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  std::string kids;
  for (int i = 0; i < pages; ++i) kids += std::to_string(4 + i * 2) + " 0 R ";
  objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " >>");
  objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  for (int i = 0; i < pages; ++i) {
    const std::string content = "BT /F1 24 Tf 72 700 Td (Multitap print-v1 page " + std::to_string(i + 1) + ") Tj ET 72 72 300 200 re S";
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents " + std::to_string(5 + i * 2) + " 0 R >>");
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream");
  }
  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
  for (const auto offset : offsets) { char line[32]; std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset); out += line; }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return out;
}
}  // namespace

// Fit math: portrait/landscape, A4 vs Letter, margins (DIPs, 96 per inch).
bool Near(float a, float b) { return std::fabs(a - b) < 0.05f; }
void CheckFit() {
  using multitap::FitPage; using multitap::PaperArea;
  const PaperArea a4{793.7f, 1122.5f, 16.0f}, letter{816.0f, 1056.0f, 16.0f};
  // A Letter page on A4: wider than the printable width, so scaled down, centred.
  auto f = FitPage(816, 1056, a4);
  Require(f.fitted && !f.landscape && Near(f.paperWidth, 793.7f) && Near(f.paperHeight, 1122.5f), "portrait page: portrait A4");
  Require(f.scale < 1 && Near(f.scale, (793.7f - 32) / 816) && Near(f.offsetX, (793.7f - 816 * f.scale) / 2) && Near(f.offsetY, (1122.5f - 1056 * f.scale) / 2), "Letter on A4 shrinks to the printable width, centred");
  Require(f.offsetX >= 16 - 0.05f && f.offsetY >= 16 - 0.05f, "nothing falls inside the margins");
  // A4 page on Letter: the height limits it.
  f = FitPage(793.7f, 1122.5f, letter);
  Require(Near(f.scale, (1056 - 32) / 1122.5f), "A4 on Letter shrinks to the printable height");
  // A page that fits is never enlarged and sits in the middle.
  f = FitPage(400, 500, a4);
  Require(f.scale == 1 && Near(f.offsetX, (793.7f - 400) / 2) && Near(f.offsetY, (1122.5f - 500) / 2), "a small page stays 100% and centred");
  // Landscape page turns the paper.
  f = FitPage(1056, 816, letter);
  Require(f.landscape && Near(f.paperWidth, 1056) && Near(f.paperHeight, 816) && Near(f.scale, (816 - 32) / 816.0f), "landscape Letter page: landscape paper, limited by height");
  f = FitPage(1122.5f, 793.7f, a4);
  Require(f.landscape && Near(f.paperWidth, 1122.5f) && f.scale < 1, "landscape A4 page on A4 paper with margins shrinks slightly");
  // Zero margin and exact size: unchanged.
  f = FitPage(793.7f, 1122.5f, {793.7f, 1122.5f, 0});
  Require(f.scale == 1 && Near(f.offsetX, 0) && Near(f.offsetY, 0), "borderless exact fit is 1:1 at the origin");
  // Paper given long side first, or unknown: no change / 1:1.
  f = FitPage(612, 792, {1122.5f, 793.7f, 10});
  Require(f.fitted && !f.landscape && Near(f.paperWidth, 793.7f), "paper edges are ordered by size");
  f = FitPage(612, 792, {});
  Require(!f.fitted && f.scale == 1 && f.offsetX == 0 && Near(f.paperWidth, 612) && Near(f.paperHeight, 792), "unknown paper keeps the page size 1:1");
  f = FitPage(612, 792, {100, 200, 60});
  Require(!f.fitted, "margins eating the paper are not trusted");
}

// What the spooler's status bits mean for the person at the dialog.
void CheckStatus() {
  const auto code = [](DWORD status, DWORD attributes = 0) { return std::string(multitap::PrinterStatusCode(status, attributes)); };
  Require(code(0).empty() && code(PRINTER_STATUS_PRINTING).empty() && code(PRINTER_STATUS_TONER_LOW).empty() && code(PRINTER_STATUS_BUSY).empty(), "a working printer (even printing, busy or low on toner) has no warning");
  Require(code(PRINTER_STATUS_OFFLINE) == "print_printer_offline" && code(0, PRINTER_ATTRIBUTE_WORK_OFFLINE) == "print_printer_offline", "offline by status or by 'use printer offline'");
  Require(code(PRINTER_STATUS_PAUSED) == "print_printer_paused", "paused");
  Require(code(PRINTER_STATUS_PAPER_OUT) == "print_printer_attention" && code(PRINTER_STATUS_PAPER_JAM) == "print_printer_attention" && code(PRINTER_STATUS_DOOR_OPEN) == "print_printer_attention" && code(PRINTER_STATUS_NO_TONER) == "print_printer_attention" && code(PRINTER_STATUS_USER_INTERVENTION) == "print_printer_attention", "paper, jam, cover, toner and user intervention need attention");
  Require(code(PRINTER_STATUS_ERROR) == "print_printer_error" && code(PRINTER_STATUS_NOT_AVAILABLE) == "print_printer_error", "error and not available");
  Require(code(PRINTER_STATUS_OFFLINE | PRINTER_STATUS_PAPER_OUT) == "print_printer_offline" && code(PRINTER_STATUS_PAUSED | PRINTER_STATUS_PAPER_OUT) == "print_printer_paused" && code(PRINTER_STATUS_ERROR | PRINTER_STATUS_PAPER_OUT) == "print_printer_error", "the most blocking reason wins");
  Require(multitap::LocalPrinterStatus(L"Multitap no such printer").empty() && multitap::LocalPrinterStatus(L"").empty(), "an unknown printer cannot be asked: no warning, no crash");
  for (const auto& printer : multitap::LocalPrinters()) if (printer.name == L"Microsoft Print to PDF") Require(multitap::LocalPrinterStatus(printer.name).empty(), "the in-box PDF printer is ready");
}

int main() {
  CheckFit();
  CheckStatus();
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  const auto root = std::filesystem::temp_directory_path() / (L"multitap-client-print-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ignored; std::filesystem::remove_all(root, ignored); std::filesystem::create_directories(root);
  const auto input = root / L"document.pdf", output = root / L"printed.pdf";
  { std::ofstream file(input, std::ios::binary); const auto pdf = MakePdf(2); file.write(pdf.data(), std::streamsize(pdf.size())); }

  Require(multitap::LocalPdfPages(input.wstring()) == 2, "Windows.Data.Pdf opens the document with its 2 pages");
  { std::ofstream file(root / L"not.pdf", std::ios::binary); file << "hello"; }
  Require(multitap::LocalPdfPages((root / L"not.pdf").wstring()) == 0, "a file that is not a PDF is refused");
  const auto printers = multitap::LocalPrinters();
  std::wstring target;
  for (const auto& printer : printers) if (printer.name == L"Microsoft Print to PDF") target = printer.name;
  const auto refused = multitap::PrintLocalPdf(input.wstring(), L"Multitap no such printer", 1, L"smoke");
  Require(!refused.ok && refused.code == "print_printer_unavailable", "an unknown printer is refused before anything is sent");
  Require(!multitap::PrintLocalPdf(input.wstring(), target.empty() ? L"x" : target, 0, L"smoke").ok, "zero copies are refused");

  int images = -1, fonts = -1; std::string boxes; bool printed = false; int outputPages = 0; uint64_t outputBytes = 0;
  if (target.empty()) {
    std::cout << "{\"printToPdfAvailable\":false}\n";
  } else {
    IStream* stream = nullptr;
    Require(SUCCEEDED(SHCreateStreamOnFileEx(output.c_str(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream)), "output stream");
    if (stream) {
      std::vector<std::pair<int, int>> reported;
        const auto outcome = multitap::PrintLocalPdfToStream(input.wstring(), target, 2, L"Multitap print smoke", stream,
                                                            [&](int done, int total) { reported.emplace_back(done, total); });
      std::cout << "{\"printOk\":" << (outcome.ok ? "true" : "false") << ",\"code\":\"" << outcome.code << "\",\"pages\":" << outcome.pages << "}\n";
      Require(outcome.ok && outcome.code.empty() && outcome.pages == 2, "the job was handed over completely (package completion)");
      // Real page callbacks: 0/4 first, then 1..4 of 4 (2 pages x 2 copies).
      Require(reported.size() == 5 && reported.front() == std::make_pair(0, 4) && reported.back() == std::make_pair(4, 4), "progress was reported per page");
      for (size_t i = 1; i < reported.size(); ++i) Require(reported[i].first == static_cast<int>(i) && reported[i].second == 4, "progress counts up one page at a time");
      // The driver's output came back through this stream; its buffered
      // writes reach the file when the stream is released.
      stream->Release();
      std::ifstream file(output, std::ios::binary); std::stringstream all; all << file.rdbuf();
      const auto text = all.str();
      if (text.rfind("%PDF-", 0) == 0 && text.find("%%EOF", text.size() > 64 ? text.size() - 64 : 0) != std::string::npos) outputBytes = text.size();
      file.close();
      outputPages = outputBytes ? multitap::LocalPdfPages(output.wstring()) : 0;
      printed = outputBytes > 0;
      // Vector path: text stays text (fonts) and no page is one image.
      const auto count = [&](const char* needle) { int n = 0; for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n; return n; };
      images = count("/Subtype/Image") + count("/Subtype /Image"); fonts = count("/FontFile") + count("/Type/Font") + count("/Type /Font");
      // Page boxes of the driver output (points), when stored uncompressed.
      const std::regex mediaBox("/MediaBox\\s*\\[([^\\]]*)\\]");
      for (std::sregex_iterator it(text.begin(), text.end(), mediaBox), end; it != end; ++it) boxes += "[" + (*it)[1].str() + "]";
      Require(printed, "the printer driver produced a complete PDF");
      Require(images == 0 && fonts > 0, "output is vector: fonts present, no page image");
      Require(outputPages == 4, "two collated copies of two pages came out (4 pages)");
    }
  }
  std::cout << "{\"kind\":\"client-print-v1\",\"failures\":" << failures << ",\"printers\":" << printers.size() << ",\"printToPdf\":" << (target.empty() ? "false" : "true")
            << ",\"printedToFile\":" << (printed ? "true" : "false") << ",\"outputPages\":" << outputPages << ",\"outputBytes\":" << outputBytes << ",\"images\":" << images << ",\"fonts\":" << fonts << ",\"mediaBoxes\":\"" << boxes << "\",\"paper\":false}\n";
  std::filesystem::remove_all(root, ignored);
  return failures ? 1 : 0;
}
