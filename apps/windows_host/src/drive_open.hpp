#pragma once
// Private user-helper action. Callers derive root from a CURRENT owned mapping;
// no client-provided path, command, executable, verb or arguments reach this API.
#include "drive_protocol.hpp"
#include <objbase.h>
#include <shlguid.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shobjidl.h>
#include <servprov.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

namespace host::drive {
inline constexpr const char* openProtocol = "drive.open-v1";
inline bool openErrorCode(const std::string& code) {
  return code == "drive_share_unknown" || code == "drive_open_unavailable" ||
    code == "drive_open_failed" || code == "drive_open_unknown" ||
    code == "drive_busy" || code == "no_interactive_session";
}
inline bool openRequest(const Json& value, uint32_t& id, std::string& share) {
  const auto* object = std::get_if<Json::Object>(&value.value);
  const double number = value.get("id").number(0);
  share = value.get("share").string();
  if (!object || object->size() != 3 || value.get("type").string() != "drive.open" ||
      !std::isfinite(number) || number < 1 || number > 4294967295.0 ||
      std::floor(number) != number || !shareId(share)) return false;
  id = uint32_t(number); return true;
}
inline std::string openResponse(uint32_t id, const std::string& share, const std::string& error) {
  return "{\"type\":\"drive.opened\",\"id\":" + std::to_string(id) +
    ",\"share\":" + jsonQuote(share) + ",\"ok\":" + (error.empty() ? "true" : "false") +
    (error.empty() ? "" : ",\"error\":" + jsonQuote(openErrorCode(error) ? error : "drive_open_failed")) + "}";
}
inline bool ownedTargetMatches(const std::wstring& expected, const std::wstring& current) {
  return !expected.empty() && expected.front() == L'\\' && expected == current;
}
// A letter may have been removed/reused outside this helper. Match its current
// DOS target to the exact SDK-created volume, never just a label/serial/count.
inline bool currentOwnedDrive(const std::wstring& root, const std::wstring& expected) {
  if (root.size() != 3 || root[0] < L'D' || root[0] > L'Z' || root[1] != L':' || root[2] != L'\\' || expected.empty()) return false;
  const std::wstring letter = root.substr(0, 2);
  wchar_t target[4096]{};
  if (!QueryDosDeviceW(letter.c_str(), target, DWORD(std::size(target)))) return false;
  return ownedTargetMatches(expected, target);
}
namespace open_detail {
template<class T> struct Com {
  T* p = nullptr;
  ~Com() { if (p) p->Release(); }
  Com() = default; Com(const Com&) = delete; Com& operator=(const Com&) = delete;
  T** put() { return &p; }
  void** out() { return reinterpret_cast<void**>(&p); }
  T* operator->() const { return p; }
};
struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
inline bool sameLogon(HANDLE left, HANDLE right) {
  TOKEN_STATISTICS a{}, b{}; DWORD size = 0, as = 0, bs = 0, sessionA = 0, sessionB = 0;
  if (!GetTokenInformation(left, TokenStatistics, &a, sizeof(a), &size) ||
      !GetTokenInformation(right, TokenStatistics, &b, sizeof(b), &size) ||
      a.AuthenticationId.LowPart != b.AuthenticationId.LowPart ||
      a.AuthenticationId.HighPart != b.AuthenticationId.HighPart ||
      !GetTokenInformation(left, TokenSessionId, &sessionA, sizeof(sessionA), &size) ||
      !GetTokenInformation(right, TokenSessionId, &sessionB, sizeof(sessionB), &size) ||
      sessionA == 0 || sessionA != sessionB) return false;
  GetTokenInformation(left, TokenUser, nullptr, 0, &as);
  GetTokenInformation(right, TokenUser, nullptr, 0, &bs);
  if (!as || !bs || as > 65536 || bs > 65536) return false;
  std::vector<BYTE> au(as), bu(bs);
  return GetTokenInformation(left, TokenUser, au.data(), as, &as) &&
    GetTokenInformation(right, TokenUser, bu.data(), bs, &bs) &&
    EqualSid(reinterpret_cast<TOKEN_USER*>(au.data())->User.Sid,
             reinterpret_cast<TOKEN_USER*>(bu.data())->User.Sid);
}
}
// Delegate only to the already-running desktop shell of this SAME user logon.
// This avoids spawning Explorer inside the helper's kill-on-close job, and never
// falls back to SYSTEM, another login, an arbitrary executable or a command line.
inline std::string openCurrentUserRoot(const std::wstring& root) {
  const HWND shellWindow = GetShellWindow(); DWORD shellPid = 0;
  if (!shellWindow || !GetWindowThreadProcessId(shellWindow, &shellPid) || !shellPid)
    return "no_interactive_session";
  WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellPid));
  HANDLE ownRaw = nullptr, shellRaw = nullptr;
  if (!process.value || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownRaw)) return "drive_open_unavailable";
  WinHandle own(ownRaw);
  if (!OpenProcessToken(process.value, TOKEN_QUERY, &shellRaw)) return "drive_open_unavailable";
  WinHandle shellToken(shellRaw);
  if (!open_detail::sameLogon(own.value, shellToken.value)) return "no_interactive_session";
  open_detail::Apartment apartment;
  if (FAILED(apartment.result)) return "drive_open_unavailable";
  open_detail::Com<IShellWindows> windows;
  open_detail::Com<IDispatch> desktop, background, application;
  open_detail::Com<IServiceProvider> provider;
  open_detail::Com<IShellBrowser> browser;
  open_detail::Com<IShellView> view;
  open_detail::Com<IShellFolderViewDual> folder;
  open_detail::Com<IShellDispatch2> shell;
  VARIANT location{}, empty{}; location.vt = VT_I4; location.lVal = CSIDL_DESKTOP;
  long ignored = 0; HWND browserWindow = nullptr; DWORD browserPid = 0;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, windows.out())) || !windows.p ||
      FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &ignored, SWFO_NEEDDISPATCH, desktop.put())) || !desktop.p ||
      FAILED(desktop->QueryInterface(IID_IServiceProvider, provider.out())) || !provider.p ||
      FAILED(provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, browser.out())) || !browser.p ||
      FAILED(browser->GetWindow(&browserWindow)) || !browserWindow ||
      !GetWindowThreadProcessId(browserWindow, &browserPid) || browserPid != shellPid ||
      FAILED(browser->QueryActiveShellView(view.put())) || !view.p ||
      FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, background.out())) || !background.p ||
      FAILED(background->QueryInterface(IID_IShellFolderViewDual, folder.out())) || !folder.p ||
      FAILED(folder->get_Application(application.put())) || !application.p ||
      FAILED(application->QueryInterface(IID_IShellDispatch2, shell.out())) || !shell.p ||
      GetShellWindow() != shellWindow) return "drive_open_unavailable";
  VARIANT target{}; target.vt = VT_BSTR;
  target.bstrVal = SysAllocStringLen(root.data(), UINT(root.size()));
  if (!target.bstrVal) return "drive_open_unavailable";
  const HRESULT accepted = shell->Explore(target);
  VariantClear(&target);
  // A COM failure can be ambiguous; never automatically repeat it.
  return SUCCEEDED(accepted) ? std::string() : "drive_open_unknown";
}
}
