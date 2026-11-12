#include "thumbnail_writer.h"
#include "video_bounds.h"
#include <aclapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;
namespace multitap {
namespace {
std::wstring Ascii(const std::string& value) { return std::wstring(value.begin(), value.end()); }

double Milliseconds(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

// A junction or symbolic link where the writer expected a plain folder/file.
const HRESULT kLinkRefused = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);

class Owned {
 public:
  Owned() = default;
  explicit Owned(HANDLE value) : value_(value) {}
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
  Owned(Owned&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
  Owned& operator=(Owned&& other) noexcept {
    if (this != &other) { if (valid()) CloseHandle(value_); value_ = other.value_; other.value_ = INVALID_HANDLE_VALUE; }
    return *this;
  }
  ~Owned() { if (valid()) CloseHandle(value_); }
  bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
  HANDLE get() const { return value_; }
 private:
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

bool SameText(const std::wstring& a, const std::wstring& b) {
  return a.size() == b.size() &&
      (a.empty() || CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL);
}
// Where Windows says an open handle is (\\?\C:\...), or empty.
std::wstring FinalPath(HANDLE handle) {
  std::wstring buffer(512, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length) return {};
    if (length < buffer.size()) { buffer.resize(length); return buffer; }
    buffer.resize(static_cast<size_t>(length) + 1);
  }
  return {};
}
DWORD AttributesOf(HANDLE handle) {
  FILE_ATTRIBUTE_TAG_INFO info{};
  return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ? info.FileAttributes
                                                                                          : INVALID_FILE_ATTRIBUTES;
}
// The folder itself, never what a link points to. Held without delete
// sharing, so it cannot be renamed or replaced while the picture is written.
HRESULT OpenFolder(const std::wstring& path, Owned* out) {
  Owned folder(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!folder.valid()) return HRESULT_FROM_WIN32(GetLastError());
  const DWORD attributes = AttributesOf(folder.get());
  if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return kLinkRefused;
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
  *out = std::move(folder);
  return S_OK;
}
// The anchor belongs to the signed-in user: a user variable pointing
// %LOCALAPPDATA% at someone else's folder cannot make this writer create
// folders or files there.
bool OwnedByThisUser(HANDLE folder) {
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetSecurityInfo(folder, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS)
    return false;
  bool same = false;
  HANDLE token = nullptr;
  if (owner && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    if (size && GetTokenInformation(token, TokenUser, user.data(), size, &size))
      same = EqualSid(owner, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) != FALSE;
    CloseHandle(token);
  }
  LocalFree(descriptor);
  return same;
}
void DeleteHandle(HANDLE handle) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
}
// Renames exactly the open file to the full path |path|, replacing a file of
// that name (a link there is replaced as itself, never followed).
DWORD RenameHandle(HANDLE handle, const std::wstring& path) {
  std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + path.size() * sizeof(wchar_t), 0);
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  info->ReplaceIfExists = TRUE;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(path.size() * sizeof(wchar_t));
  std::memcpy(info->FileName, path.data(), path.size() * sizeof(wchar_t));
  return SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(buffer.size())) ? ERROR_SUCCESS : GetLastError();
}

