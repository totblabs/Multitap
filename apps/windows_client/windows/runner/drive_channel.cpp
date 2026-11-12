#include "drive_channel.h"

#include <shobjidl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "drive_files_core.h"
#include "ui_language.h"

namespace multitap {
namespace {
using V = flutter::EncodableValue;
using M = flutter::EncodableMap;
using L = flutter::EncodableList;
namespace drive_core = multitap::drive;

[[noreturn]] void Invalid() { throw drive_core::Failure("drive_invalid_path"); }
const V& Argument(const M& map, const char* key) {
  const auto found = map.find(V(key));
  if (found == map.end()) Invalid();
  return found->second;
}
std::string String(const M& map, const char* key) {
  const auto* value = std::get_if<std::string>(&Argument(map, key));
  if (!value || value->size() > 32768) Invalid();
  return *value;
}
std::wstring Path(const M& map, const char* key) {
  try { return drive_core::Utf8ToWide(String(map, key)); } catch (const drive_core::Failure&) { throw; } catch (...) { Invalid(); }
}
bool Bool(const M& map, const char* key) {
  const auto* value = std::get_if<bool>(&Argument(map, key));
  if (!value) Invalid();
  return *value;
}
int64_t Integer(const M& map, const char* key) {
  const auto& value = Argument(map, key);
  if (const auto* number = std::get_if<int32_t>(&value)) return *number;
  if (const auto* number = std::get_if<int64_t>(&value)) return *number;
  Invalid();
}
V EntryValue(const drive_core::Entry& entry) {
  M out{{V("name"), V(drive_core::WideToUtf8(entry.name))},
        {V("dir"), V(entry.dir)},
        {V("size"), V(static_cast<int64_t>(entry.size))},
        {V("modified"), V(entry.modified)},
        {V("created"), V(entry.created)},
        {V("readOnly"), V(entry.readOnly)}};
  if (entry.hasSpace) {
    out[V("free")] = V(static_cast<int64_t>(entry.free));
    out[V("total")] = V(static_cast<int64_t>(entry.total));
  }
  return V(std::move(out));
}

struct OwnedHandle {
  HANDLE value = nullptr;
  ~OwnedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool ProcessElevated() {
  OwnedHandle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return true;  // unknown: fail closed
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  if (!GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size)) return true;
  return elevation.TokenIsElevated != 0;
}

// An impersonation token of the desktop shell's user (the Explorer the person
// at this PC runs), the same source as CheckDesktopRead (client_clipboard.cpp).
HANDLE DesktopUserToken() {
  DWORD pid = 0;
  if (const HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &pid);
  if (!pid) return nullptr;
  OwnedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)}, token;
  if (!process.value || !OpenProcessToken(process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &token.value)) return nullptr;
  HANDLE impersonation = nullptr;
  if (!DuplicateTokenEx(token.value, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation, &impersonation)) return nullptr;
  return impersonation;
}

std::wstring Leaf(std::wstring path) {
  while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
  if (path.size() <= 3 && path.size() >= 2 && path[1] == L':') return path.substr(0, 1);
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
}  // namespace

struct ClientDriveChannel::State {
  struct Job {
    std::string method;
    M args;
    std::unique_ptr<flutter::MethodResult<V>> result;
    V value;
    std::string error;
    DWORD win32 = 0;
  };
  explicit State(HWND window) : window(window) {}
  HWND window;
  drive_core::Files files;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Job> jobs, complete;
  std::vector<std::thread> workers;
  bool stopped = false, picking = false;
  const bool elevated = ProcessElevated();

  void Start() {
    // Called with `mutex` held; threads start on first use only.
    if (!workers.empty()) return;
    for (int i = 0; i < 3; ++i) workers.emplace_back([this] { Work(); });
  }

