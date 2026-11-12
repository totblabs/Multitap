#pragma once
#include "clipboard.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace multitap::clipboard {
// The caller only constructs this store for an explicit file transfer. It never
// observes the clipboard on a timer or follows a path supplied by the peer.
class Storage {
 public:
  static constexpr uint64_t quotaBytes = 16ull * 1024 * 1024 * 1024;
  static constexpr uint64_t retentionMillis = 24ull * 60 * 60 * 1000;
  explicit Storage(std::filesystem::path parent) : parent_(std::move(parent)) {
    demand(parent_.is_absolute(), "Clipboard storage path must be absolute");
    HANDLE rawToken = nullptr;
    winCheck(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken), "Clipboard storage identity");
    Handle token(rawToken); DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    demand(size > 0 && size < 65536, "Clipboard storage identity is unavailable");
    user_.resize(size);
    winCheck(GetTokenInformation(token.value, TokenUser, user_.data(), size, &size), "Clipboard storage identity");
    LPWSTR sid = nullptr;
    winCheck(ConvertSidToStringSidW(userSid(), &sid), "Clipboard storage SID");
    // An elevated Host worker also labels the store High (no write up): the account's
    // ordinary programs keep reading what was received but cannot change it.
    labelled_ = multitap::clipboard::processAboveMedium();
    const auto sddl = L"O:" + std::wstring(sid) + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + sid + L")" + (labelled_ ? L"S:(ML;OICI;NW;;;HI)" : L"");
    LocalFree(sid);
    winCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security_, nullptr), "Clipboard storage ACL");
    try {
      auto current = parent_.root_path();
      for (const auto& part : parent_.relative_path()) {
        demand(part != L"." && part != L"..", "Clipboard storage path is invalid");
        current /= part;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), security_, FALSE};
        if (!CreateDirectoryW(multitap::clipboard::longPath(current).c_str(), current == parent_ ? &attributes : nullptr))
          demand(GetLastError() == ERROR_ALREADY_EXISTS, "Clipboard storage directory cannot be created");
        Handle folder(CreateFileW(multitap::clipboard::longPath(current).c_str(), FILE_READ_ATTRIBUTES | (current == parent_ ? READ_CONTROL | WRITE_DAC | (labelled_ ? WRITE_OWNER : 0) : 0),
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        demand(folder.value != INVALID_HANDLE_VALUE, "Clipboard storage directory is unavailable");
        FILE_ATTRIBUTE_TAG_INFO info{};
        winCheck(GetFileInformationByHandleEx(folder.value, FileAttributeTagInfo, &info, sizeof(info)), "Clipboard storage attributes");
        demand((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Clipboard storage directory is untrusted");
        if (current == parent_) {
          demand(trusted(folder.value), "Clipboard storage owner is untrusted");
          PACL dacl = nullptr; BOOL present = FALSE, ignored = FALSE;
          winCheck(GetSecurityDescriptorDacl(security_, &present, &dacl, &ignored), "Clipboard storage ACL");
          demand(present && dacl && SetSecurityInfo(folder.value, SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS,
              "Clipboard storage ACL cannot be restricted");
          if (labelled_) {
            PACL sacl = nullptr; BOOL saclPresent = FALSE, saclDefaulted = FALSE;
            winCheck(GetSecurityDescriptorSacl(security_, &saclPresent, &sacl, &saclDefaulted), "Clipboard storage label");
            demand(saclPresent && sacl && SetSecurityInfo(folder.value, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl) == ERROR_SUCCESS,
                "Clipboard storage label cannot be set");
          }
          demand(trusted(folder.value, true), "Clipboard storage ACL is untrusted");
        }
        // Retain directory handles without FILE_SHARE_DELETE for the duration
        // of the operation; ancestor rename/reparse replacement is refused.
        directories_.push_back(folder.value); folder.value = INVALID_HANDLE_VALUE;
      }
    } catch (...) { close(); throw; }
  }
  ~Storage() { close(); }
  Storage(const Storage&) = delete;
  // Removes `path` and what is inside it without following any reparse point (a link or junction is removed as itself).
  // Bounded depth; true only when the path is gone.
  // Every item is opened relative to its folder's handle (clipboard.cpp), so a link put in between is never followed.
  static bool removeWithoutFollowing(const std::filesystem::path& path, int depth) noexcept {
    if (depth > 600) return false;
    try { return ::multitap::clipboard::removeWithoutFollowing(path.parent_path(), path.filename().wstring()); } catch (...) { return false; }
  }
  const std::filesystem::path& parent() const { return parent_; }
  void prepare(uint64_t incomingBytes, const std::vector<std::filesystem::path>& currentClipboardFiles) {
    demand(incomingBytes <= multitap::clipboard::scaleBytes, "clipboard_total_size_limit");
    const auto now = nowMillis();
    size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(multitap::clipboard::longPath(parent_))) {
      demand(++entries <= 4096, "Clipboard storage contains too many entries");
      const auto filename = entry.path().filename().wstring();
      if (filename.size() != 46 || filename.rfind(L"completed-", 0) || filename.substr(42) != L".bin") continue;
      const auto id = filename.substr(10, 32);
      if (!hexId(id)) continue;
      try {
        const auto receipt = readReceipt(entry.path(), id);
        if (now < receipt.created || now - receipt.created < retentionMillis) continue;
        const auto root = parent_ / (L"transfer-" + id);
        if (std::any_of(currentClipboardFiles.begin(), currentClipboardFiles.end(), [&](const auto& path) { return path.parent_path() == root; })) continue;
        // A received tree served as virtual files from its transfer folder is still on the clipboard.
        if (multitap::clipboard::localTreeHolds(root)) continue;
        // expireFiles removes only manifest-listed regular files and an empty
        // exact child directory. Unknown names, directories and links survive.
        // A folder tree is removed the same way: listed files, then listed folders.
        const bool expired = receipt.tree ? multitap::clipboard::expireTree(parent_, root, receipt.nodes)
                                          : multitap::clipboard::expireFiles(parent_, receipt.files);
        if (expired) removeReceipt(entry.path());
      } catch (...) { /* An invalid or busy receipt is retained, never trusted. */ }
    }
    sweepUnfinished(now, currentClipboardFiles);
    const auto retained = retainedBytes();
    demand(retained <= quotaBytes && incomingBytes <= quotaBytes - retained, "clipboard_disk_full");
  }
  // A transfer-<id> folder with no receipt is what a transfer left that never finished: a crash or power loss skipped its
  // discard(). It was never on the clipboard as a completed copy, yet it would count against the quota for ever, so after enough
  // crashes every file transfer would fail as clipboard_disk_full. After the retention period it is removed:
  // regular files and folders inside it; a reparse point (link, junction) is removed as the link itself, never followed.
  // A folder the clipboard still refers to, or that cannot be read, is kept.
  size_t sweepUnfinished(uint64_t now, const std::vector<std::filesystem::path>& currentClipboardFiles) noexcept {
    size_t removed = 0;
    try {
      std::vector<std::filesystem::path> unfinished;
      for (const auto& entry : std::filesystem::directory_iterator(multitap::clipboard::longPath(parent_))) {
        const auto name = entry.path().filename().wstring();
        if (name.size() != 41 || name.rfind(L"transfer-", 0) != 0 || !hexId(name.substr(9))) continue;
        const auto id = name.substr(9);
        if (GetFileAttributesW(multitap::clipboard::longPath(parent_ / (L"completed-" + id + L".bin")).c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const auto root = parent_ / name;
        if (!GetFileAttributesExW(multitap::clipboard::longPath(root).c_str(), GetFileExInfoStandard, &data) ||
            !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        // nowMillis() counts FILETIME milliseconds (since 1601), as this does.
        const uint64_t writtenMillis = ((uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime) / 10000;
        if (now < writtenMillis || now - writtenMillis < retentionMillis) continue;
        if (std::any_of(currentClipboardFiles.begin(), currentClipboardFiles.end(), [&](const auto& path) { return path.parent_path() == root; })) continue;
        if (multitap::clipboard::localTreeHolds(root)) continue;
        unfinished.push_back(root);
      }
      for (const auto& root : unfinished) if (removeWithoutFollowing(root, 0)) ++removed;
    } catch (...) { /* Kept as it was; the next prepare tries again. */ }
    return removed;
  }
  void record(const std::vector<std::filesystem::path>& paths, const std::vector<uint64_t>& sizes) {
    demand(!paths.empty() && paths.size() == sizes.size() && paths.size() <= multitap::clipboard::maxFiles, "Clipboard receipt is invalid");
    const auto root = paths.front().parent_path(); const auto name = root.filename().wstring();
    demand(root.parent_path() == parent_ && name.size() == 41 && name.rfind(L"transfer-", 0) == 0 && hexId(name.substr(9)), "Clipboard receipt path is invalid");
    std::vector<uint8_t> receipt(magic.begin(), magic.end());
    appendNumber(receipt, nowMillis(), 8); appendNumber(receipt, paths.size(), 4);
    uint64_t total = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
      demand(paths[i].parent_path() == root && multitap::clipboard::safeLeaf(paths[i].filename().wstring()), "Clipboard receipt path is invalid");
      demand(sizes[i] <= multitap::clipboard::scaleBytes && sizes[i] <= multitap::clipboard::scaleBytes - total, "Clipboard receipt size is invalid"); total += sizes[i];
      const auto leaf = paths[i].filename().wstring(); appendNumber(receipt, leaf.size(), 4);
      for (wchar_t unit : leaf) appendNumber(receipt, uint16_t(unit), 2);
      appendNumber(receipt, sizes[i], 8);
    }
    demand(receipt.size() <= 65536, "Clipboard receipt exceeds bounds");
    const auto path = parent_ / (L"completed-" + name.substr(9) + L".bin");
    Handle file(CreateFileW(multitap::clipboard::longPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    demand(file.value != INVALID_HANDLE_VALUE, "Clipboard receipt cannot be created");
    DWORD count = 0;
    if (!WriteFile(file.value, receipt.data(), DWORD(receipt.size()), &count, nullptr) || count != receipt.size() || !FlushFileBuffers(file.value)) {
      file.close(); removeReceipt(path); throw std::runtime_error("Clipboard receipt cannot be saved");
    }
  }
  // A received folder tree: every entry is listed so that expiry removes exactly what was received.
  void recordTree(const std::filesystem::path& root, const std::vector<TreeEntry>& nodes) {
    const auto name = root.filename().wstring();
    demand(root.parent_path() == parent_ && name.size() == 41 && name.rfind(L"transfer-", 0) == 0 && hexId(name.substr(9)), "Clipboard receipt path is invalid");
    demand(!nodes.empty() && nodes.size() <= multitap::clipboard::maxEntries, "Clipboard receipt is invalid");
    std::vector<uint8_t> receipt(treeMagic.begin(), treeMagic.end());
    appendNumber(receipt, nowMillis(), 8); appendNumber(receipt, nodes.size(), 4);
    uint64_t total = 0;
    for (const auto& node : nodes) {
      demand(!node.relative.empty() && node.relative.size() <= multitap::clipboard::maxPathChars, "Clipboard receipt path is invalid");
      demand(node.size <= multitap::clipboard::scaleBytes && node.size <= multitap::clipboard::scaleBytes - total, "Clipboard receipt size is invalid"); total += node.size;
      receipt.push_back(node.directory ? 1 : 0); appendNumber(receipt, node.relative.size(), 4);
      for (wchar_t unit : node.relative) appendNumber(receipt, uint16_t(unit), 2);
      appendNumber(receipt, node.size, 8);
    }
    demand(receipt.size() <= maxTreeReceiptBytes, "Clipboard receipt exceeds bounds");
    const auto path = parent_ / (L"completed-" + name.substr(9) + L".bin");
    Handle file(CreateFileW(multitap::clipboard::longPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    demand(file.value != INVALID_HANDLE_VALUE, "Clipboard receipt cannot be created");
    DWORD count = 0;
    if (!WriteFile(file.value, receipt.data(), DWORD(receipt.size()), &count, nullptr) || count != receipt.size() || !FlushFileBuffers(file.value)) {
      file.close(); removeReceipt(path); throw std::runtime_error("Clipboard receipt cannot be saved");
    }
  }
  void forget(const std::vector<std::filesystem::path>& paths) noexcept {
    if (paths.empty()) return;
    const auto root = paths.front().parent_path(); const auto name = root.filename().wstring();
    if (root.parent_path() == parent_ && name.size() == 41 && name.rfind(L"transfer-", 0) == 0 && hexId(name.substr(9)))
      removeReceipt(parent_ / (L"completed-" + name.substr(9) + L".bin"));
  }
 private:
  struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value) : value(value) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
  };
  struct Receipt { uint64_t created = 0; std::vector<std::filesystem::path> files; bool tree = false; std::vector<TreeEntry> nodes; };
  static constexpr std::array<uint8_t, 24> magic{'T','o','T','B','V','D','I','.','C','L','I','P','.','F','I','L','E','S','.','v','1',0,0,0};
  static constexpr std::array<uint8_t, 24> treeMagic{'T','o','T','B','V','D','I','.','C','L','I','P','.','T','R','E','E','.','v','1',0,0,0,0};
  static constexpr size_t maxTreeReceiptBytes = 4 * 1024 * 1024;
  std::filesystem::path parent_; std::vector<BYTE> user_; std::vector<HANDLE> directories_; PSECURITY_DESCRIPTOR security_ = nullptr; bool labelled_ = false;
  static void demand(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
  static void winCheck(BOOL value, const char* reason) { if (!value) throw std::runtime_error(std::string(reason) + " (Windows " + std::to_string(GetLastError()) + ")"); }
  static void appendNumber(std::vector<uint8_t>& bytes, uint64_t value, size_t length) { for (size_t index = 0; index < length; ++index) { bytes.push_back(uint8_t(value)); value >>= 8; } }
  static uint64_t readNumber(const std::vector<uint8_t>& bytes, size_t& offset, size_t length) { demand(offset <= bytes.size() && length <= bytes.size() - offset, "Clipboard receipt is truncated"); uint64_t value = 0; for (size_t index = 0; index < length; ++index) value |= uint64_t(bytes[offset++]) << (index * 8); return value; }
  PSID userSid() const { return reinterpret_cast<const TOKEN_USER*>(user_.data())->User.Sid; }
  static bool hexId(const std::wstring& id) { return id.size() == 32 && id.find_first_not_of(L"0123456789abcdef") == std::wstring::npos; }
  static uint64_t nowMillis() { FILETIME time{}; GetSystemTimeAsFileTime(&time); ULARGE_INTEGER value{}; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime; return value.QuadPart / 10000; }
  void close() { for (const auto handle : directories_) CloseHandle(handle); directories_.clear(); if (security_) LocalFree(security_); security_ = nullptr; }
  bool trusted(HANDLE file, bool restricted = false) const {
    PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    const auto allowed = [&](PSID sid) { return EqualSid(sid, userSid()) || IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid); };
    bool safe = owner && allowed(owner) && dacl;
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    if (restricted) safe = safe && GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED);
    if (safe && restricted) for (DWORD i = 0; i < dacl->AceCount; ++i) {
      void* raw = nullptr; if (!GetAce(dacl, i, &raw)) { safe = false; break; }
      const auto* header = static_cast<ACE_HEADER*>(raw);
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { safe = false; break; }
      const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
      if (!allowed(const_cast<DWORD*>(&ace->SidStart))) { safe = false; break; }
    }
    LocalFree(descriptor); return safe;
  }
  Receipt readReceipt(const std::filesystem::path& path, const std::wstring& id) const {
    Handle file(CreateFileW(multitap::clipboard::longPath(path).c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    demand(file.value != INVALID_HANDLE_VALUE && trusted(file.value), "Clipboard receipt is untrusted");
    FILE_ATTRIBUTE_TAG_INFO info{}; LARGE_INTEGER size{};
    demand(GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &info, sizeof(info)) && !(info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        GetFileSizeEx(file.value, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(maxTreeReceiptBytes), "Clipboard receipt is invalid");
    std::vector<uint8_t> bytes(size_t(size.QuadPart)); DWORD count = 0;
    demand(ReadFile(file.value, bytes.data(), DWORD(bytes.size()), &count, nullptr) && count == bytes.size(), "Clipboard receipt cannot be read");
    if (bytes.size() >= treeMagic.size() && std::equal(treeMagic.begin(), treeMagic.end(), bytes.begin())) return readTreeReceipt(bytes, id);
    demand(bytes.size() <= 65536 && bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin()), "Clipboard receipt is invalid");
    size_t offset = magic.size(); Receipt result; result.created = readNumber(bytes, offset, 8); const auto items = readNumber(bytes, offset, 4);
    demand(result.created > 0 && items > 0 && items <= maxFiles, "Clipboard receipt is invalid");
    std::set<std::wstring> names; uint64_t total = 0;
    for (uint64_t i = 0; i < items; ++i) {
      const auto units = readNumber(bytes, offset, 4); demand(units > 0 && units <= 240, "Clipboard receipt name is invalid");
      std::wstring name; name.reserve(size_t(units)); for (uint64_t unit = 0; unit < units; ++unit) name += wchar_t(readNumber(bytes, offset, 2));
      auto folded = name; CharUpperBuffW(folded.data(), DWORD(folded.size())); const auto length = readNumber(bytes, offset, 8);
      demand(safeLeaf(name) && names.insert(folded).second && length <= multitap::clipboard::scaleBytes && length <= multitap::clipboard::scaleBytes - total, "Clipboard receipt item is invalid");
      total += length; result.files.push_back(parent_ / (L"transfer-" + id) / name);
    }
    demand(offset == bytes.size(), "Clipboard receipt contains trailing data");
    return result;
  }
  Receipt readTreeReceipt(const std::vector<uint8_t>& bytes, const std::wstring& id) const {
    size_t offset = treeMagic.size(); Receipt result; result.tree = true; result.created = readNumber(bytes, offset, 8);
    const auto items = readNumber(bytes, offset, 4);
    demand(result.created > 0 && items > 0 && items <= multitap::clipboard::maxEntries, "Clipboard receipt is invalid");
    std::set<std::wstring> names; uint64_t total = 0;
    for (uint64_t i = 0; i < items; ++i) {
      const auto kind = readNumber(bytes, offset, 1); demand(kind <= 1, "Clipboard receipt item is invalid");
      const auto units = readNumber(bytes, offset, 4); demand(units > 0 && units <= multitap::clipboard::maxPathChars, "Clipboard receipt name is invalid");
      std::wstring name; name.reserve(size_t(units)); for (uint64_t unit = 0; unit < units; ++unit) name += wchar_t(readNumber(bytes, offset, 2));
      const auto length = readNumber(bytes, offset, 8);
      // Every component must still be a plain leaf; the receipt is trusted only that far.
      std::wstring wire = name; std::replace(wire.begin(), wire.end(), L'\\', L'/');
      std::wstring checked; try { checked = multitap::clipboard::treePathFromWire(wire); } catch (...) { checked.clear(); }
      auto folded = name; CharUpperBuffW(folded.data(), DWORD(folded.size()));
      demand(checked == name && names.insert(folded).second && length <= multitap::clipboard::scaleBytes && length <= multitap::clipboard::scaleBytes - total && (kind == 0 || length == 0), "Clipboard receipt item is invalid");
      total += length;
      TreeEntry node; node.relative = name; node.directory = kind == 1; node.size = length; result.nodes.push_back(std::move(node));
    }
    demand(offset == bytes.size(), "Clipboard receipt contains trailing data");
    (void)id; return result;
  }
  // Counts every byte under the storage parent. Transfer folders may hold nested folders (trees) down to
  // maxDepth; a link anywhere, or an entry that cannot be inspected, makes the store untrusted as before.
  void sumDirectory(const std::filesystem::path& folder, size_t depth, uint64_t& bytes, size_t& count) const {
    demand(depth <= multitap::clipboard::maxDepth + 2, "Clipboard storage is nested too deeply");
    for (const auto& entry : std::filesystem::directory_iterator(multitap::clipboard::longPath(folder))) {
      demand(++count <= 250000, "Clipboard storage contains too many entries");
      Handle file(CreateFileW(multitap::clipboard::longPath(entry.path()).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      FILE_ATTRIBUTE_TAG_INFO info{}; LARGE_INTEGER size{};
      demand(file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &info, sizeof(info)) && !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Clipboard storage contains an untrusted or oversized entry");
      if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) { file.close(); sumDirectory(entry.path(), depth + 1, bytes, count); continue; }
      demand(GetFileSizeEx(file.value, &size) && size.QuadPart >= 0 && uint64_t(size.QuadPart) <= quotaBytes - bytes, "Clipboard storage contains an untrusted or oversized entry");
      bytes += uint64_t(size.QuadPart);
    }
  }
  uint64_t retainedBytes() const {
    uint64_t bytes = 0; size_t count = 0;
    for (const auto& folder : std::filesystem::directory_iterator(multitap::clipboard::longPath(parent_))) {
      demand(++count <= 4096, "Clipboard storage contains too many entries");
      Handle directory(CreateFileW(multitap::clipboard::longPath(folder.path()).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      FILE_ATTRIBUTE_TAG_INFO directoryInfo{};
      demand(directory.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(directory.value, FileAttributeTagInfo, &directoryInfo, sizeof(directoryInfo)) &&
          !(directoryInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Clipboard storage entry changed");
      if (!(directoryInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        LARGE_INTEGER size{}; demand(GetFileSizeEx(directory.value, &size) && size.QuadPart >= 0 && uint64_t(size.QuadPart) <= quotaBytes - bytes, "Clipboard storage quota exceeded");
        bytes += uint64_t(size.QuadPart); continue;
      }
      directory.close();
      sumDirectory(folder.path(), 1, bytes, count);
    }
    return bytes;
  }
  void removeReceipt(const std::filesystem::path& path) const noexcept {
    Handle file(CreateFileW(multitap::clipboard::longPath(path).c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return;
    FILE_ATTRIBUTE_TAG_INFO info{}; FILE_DISPOSITION_INFO disposition{TRUE};
    if (GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &info, sizeof(info)) && !(info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
  }
};
}