// Temporary file in the same folder, then one atomic replacement, so the
// launcher only ever opens a complete previous or complete new picture.
// Every folder from the anchor down is held open while it is checked, every
// entry is opened without following a link and must be where it was meant to
// be (its final path), and the picture is renamed by handle.
HRESULT Replace(const ThumbnailWriter::Target& target, const void* bytes, size_t size,const ThumbnailWriter::Ticket* ticket,bool durable=false) {
  const auto& anchor = target.anchor;
  if (anchor.empty() || target.folder.size() <= anchor.size() + 1 || target.folder.compare(0, anchor.size(), anchor) != 0 ||
      target.folder[anchor.size()] != L'\\' || target.path.size() <= target.folder.size() + 1 ||
      target.path.compare(0, target.folder.size(), target.folder) != 0 || target.path[target.folder.size()] != L'\\')
    return E_INVALIDARG;
  std::vector<Owned> held;
  Owned base;
  HRESULT hr = OpenFolder(anchor, &base);
  if (FAILED(hr)) return hr;
  if (!OwnedByThisUser(base.get())) return E_ACCESSDENIED;
  std::wstring expected = FinalPath(base.get());
  if (expected.empty()) return E_FAIL;
  held.push_back(std::move(base));
  std::wstring current = anchor;
  for (size_t start = anchor.size() + 1; start <= target.folder.size();) {
    size_t end = target.folder.find(L'\\', start);
    if (end == std::wstring::npos) end = target.folder.size();
    const std::wstring name = target.folder.substr(start, end - start);
    if (name.empty() || name == L"." || name == L"..") return E_INVALIDARG;
    current += L"\\" + name;
    expected += L"\\" + name;
    if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
      return HRESULT_FROM_WIN32(GetLastError());
    Owned folder;
    hr = OpenFolder(current, &folder);
    if (FAILED(hr)) return hr;
    if (!SameText(FinalPath(folder.get()), expected)) return kLinkRefused;
    held.push_back(std::move(folder));
    start = end + 1;
  }
  const std::wstring leaf = target.path.substr(target.folder.size() + 1);
  const std::wstring destination = expected + L"\\" + leaf, temporary = destination + L".tmp";
  // A leftover temporary (or a link planted under its name) goes as itself.
  {
    Owned stale(CreateFileW(temporary.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (stale.valid()) DeleteHandle(stale.get());
  }
  Owned file(CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid()) return HRESULT_FROM_WIN32(GetLastError());
  if (!SameText(FinalPath(file.get()), temporary)) { DeleteHandle(file.get()); return kLinkRefused; }
  DWORD written = 0;
  if (!WriteFile(file.get(), bytes, static_cast<DWORD>(size), &written, nullptr) || written != size) {
    const DWORD error = GetLastError();
    DeleteHandle(file.get());
    return HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT);
  }
  if(durable&&!FlushFileBuffers(file.get())){const auto error=GetLastError();DeleteHandle(file.get());return HRESULT_FROM_WIN32(error);}
  for (int attempt = 0;; ++attempt) {
    if(ticket&&!ThumbnailGeneration::Current(*ticket)){DeleteHandle(file.get());return E_ABORT;}
    DWORD failure=ERROR_OPERATION_ABORTED;bool renamed=false;
    const auto rename=[&]{failure=RenameHandle(file.get(),destination);renamed=failure==ERROR_SUCCESS;return renamed;};
    const bool published=ticket?ThumbnailGeneration::Publish(*ticket,rename,[&]{DeleteHandle(file.get());}):rename();
    if(!published&&(renamed||failure==ERROR_OPERATION_ABORTED)){if(!renamed)DeleteHandle(file.get());return E_ABORT;}
    if (failure == ERROR_SUCCESS) {
      if (SameText(FinalPath(file.get()), destination)) {
        // Keep a published revocation on a failed final flush, but never
        // acknowledge it as durable. Readers still conservatively refuse it.
        if(durable&&!FlushFileBuffers(file.get()))return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
      }
      DeleteHandle(file.get());
      return kLinkRefused;
    }
    // The launcher may hold the previous picture open for a moment.
    if (attempt < 5 && (failure == ERROR_ACCESS_DENIED || failure == ERROR_SHARING_VIOLATION)) {
      Sleep(40);
      continue;
    }
    DeleteHandle(file.get());
    return HRESULT_FROM_WIN32(failure);
  }
}
struct VerifiedTargetFolder {std::vector<Owned> held;std::wstring path;};
HRESULT ExistingTargetFolder(const ThumbnailWriter::Target& target,VerifiedTargetFolder& verified){
  if(target.anchor.empty()||target.folder.size()<=target.anchor.size()+1||target.folder.compare(0,target.anchor.size(),target.anchor)!=0||target.folder[target.anchor.size()]!=L'\\'||
     target.path.size()<=target.folder.size()+1||target.path.compare(0,target.folder.size(),target.folder)!=0||target.path[target.folder.size()]!=L'\\')return E_INVALIDARG;
  Owned base;auto hr=OpenFolder(target.anchor,&base);if(FAILED(hr))return hr;
  if(!OwnedByThisUser(base.get()))return E_ACCESSDENIED;
  auto expected=FinalPath(base.get());if(expected.empty())return E_FAIL;verified.held.push_back(std::move(base));auto current=target.anchor;
  for(size_t start=target.anchor.size()+1;start<=target.folder.size();){auto end=target.folder.find(L'\\',start);if(end==std::wstring::npos)end=target.folder.size();
    const auto name=target.folder.substr(start,end-start);if(name.empty()||name==L"."||name==L"..")return E_INVALIDARG;
    current+=L"\\"+name;expected+=L"\\"+name;Owned folder;hr=OpenFolder(current,&folder);if(FAILED(hr))return hr;
    if(!SameText(FinalPath(folder.get()),expected))return kLinkRefused;verified.held.push_back(std::move(folder));start=end+1;
  }
  verified.path=expected+L"\\"+target.path.substr(target.folder.size()+1);return S_OK;
}
HRESULT PictureBelongsToEpoch(const ThumbnailWriter::Target& target,bool& belongs){
  belongs=false;VerifiedTargetFolder folder;auto hr=ExistingTargetFolder(target,folder);
  if(hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)||hr==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))return S_OK;
  if(FAILED(hr))return hr;
  Owned image(CreateFileW(folder.path.c_str(),GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!image.valid()){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?S_OK:HRESULT_FROM_WIN32(error);}
  const auto attributes=AttributesOf(image.get());if(attributes==INVALID_FILE_ATTRIBUTES)return HRESULT_FROM_WIN32(GetLastError());
  if((attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!SameText(FinalPath(image.get()),folder.path))return kLinkRefused;
  const std::string marker="Multitap:thumbnail-privacy-v1:"+target.epoch;std::vector<uint8_t> prefix(marker.size()+6);DWORD read=0;
  if(!ReadFile(image.get(),prefix.data(),DWORD(prefix.size()),&read,nullptr))return HRESULT_FROM_WIN32(GetLastError());
  belongs=read==prefix.size()&&prefix[0]==0xff&&prefix[1]==0xd8&&prefix[2]==0xff&&prefix[3]==0xfe&&
    (size_t(prefix[4])*256+prefix[5])==marker.size()+2&&std::equal(marker.begin(),marker.end(),prefix.begin()+6);
  return S_OK;
}
HRESULT PersistRevocation(const ThumbnailWriter::Target& target,const ThumbnailWriter::Ticket& ticket){
  if(!ThumbnailGeneration::Current(ticket,false))return E_ABORT;
  if(target.epoch.empty()){ticket.state->durablyRevoked=ticket.stamp;return S_OK;} // standalone legacy QA only
  if(target.epoch.size()!=32||!std::all_of(target.epoch.begin(),target.epoch.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return E_INVALIDARG;
  bool belongs=false;const auto observed=PictureBelongsToEpoch(target,belongs);if(FAILED(observed))return observed;
  if(!ThumbnailGeneration::Current(ticket,false))return E_ABORT;
  if(!belongs){
    // The serialized writer has drained earlier work and this ticket forbids
    // later old work. Do not overwrite a newer epoch's revocation marker.
    ticket.state->durablyRevoked=ticket.stamp;return S_OK;
  }
  auto marker=target;marker.path+=L".revoked";
  const auto result=Replace(marker,target.epoch.data(),target.epoch.size(),nullptr,true);
  if(SUCCEEDED(result)&&ThumbnailGeneration::Current(ticket,false))ticket.state->durablyRevoked=ticket.stamp;
  return result;
}
// A freshly published authorized picture supersedes the prior revocation. Hold every
// directory, refuse links and delete the exact opened marker, never a pathname
// that another writer could replace between a check and deletion.
HRESULT ClearRevocation(const ThumbnailWriter::Target& target,const ThumbnailWriter::Ticket* ticket){
  if(target.epoch.empty())return S_OK;
  if(ticket&&!ThumbnailGeneration::Current(*ticket))return E_ABORT;
  VerifiedTargetFolder folder;const auto hr=ExistingTargetFolder(target,folder);if(FAILED(hr))return hr;
  const auto path=folder.path+L".revoked";
  Owned marker(CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!marker.valid()){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?S_OK:HRESULT_FROM_WIN32(error);}
  const auto attributes=AttributesOf(marker.get());if(attributes==INVALID_FILE_ATTRIBUTES)return HRESULT_FROM_WIN32(GetLastError());
  if((attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!SameText(FinalPath(marker.get()),path))return kLinkRefused;
  if(ticket&&!ThumbnailGeneration::Current(*ticket))return E_ABORT;
  FILE_DISPOSITION_INFO remove{};remove.DeleteFile=TRUE;
  return SetFileInformationByHandle(marker.get(),FileDispositionInfo,&remove,sizeof(remove))?S_OK:HRESULT_FROM_WIN32(GetLastError());
}
// Existing target only, no directory creation and no links followed. Runs on
// the same writer after earlier rename/cleanup, never on the UI thread.
HRESULT Purge(const ThumbnailWriter::Target& target,const ThumbnailWriter::Ticket& ticket){
  if(target.anchor.empty()||target.folder.size()<=target.anchor.size()+1||target.folder.compare(0,target.anchor.size(),target.anchor)!=0||target.folder[target.anchor.size()]!=L'\\'||
     target.path.size()<=target.folder.size()+1||target.path.compare(0,target.folder.size(),target.folder)!=0||target.path[target.folder.size()]!=L'\\')return E_INVALIDARG;
  std::vector<Owned> held;Owned base;auto hr=OpenFolder(target.anchor,&base);if(FAILED(hr))return hr;
  if(!OwnedByThisUser(base.get()))return E_ACCESSDENIED;
  auto expected=FinalPath(base.get());if(expected.empty())return E_FAIL;held.push_back(std::move(base));auto current=target.anchor;
  for(size_t start=target.anchor.size()+1;start<=target.folder.size();){auto end=target.folder.find(L'\\',start);if(end==std::wstring::npos)end=target.folder.size();
    const auto name=target.folder.substr(start,end-start);if(name.empty()||name==L"."||name==L"..")return E_INVALIDARG;
    current+=L"\\"+name;expected+=L"\\"+name;Owned folder;hr=OpenFolder(current,&folder);if(FAILED(hr))return hr;
    if(!SameText(FinalPath(folder.get()),expected))return kLinkRefused;held.push_back(std::move(folder));start=end+1;
  }
  const auto path=expected+L"\\"+target.path.substr(target.folder.size()+1);
  for(const auto& leaf:{path,path+L".tmp"}){
    if(!ThumbnailGeneration::Current(ticket,false))return E_ABORT;
    Owned file(CreateFileW(leaf.c_str(),DELETE|GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file.valid()){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)continue;return HRESULT_FROM_WIN32(error);}
    const auto attributes=AttributesOf(file.get());if(attributes==INVALID_FILE_ATTRIBUTES)return HRESULT_FROM_WIN32(GetLastError());
    if(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))return kLinkRefused;
    if(!SameText(FinalPath(file.get()),leaf))return kLinkRefused;
    if(!target.epoch.empty()){
      const std::string marker="Multitap:thumbnail-privacy-v1:"+target.epoch;
      std::vector<uint8_t> prefix(marker.size()+6);DWORD read=0;
      if(!ReadFile(file.get(),prefix.data(),DWORD(prefix.size()),&read,nullptr))return HRESULT_FROM_WIN32(GetLastError());
      // A newer/foreign picture at the same name is never this purge's file.
      if(read!=prefix.size()||prefix[0]!=0xff||prefix[1]!=0xd8||prefix[2]!=0xff||prefix[3]!=0xfe||
         (size_t(prefix[4])*256+prefix[5])!=marker.size()+2||!std::equal(marker.begin(),marker.end(),prefix.begin()+6))continue;
    }
    if(!ThumbnailGeneration::Current(ticket,false))return E_ABORT;
    FILE_DISPOSITION_INFO remove{};remove.DeleteFile=TRUE;if(!SetFileInformationByHandle(file.get(),FileDispositionInfo,&remove,sizeof(remove)))return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}
}  // namespace

std::optional<ThumbnailWriter::Target> ThumbnailWriter::Resolve(const std::string& key, const std::string& desktop,
                                                                const std::wstring& root,const std::string& epoch) {
  const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (key.size() != 16 || !std::all_of(key.begin(), key.end(), hex)) return std::nullopt;
  if(!epoch.empty()&&(epoch.size()!=32||!std::all_of(epoch.begin(),epoch.end(),hex)))return std::nullopt;
  const bool vm = desktop.size() >= 5 && desktop.size() <= 16 && desktop.compare(0, 4, "pve-") == 0 &&
                  std::all_of(desktop.begin() + 4, desktop.end(), digit);
  // A local PC: pc-<lowercase UUID>.
  bool pc = desktop.size() == 39 && desktop.compare(0, 3, "pc-") == 0;
  for (size_t i = 3; pc && i < desktop.size(); ++i)
    pc = (i == 11 || i == 16 || i == 21 || i == 26) ? desktop[i] == '-' : hex(desktop[i]);
  if (!vm && !pc) return std::nullopt;
  std::wstring base = root, anchor;
  if (base.empty()) {
    // The launcher reads the same variable, so both agree on the folder.
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!length || length > 32767) return std::nullopt;
    std::wstring local(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), length);
    if (!copied || copied >= length) return std::nullopt;
    local.resize(copied);
    while (!local.empty() && (local.back() == L'\\' || local.back() == L'/')) local.pop_back();
    const bool drive = local.size() >= 3 && local[1] == L':' && (local[2] == L'\\' || local[2] == L'/');
    const bool share = local.size() > 2 && local[0] == L'\\' && local[1] == L'\\';
    if (!drive && !share) return std::nullopt;
    for (auto& ch : local) if (ch == L'/') ch = L'\\';
    anchor = local;
    base = local + L"\\Multitap\\thumbnails";
  }
  while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
  if (base.empty()) return std::nullopt;
  if (anchor.empty()) anchor = base;
  Target target;
  target.key = key;
  target.desktop = desktop;
  target.epoch = epoch;
  target.anchor = anchor;
  target.folder = base + L"\\" + Ascii(key);
  target.path = target.folder + L"\\" + Ascii(desktop) + L".jpg";
  return target;
}

bool ThumbnailWriter::Scale(const DecodedFrame& frame, uint32_t& width, uint32_t& height, uint32_t& stride,
                            std::vector<uint8_t>& bgr) {
  const uint32_t w = frame.width, h = frame.height;
  video::FrameLayout layout{};
  if (!video::frameLayout(w,h,layout,false)||frame.rgba.size()!=layout.rgbaBytes) return false;
  const double scale = std::min({static_cast<double>(kMaxWidth) / w, static_cast<double>(kMaxHeight) / h, 1.0});
  width = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(w * scale)), 1, w);
  height = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(h * scale)), 1, h);
  stride = (width * 3 + 3) & ~3u;
  bgr.assign(static_cast<size_t>(stride) * height, 0);
  // Each output pixel averages exactly its own source box (every source pixel
  // is read once), so text and edges do not alias as a nearest pick would.
  std::vector<uint32_t> columns(width + 1);
  for (uint32_t x = 0; x <= width; ++x) columns[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * w / width);
  std::vector<uint32_t> sums(static_cast<size_t>(width) * 3);
  const uint8_t* source = frame.rgba.data();
  for (uint32_t ty = 0; ty < height; ++ty) {
    const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(ty) * h / height);
    const uint32_t y1 = static_cast<uint32_t>(static_cast<uint64_t>(ty + 1) * h / height);
    std::fill(sums.begin(), sums.end(), 0u);
    for (uint32_t y = y0; y < y1; ++y) {
      const uint8_t* row = source + static_cast<size_t>(y) * w * 4;
      for (uint32_t tx = 0; tx < width; ++tx) {
        uint32_t r = 0, g = 0, b = 0;
        for (uint32_t x = columns[tx], end = columns[tx + 1]; x < end; ++x) {
          const uint8_t* pixel = row + static_cast<size_t>(x) * 4;
          r += pixel[0]; g += pixel[1]; b += pixel[2];
        }
        uint32_t* sum = sums.data() + static_cast<size_t>(tx) * 3;
        sum[0] += r; sum[1] += g; sum[2] += b;
      }
    }
    uint8_t* out = bgr.data() + static_cast<size_t>(ty) * stride;
    for (uint32_t tx = 0; tx < width; ++tx) {
      const uint32_t count = (y1 - y0) * (columns[tx + 1] - columns[tx]);
      const uint32_t* sum = sums.data() + static_cast<size_t>(tx) * 3;
      out[tx * 3 + 0] = static_cast<uint8_t>((sum[2] + count / 2) / count);
      out[tx * 3 + 1] = static_cast<uint8_t>((sum[1] + count / 2) / count);
      out[tx * 3 + 2] = static_cast<uint8_t>((sum[0] + count / 2) / count);
    }
  }
  return true;
}

