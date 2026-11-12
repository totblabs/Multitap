#include "paste_target.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace multitap {
namespace {

using Microsoft::WRL::ComPtr;
using V = flutter::EncodableValue;

bool IsFolder(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// A folder on disk with Explorer's name for it; nothing for a virtual place (This PC, a library, a zip).
std::optional<PasteFolder> FolderOf(PCIDLIST_ABSOLUTE pidl) {
  PWSTR path = nullptr;
  if (FAILED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path)) || !path) return std::nullopt;
  PasteFolder folder{path, {}};
  CoTaskMemFree(path);
  if (folder.path.size() < 3 || !IsFolder(folder.path)) return std::nullopt;
  PWSTR display = nullptr;
  if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &display)) && display) {
    folder.name = display;
    CoTaskMemFree(display);
  }
  if (folder.name.empty()) folder.name = folder.path;
  return folder;
}

std::optional<PasteFolder> DesktopFolder() {
  PIDLIST_ABSOLUTE pidl = nullptr;
  if (FAILED(SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &pidl)) || !pidl) return std::nullopt;
  auto folder = FolderOf(pidl);
  CoTaskMemFree(pidl);
  return folder;
}

// Whether the tab a shell view belongs to is the one shown. The view's own parents include hidden helper windows
// (Windows 11: CtrlNotifySink), so the tab window's WS_VISIBLE decides; a view outside any tab window is shown.
bool TabShown(HWND view, HWND root) {
  for (HWND w = view; w && w != root; w = GetParent(w)) {
    wchar_t name[32]{};
    GetClassNameW(w, name, 32);
    if (std::wstring_view(name) == L"ShellTabWindowClass") return (GetWindowLongW(w, GWL_STYLE) & WS_VISIBLE) != 0;
  }
  return view != nullptr;
}

// The folder of the tab on screen in the File Explorer window `root`.
std::optional<PasteFolder> ExplorerFolder(HWND root) {
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return std::nullopt;
  long count = 0;
  if (FAILED(windows->get_Count(&count)) || count <= 0) return std::nullopt;
  std::vector<bool> visible;
  std::vector<std::optional<PasteFolder>> folders;
  for (long i = 0; i < count && i < 256; ++i) {
    VARIANT index;
    VariantInit(&index);
    index.vt = VT_I4;
    index.lVal = i;
    ComPtr<IDispatch> dispatch;
    if (windows->Item(index, &dispatch) != S_OK || !dispatch) continue;
    ComPtr<IWebBrowserApp> app;
    SHANDLE_PTR handle = 0;
    if (FAILED(dispatch.As(&app)) || FAILED(app->get_HWND(&handle)) || reinterpret_cast<HWND>(handle) != root) continue;
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    if (FAILED(dispatch.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || !view) {
      continue;
    }
    // Every tab of the window has its own view; only the one on screen counts.
    HWND shown = nullptr;
    visible.push_back(SUCCEEDED(view->GetWindow(&shown)) && TabShown(shown, root));
    std::optional<PasteFolder> folder;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> persist;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(view.As(&folderView)) && SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(&persist))) &&
        SUCCEEDED(persist->GetCurFolder(&pidl)) && pidl) {
      folder = FolderOf(pidl);
      CoTaskMemFree(pidl);
    }
    folders.push_back(std::move(folder));
  }
  const auto shown = ShownView(visible);
  if (!shown) return std::nullopt;
  return folders[*shown];
}

std::string Utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(size > 0 ? size : 0), '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), size, nullptr, nullptr);
  return out;
}
std::wstring Wide(const std::string& value) {
  if (value.empty() || value.size() > 32767) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
  std::wstring out(size_t(size > 0 ? size : 0), L'\0');
  if (size > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), out.data(), size);
  return out;
}
std::string Argument(const flutter::EncodableValue* arguments, const char* key) {
  const auto* map = arguments ? std::get_if<flutter::EncodableMap>(arguments) : nullptr;
  if (!map) return {};
  const auto found = map->find(V(key));
  const auto* value = found == map->end() ? nullptr : std::get_if<std::string>(&found->second);
  return value ? *value : std::string();
}

}  // namespace