  void Work() {
    HANDLE token = nullptr;
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return stopped || !jobs.empty(); });
        if (stopped) break;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      try {
        // While elevated, every file operation runs as the desktop user.
        if (elevated && !token) {
          token = DesktopUserToken();
          if (!token || !SetThreadToken(nullptr, token)) {
            if (token) CloseHandle(token);
            token = nullptr;
            throw drive_core::Failure(job.method == "open" ? "drive_user_unavailable" : "drive_denied");
          }
        }
        job.value = Run(job.method, job.args);
      } catch (const drive_core::Failure& failure) {
        job.error = failure.what();
        job.win32 = failure.win32();
      } catch (...) {
        job.error = "drive_io_failed";
      }
      {
        std::lock_guard lock(mutex);
        complete.push_back(std::move(job));
      }
      PostMessageW(window, completed_message, 0, 0);
    }
    if (token) {
      SetThreadToken(nullptr, nullptr);
      CloseHandle(token);
    }
  }

  V Run(const std::string& method, const M& args) {
    const auto share = method == "pickFolder" ? std::string() : String(args, "share");
    if (method == "open") {
      const auto label = files.Open(share, Path(args, "path"), Bool(args, "readOnly"));
      return V(M{{V("name"), V(drive_core::WideToUtf8(label))}});
    }
    if (method == "status") {
      const auto status = files.StatusOf(share);
      return V(M{{V("deleted"), V(status.deleted)},
                 {V("offline"), V(status.offline)},
                 {V("path"), V(drive_core::WideToUtf8(status.path))},
                 {V("name"), V(drive_core::WideToUtf8(status.label))}});
    }
    if (method == "close") {
      files.Close(share);
      return V();
    }
    const auto path = Path(args, "path");
    if (method == "stat") return EntryValue(files.Stat(share, path));
    if (method == "list") {
      L entries;
      for (const auto& entry : files.List(share, path)) entries.push_back(EntryValue(entry));
      return V(M{{V("entries"), V(std::move(entries))}});
    }
    if (method == "read") {
      const auto offset = Integer(args, "offset"), length = Integer(args, "length");
      if (offset < 0 || length < 1 || length > drive_core::kMaxChunk) Invalid();
      return V(files.Read(share, path, static_cast<uint64_t>(offset), static_cast<uint32_t>(length)));
    }
    if (method == "write") {
      const auto offset = Integer(args, "offset");
      const auto* data = std::get_if<std::vector<uint8_t>>(&Argument(args, "data"));
      if (offset < 0 || !data) Invalid();
      files.Write(share, path, static_cast<uint64_t>(offset), data->data(), data->size(), Bool(args, "truncate"));
      return V();
    }
    if (method == "setSize") {
      const auto size = Integer(args, "size");
      if (size < 0) Invalid();
      files.SetSize(share, path, static_cast<uint64_t>(size));
      return V();
    }
    if (method == "flush") {
      files.Flush(share, path);
      return V();
    }
    if (method == "create") {
      files.Create(share, path, Bool(args, "dir"), Bool(args, "truncate"));
      return V();
    }
    if (method == "rename") {
      files.Rename(share, path, Path(args, "to"), Bool(args, "replace"));
      return V();
    }
    if (method == "delete") {
      files.Delete(share, path);
      return V();
    }
    if (method == "touch") {
      files.Touch(share, path, Integer(args, "modified"));
      return V();
    }
    Invalid();
  }

  // The folder picker, on the platform thread with this window as owner.
  void Pick(std::unique_ptr<flutter::MethodResult<V>> result) {
    if (picking) {
      result->Error("drive_busy", "A folder picker is already open.");
      return;
    }
    picking = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{picking};
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
      result->Error("drive_io_failed", "Folder picker unavailable.");
      return;
    }
    struct Release { IUnknown* value; ~Release() { if (value) value->Release(); } } release_dialog{dialog};
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(multitap::Tr(L"데스크톱에 공유할 폴더 선택", L"Choose a folder to share with the desktop"));
    const HRESULT shown = dialog->Show(GetAncestor(window, GA_ROOT));
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      result->Success(V());
      return;
    }
    IShellItem* item = nullptr;
    if (FAILED(shown) || FAILED(dialog->GetResult(&item)) || !item) {
      result->Error("drive_io_failed", "Folder picker failed.");
      return;
    }
    Release release_item{item};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
      result->Error("drive_not_found", "Not a file-system folder.");
      return;
    }
    std::wstring text(path);
    CoTaskMemFree(path);
    try {
      result->Success(V(M{{V("path"), V(drive_core::WideToUtf8(text))}, {V("name"), V(drive_core::WideToUtf8(Leaf(text)))}}));
    } catch (...) {
      result->Error("drive_invalid_path", "The folder path is not valid Unicode.");
    }
  }
};

ClientDriveChannel::ClientDriveChannel(HWND window) : state_(std::make_unique<State>(window)) {}

ClientDriveChannel::~ClientDriveChannel() {
  std::deque<State::Job> abandoned;
  {
    std::lock_guard lock(state_->mutex);
    state_->stopped = true;
    abandoned.swap(state_->jobs);
  }
  state_->changed.notify_all();
  for (auto& worker : state_->workers) {
    CancelSynchronousIo(worker.native_handle());
    if (worker.joinable()) worker.join();
  }
  Complete();
  for (auto& job : abandoned) job.result->Error("drive_unavailable", "The window closed.");
}

void ClientDriveChannel::Handle(const flutter::MethodCall<V>& call, std::unique_ptr<flutter::MethodResult<V>> result) {
  if (call.method_name() == "pickFolder") {
    state_->Pick(std::move(result));
    return;
  }
  const auto* args = std::get_if<M>(call.arguments());
  if (!args) {
    result->Error("drive_invalid_path", "Drive operation arguments required.");
    return;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || state_->jobs.size() >= 64) {
      result->Error("drive_busy", "Drive worker is busy.");
      return;
    }
    state_->Start();
    state_->jobs.push_back({call.method_name(), *args, std::move(result), {}, {}});
  }
  state_->changed.notify_one();
}

void ClientDriveChannel::Complete() {
  std::deque<State::Job> jobs;
  {
    std::lock_guard lock(state_->mutex);
    jobs.swap(state_->complete);
  }
  for (auto& job : jobs) {
    if (job.error.empty()) job.result->Success(job.value);
    else if (job.win32) job.result->Error(job.error, "Drive operation failed.", V(static_cast<int32_t>(job.win32)));
    else job.result->Error(job.error, "Drive operation failed.");
  }
}

}  // namespace multitap