HRESULT ThumbnailWriter::Write(const Target& target, const DecodedFrame& frame, IWICImagingFactory* factory,
                               Timing* timing,const Ticket* ticket) {
  if(ticket&&!ThumbnailGeneration::Current(*ticket))return E_ABORT;
  if (!factory || target.folder.empty() || target.path.empty()) return E_INVALIDARG;
  if(!target.epoch.empty()&&(target.epoch.size()!=32||!std::all_of(target.epoch.begin(),target.epoch.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})))return E_INVALIDARG;
  using Clock = std::chrono::steady_clock;
  const auto scaling = Clock::now();
  uint32_t width = 0, height = 0, stride = 0;
  std::vector<uint8_t> bgr;
  if (!Scale(frame, width, height, stride, bgr)) return E_INVALIDARG;
  const auto encoding = Clock::now();
  ComPtr<IStream> stream;
  HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  ComPtr<IWICBitmapEncoder> encoder;
  if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> picture;
  ComPtr<IPropertyBag2> options;
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&picture, &options);
  if (SUCCEEDED(hr)) {
    PROPBAG2 name{};
    name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT quality;
    VariantInit(&quality);
    quality.vt = VT_R4;
    quality.fltVal = 0.8f;
    hr = options->Write(1, &name, &quality);
  }
  if (SUCCEEDED(hr)) hr = picture->Initialize(options.Get());
  if (SUCCEEDED(hr)) hr = picture->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (SUCCEEDED(hr)) hr = picture->SetPixelFormat(&format);
  if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
  if (SUCCEEDED(hr)) hr = picture->WritePixels(height, stride, static_cast<UINT>(bgr.size()), bgr.data());
  if (SUCCEEDED(hr)) hr = picture->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  STATSTG stat{};
  if (SUCCEEDED(hr)) hr = stream->Stat(&stat, STATFLAG_NONAME);
  HGLOBAL memory = nullptr;
  if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.Get(), &memory);
  if (FAILED(hr)) return hr;
  const size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
  if (!size || size > 4 * 1024 * 1024 || size > GlobalSize(memory)) return E_UNEXPECTED;
  const auto writing = Clock::now();
  const void* bytes = GlobalLock(memory);
  if (!bytes) return HRESULT_FROM_WIN32(GetLastError());
  struct Unlock {HGLOBAL value;~Unlock(){GlobalUnlock(value);}} unlock{memory};
  // Bind the persisted JPEG itself to the authenticated Host privacy epoch.
  // One atomic image replacement carries both pixels and provenance: there is
  // no sidecar whose update could accidentally authorize an older picture.
  std::vector<uint8_t> stamped;
  const void* output=bytes;size_t outputSize=size;
  if(!target.epoch.empty()){
    const auto* jpeg=static_cast<const uint8_t*>(bytes);
    const std::string marker="Multitap:thumbnail-privacy-v1:"+target.epoch;
    if(size<4||jpeg[0]!=0xff||jpeg[1]!=0xd8||marker.size()+4>4*1024*1024-size)return E_UNEXPECTED;
    const auto length=uint16_t(marker.size()+2);
    stamped.reserve(size+marker.size()+4);
    stamped.insert(stamped.end(),jpeg,jpeg+2);
    stamped.insert(stamped.end(),{0xff,0xfe,uint8_t(length>>8),uint8_t(length)});
    stamped.insert(stamped.end(),marker.begin(),marker.end());stamped.insert(stamped.end(),jpeg+2,jpeg+size);
    output=stamped.data();outputSize=stamped.size();
  }
  hr = Replace(target, output, outputSize,ticket);
  if(SUCCEEDED(hr))hr=ClearRevocation(target,ticket);
  if (timing) {
    const auto done = Clock::now();
    timing->scaleMs = Milliseconds(scaling, encoding);
    timing->encodeMs = Milliseconds(encoding, writing);
    timing->writeMs = Milliseconds(writing, done);
    timing->width = width;
    timing->height = height;
    timing->bytes = outputSize;
  }
  return hr;
}