std::optional<PasteFolder> ShellFolderOf(HWND window) {
  const HWND root = window ? GetAncestor(window, GA_ROOT) : nullptr;
  if (!root || !IsWindow(root)) return std::nullopt;
  wchar_t name[64]{};
  if (!GetClassNameW(root, name, 64)) return std::nullopt;
  const bool desktopView = FindWindowExW(root, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
  switch (ClassifyPasteWindow(name, desktopView)) {
    case PasteWindow::desktop:
      return DesktopFolder();
    case PasteWindow::explorer:
      return ExplorerFolder(root);
    default:
      return std::nullopt;
  }
}

bool OpenShellFolder(const std::wstring& folder, const std::wstring& select) {
  if (folder.size() < 3 || folder.size() > 32767 || !IsFolder(folder)) return false;
  std::wstring target = folder;
  bool item = false;
  if (PlainItemName(select)) {
    std::wstring path = folder;
    if (path.back() != L'\\') path += L'\\';
    path += select;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      target = path;
      item = true;
    }
  }
  PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(target.c_str());
  if (!pidl) return false;
  bool opened = false;
  if (item) {
    // A full item ID and no children: Explorer opens its folder with the item selected.
    opened = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
  } else {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpIDList = pidl;
    info.nShow = SW_SHOWNORMAL;
    opened = ShellExecuteExW(&info) != FALSE;
  }
  ILFree(pidl);
  return opened;
}

struct PasteTargetChannel::State {
  explicit State(HWND window_in) : window(window_in) {}
  HWND window;
  std::mutex mutex;
  bool stopped = false;
  int running = 0;
  uint64_t serial = 0;
  std::map<uint64_t, std::unique_ptr<flutter::MethodResult<V>>> waiting;  // the window's thread only
  std::deque<std::pair<uint64_t, V>> done;
};

PasteTargetChannel::PasteTargetChannel(HWND window) : state_(std::make_shared<State>(window)) {}

PasteTargetChannel::~PasteTargetChannel() {
  std::lock_guard lock(state_->mutex);
  state_->stopped = true;  // a worker still in Explorer finishes on its own and answers nobody
}

void PasteTargetChannel::Handle(const flutter::MethodCall<V>& call, std::unique_ptr<flutter::MethodResult<V>> result) {
  const auto& method = call.method_name();
  std::function<V()> work;
  if (method == "capture") {
    // The window in front is read right away: a moment later it may be another one.
    const HWND front = GetForegroundWindow();
    work = [front]() -> V {
      const auto folder = front ? ShellFolderOf(front) : std::nullopt;
      if (!folder) return V();
      return V(flutter::EncodableMap{{V("path"), V(Utf8(folder->path))}, {V("name"), V(Utf8(folder->name))}});
    };
  } else if (method == "open") {
    const auto folder = Wide(Argument(call.arguments(), "folder"));
    const auto select = Wide(Argument(call.arguments(), "select"));
    if (folder.empty()) {
      result->Success(V(false));
      return;
    }
    // The click came to this application: Explorer may come to the front with the folder.
    AllowSetForegroundWindow(ASFW_ANY);
    work = [folder, select] { return V(OpenShellFolder(folder, select)); };
  } else {
    result->NotImplemented();
    return;
  }
  uint64_t id = 0;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->running >= 4) {
      result->Success(V());
      return;
    }
    ++state_->running;
    id = ++state_->serial;
  }
  state_->waiting.emplace(id, std::move(result));
  std::thread([state = state_, id, work = std::move(work)] {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    V value;
    try {
      value = work();
    } catch (...) {
      value = V();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    std::lock_guard lock(state->mutex);
    --state->running;
    if (state->stopped) return;
    state->done.emplace_back(id, std::move(value));
    PostMessageW(state->window, completed_message, 0, 0);
  }).detach();
}

void PasteTargetChannel::Complete() {
  std::deque<std::pair<uint64_t, V>> done;
  {
    std::lock_guard lock(state_->mutex);
    done.swap(state_->done);
  }
  for (auto& [id, value] : done) {
    const auto found = state_->waiting.find(id);
    if (found == state_->waiting.end()) continue;
    auto result = std::move(found->second);
    state_->waiting.erase(found);
    result->Success(value);
  }
}

}  // namespace multitap
