// Mark-of-the-Web helper (windows/runner/zone_identifier.h) used by the
// shared folders and "PDF로 저장": in a private temporary folder only.
#include <windows.h>

#include <cstdio>
#include <string>

#include "zone_identifier.h"

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}
std::string ReadText(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return "<missing>";
  std::string out(4096, '\0');
  DWORD got = 0;
  ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
  CloseHandle(file);
  out.resize(got);
  return out;
}
bool WriteText(const std::wstring& path, const std::string& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
  CloseHandle(file);
  return ok;
}
}  // namespace

int main() {
  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring base = std::wstring(temp) + L"multitap-zone-" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(base.c_str(), nullptr);
  DWORD flags = 0;
  GetVolumeInformationW(base.substr(0, 3).c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0);
  if (!(flags & FILE_NAMED_STREAMS)) {
    std::printf("note: the temporary folder's volume has no named streams; nothing to check\n");
    RemoveDirectoryW(base.c_str());
    return 0;
  }
  const std::string internet = multitap::kInternetZoneIdentifier;

  // As SaveLocalPdf does: an exclusive handle on the content, a plain path.
  const std::wstring pdf = base + L"\\print.pdf";
  HANDLE output = CreateFileW(pdf.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  Check(output != INVALID_HANDLE_VALUE, "open the output exclusively");
  Check(multitap::MarkInternetZone(output, pdf) == ERROR_SUCCESS, "mark beside an exclusive content handle");
  DWORD written = 0;
  Check(WriteFile(output, "%PDF-1.7", 8, &written, nullptr) && written == 8, "content written after the mark");
  CloseHandle(output);
  Check(ReadText(pdf + L":Zone.Identifier") == internet, "the PDF carries the Internet zone");
  Check(ReadText(pdf) == "%PDF-1.7", "the content is unchanged by the mark");

  // An existing stronger or equal mark is kept; a weaker one is replaced.
  const std::wstring file = base + L"\\file.txt";
  Check(WriteText(file, "x"), "fixture file");
  HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  Check(handle != INVALID_HANDLE_VALUE, "open fixture file");
  const std::string restricted = "[ZoneTransfer]\r\nZoneId=4\r\n";
  Check(WriteText(file + L":Zone.Identifier", restricted), "fixture restricted mark");
  Check(multitap::MarkInternetZone(handle, file) == ERROR_SUCCESS && ReadText(file + L":Zone.Identifier") == restricted, "a Restricted zone mark is kept");
  Check(WriteText(file + L":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=0\r\n"), "fixture local mark");
  Check(multitap::MarkInternetZone(handle, file) == ERROR_SUCCESS && ReadText(file + L":Zone.Identifier") == internet, "a Local zone mark is replaced");
  Check(WriteText(file + L":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=30\r\n"), "fixture odd mark");
  Check(multitap::MarkInternetZone(handle, file) == ERROR_SUCCESS && ReadText(file + L":Zone.Identifier") == internet, "an unknown zone value is replaced");
  CloseHandle(handle);

  // A path longer than MAX_PATH (plain form; the helper takes the long form).
  std::wstring deep = base;
  for (int i = 0; i < 4; ++i) {
    deep += L"\\" + std::wstring(60, static_cast<wchar_t>(L'a' + i));
    CreateDirectoryW((L"\\\\?\\" + deep).c_str(), nullptr);
  }
  const std::wstring longFile = deep + L"\\long.pdf";
  Check(longFile.size() > MAX_PATH, "fixture path is longer than MAX_PATH");
  HANDLE longHandle = CreateFileW((L"\\\\?\\" + longFile).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(longHandle != INVALID_HANDLE_VALUE, "create the long-path file");
  Check(multitap::MarkInternetZone(longHandle, longFile) == ERROR_SUCCESS, "mark a long path");
  CloseHandle(longHandle);
  Check(ReadText(L"\\\\?\\" + longFile + L":Zone.Identifier") == internet, "the long-path file carries the mark");

  DeleteFileW((L"\\\\?\\" + longFile).c_str());
  for (int i = 0; i < 4; ++i) {
    RemoveDirectoryW((L"\\\\?\\" + deep).c_str());
    deep = deep.substr(0, deep.find_last_of(L'\\'));
  }
  DeleteFileW(pdf.c_str());
  DeleteFileW(file.c_str());
  RemoveDirectoryW(base.c_str());
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