ThumbnailWriter::~ThumbnailWriter() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    notify_ = nullptr;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

ThumbnailWriter::Ticket ThumbnailWriter::Own(const Target& target){
  try{
  std::lock_guard lock(mutex_);if(stopping_||target.path.empty())return {};
  for(auto it=targets_.begin();it!=targets_.end();)if(it->second.state.expired())it=targets_.erase(it);else ++it;
  auto found=targets_.find(target.path);if(found==targets_.end()&&targets_.size()>=kTargetLimit)return {};
  auto ticket=ThumbnailGeneration::Own();if(!ThumbnailGeneration::Current(ticket))return {};
  if(found!=targets_.end())ThumbnailGeneration::Retire(found->second.state.lock());
  targets_[target.path]={ticket.state,target.epoch};return ticket;
  }catch(...){return {};}
}
bool ThumbnailWriter::Invalidate(const Target& target,Ticket& ticket){
  auto revoked=ThumbnailGeneration::Change(ticket,false);if(!revoked.state)return false;ticket=revoked;
  HWND notify=nullptr;std::deque<Job> discarded;
  bool accepted=true;try{
    std::lock_guard lock(mutex_);
    notify=notify_;
    for(auto it=queue_.begin();it!=queue_.end();)if(it->target.path==target.path){discarded.push_back(std::move(*it));it=queue_.erase(it);}else ++it;
    completed_.erase(std::remove_if(completed_.begin(),completed_.end(),[&](const auto& old){return old.key==target.key&&old.desktop==target.desktop;}),completed_.end());
    // One purge/notice per live target (bounded by kTargetLimit); privacy jobs
    // are never evicted to make room for ordinary picture refreshes.
    queue_.push_back({target,{},revoked,true});completed_.push_back({target.key,target.desktop,target.epoch,revoked,true});
    if(!worker_.joinable())worker_=std::thread([this]{Run();});
  }catch(...){accepted=false;}
  wake_.notify_one();if(notify)PostMessageW(notify,completed_message,0,0);return accepted;
}
bool ThumbnailWriter::InvalidateTarget(const Target& target){
  Ticket ticket;try{
    std::lock_guard lock(mutex_);const auto found=targets_.find(target.path);
    if(stopping_)return false;
    if(found!=targets_.end()&&found->second.epoch!=target.epoch)return true;
    auto state=found==targets_.end()?nullptr:found->second.state.lock();
    if(state)ticket={state,state->stamp.load()};
    else{
      for(auto it=targets_.begin();it!=targets_.end();)if(it->second.state.expired())it=targets_.erase(it);else ++it;
      if(targets_.size()>=kTargetLimit)return false;
      ticket=ThumbnailGeneration::Own();targets_[target.path]={ticket.state,target.epoch};
    }
  }catch(...){return false;}
  if(!ticket.state||!ticket.stamp)return true;
  return Invalidate(target,ticket);
}
bool ThumbnailWriter::Submit(const Target& target,std::shared_ptr<const DecodedFrame> frame){
  // Legacy standalone callers. Production always uses the strict ticket
  // overload: a missing/retired production ticket must never adopt a new one.
  Ticket ticket;{
    std::lock_guard lock(mutex_);const auto found=targets_.find(target.path);
    const auto state=found==targets_.end()?nullptr:found->second.state.lock();if(state)ticket={state,state->stamp.load()};
  }
  if(!ticket.state)ticket=Own(target);
  return Submit(target,std::move(frame),std::move(ticket));
}
bool ThumbnailWriter::Submit(const Target& target, std::shared_ptr<const DecodedFrame> frame,Ticket ticket) {
  if (!frame || target.path.empty()) return false;
  // Declared before the lock so a replaced reference is released after it.
  std::shared_ptr<const DecodedFrame> replaced;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return false;
  if(!ThumbnailGeneration::Current(ticket))return false;
  try {
    for (auto& job : queue_) {
      if (job.purge||job.target.path != target.path||job.ticket.stamp!=ticket.stamp) continue;
      replaced = std::exchange(job.frame, std::move(frame));
      return true;
    }
    if (queue_.size() >= kQueueLimit) {
      const auto old=std::find_if(queue_.begin(),queue_.end(),[](const auto& job){return !job.purge;});
      if(old==queue_.end())return false;replaced=std::move(old->frame);queue_.erase(old);
    }
    queue_.push_back({target, std::move(frame),std::move(ticket),false});
    if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
  } catch (...) {
    if (!worker_.joinable()) queue_.clear();
    return false;
  }
  wake_.notify_one();
  return true;
}

bool ThumbnailWriter::Idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.empty() && !working_;
}

std::vector<ThumbnailWriter::Notice> ThumbnailWriter::TakeNotices() {
  std::lock_guard<std::mutex> lock(mutex_);
  completed_.erase(std::remove_if(completed_.begin(),completed_.end(),[](const auto& notice){return !notice.invalidated&&!ThumbnailGeneration::Current(notice.ticket);}),completed_.end());
  return std::exchange(completed_, {});
}
std::vector<std::pair<std::string,std::string>> ThumbnailWriter::TakeCompleted(){
  std::vector<std::pair<std::string,std::string>> result;
  for(auto& notice:TakeNotices())if(!notice.invalidated)result.emplace_back(std::move(notice.key),std::move(notice.desktop));
  return result;
}

void ThumbnailWriter::Detach() {
  std::lock_guard<std::mutex> lock(mutex_);
  notify_ = nullptr;
}

void ThumbnailWriter::Run() noexcept {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    ComPtr<IWICImagingFactory> factory;
    if (SUCCEEDED(com))
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) break;  // Stopping, and every queued picture is done.
        job = std::move(queue_.front());
        queue_.pop_front();
        working_ = true;
      }
      HRESULT hr = E_FAIL;
      try {
        if(job.purge){
          if(ThumbnailGeneration::Current(job.ticket,false)){
            hr=PersistRevocation(job.target,job.ticket);
            if(SUCCEEDED(hr))hr=Purge(job.target,job.ticket);
          }
        }
        else if (factory) hr = Write(job.target, *job.frame, factory.Get(),nullptr,&job.ticket);
      } catch (...) {
        hr = E_FAIL;
      }
      // Hand the engine's pixels back before anything else.
      job.frame.reset();
      HWND notify = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        working_ = false;
        if (!job.purge&&SUCCEEDED(hr)&&ThumbnailGeneration::Current(job.ticket)) {
          try {
            completed_.erase(std::remove_if(completed_.begin(),completed_.end(),[&](const auto& old){return old.key==job.target.key&&old.desktop==job.target.desktop;}),completed_.end());
            if(completed_.size()<kCompletedLimit)completed_.push_back({job.target.key,job.target.desktop,job.target.epoch,job.ticket,false});
            notify = notify_;
          } catch (...) {
          }
        }
      }
      if (notify) PostMessageW(notify, completed_message, 0, 0);
    }
  }
  if (SUCCEEDED(com)) CoUninitialize();
}
}  // namespace multitap
