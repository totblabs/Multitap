// Received clipboard files on disk: what the
// Host's worker creates and removes for a transfer is opened relative to a folder
// handle it holds, so a link that another program of the same account puts in
// between is refused or removed as a link, never followed; an elevated worker
// labels its folders High. Everything happens in a new folder under %TEMP%; the
// clipboard, a window station and other processes are not used.
#include "clipboard_worker.hpp"
#include <aclapi.h>
#include <winioctl.h>
#include <cstring>
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
void write(const std::filesystem::path& path, const std::string& text) { std::ofstream file(path, std::ios::binary); file << text; }
std::string read(const std::filesystem::path& path) { std::ifstream file(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(file)), {}); }
// A junction (no privilege needed, unlike a symbolic link).
bool junction(const std::filesystem::path& link, const std::filesystem::path& to) {
  if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
  WinHandle h(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (h.value == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + to.wstring(); const USHORT bytes = USHORT(substitute.size() * sizeof(wchar_t));
  std::vector<BYTE> buffer(16 + bytes + 2 * sizeof(wchar_t), 0);
  const auto put16 = [&](size_t at, USHORT value) { std::memcpy(buffer.data() + at, &value, 2); };
  const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT; std::memcpy(buffer.data(), &tag, 4); put16(4, USHORT(buffer.size() - 8));
  put16(8, 0); put16(10, bytes); put16(12, USHORT(bytes + sizeof(wchar_t))); put16(14, 0);
  std::memcpy(buffer.data() + 16, substitute.data(), bytes);
  DWORD returned = 0; return DeviceIoControl(h.value, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
}
bool reparse(const std::filesystem::path& path) { const DWORD a = GetFileAttributesW(path.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT); }
const std::string abcDigest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
// The mandatory label of [path]: 0 none, otherwise its RID (0x3000 High).
DWORD label(const std::filesystem::path& path) {
  PACL sacl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, &sacl, &descriptor) != ERROR_SUCCESS) return DWORD(-1);
  DWORD rid = 0;
  for (DWORD i = 0; sacl && i < sacl->AceCount; ++i) {
    void* raw = nullptr; if (!GetAce(sacl, i, &raw) || static_cast<ACE_HEADER*>(raw)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
    const PSID sid = &static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(raw)->SidStart; rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
  }
  LocalFree(descriptor); return rid;
}

void receiveAndExpire(const std::filesystem::path& base) {
  const auto parent = base / L"storage";
  clip::Storage storage(parent);
  std::vector<std::filesystem::path> paths;
  { clip::FileReceiver receiver(parent, {{L"received.bin", 3}}); receiver.append(0, 0, {'a', 'b', 'c'}); receiver.finish(0, abcDigest); paths = receiver.commit(); }
  require(read(paths[0]) == "abc", "A received file lost its bytes");
  require(read(paths[0].wstring() + L":Zone.Identifier").find("ZoneId=3") != std::string::npos, "A received file lacks the Internet-zone mark");
  const bool elevated = clip::processAboveMedium();
  if (elevated) require(label(paths[0].parent_path()) == SECURITY_MANDATORY_HIGH_RID && label(parent) == SECURITY_MANDATORY_HIGH_RID, "An elevated worker's transfer folder is not labelled High");
  else require(label(paths[0].parent_path()) == 0 && label(parent) == 0, "A medium process labelled its transfer folder");
  require(clip::expireFiles(parent, paths) && !std::filesystem::exists(paths[0].parent_path()), "A received transfer did not expire");
  // A cancelled transfer leaves nothing.
  { clip::FileReceiver receiver(parent, {{L"partial.bin", 3}}); receiver.append(0, 0, {'a'}); receiver.cancel(); }
  require(std::filesystem::is_empty(parent), "A cancelled transfer left files");
  // A transfer folder swapped for a junction after the commit: expiry refuses it, the target survives.
  { clip::FileReceiver receiver(parent, {{L"kept.bin", 3}}); receiver.append(0, 0, {'a', 'b', 'c'}); receiver.finish(0, abcDigest); paths = receiver.commit(); }
  const auto root = paths[0].parent_path(), moved = base / L"elsewhere";
  require(MoveFileExW(root.c_str(), moved.c_str(), 0) != FALSE && junction(root, moved), "Junction fixture unavailable");
  require(!clip::expireFiles(parent, paths) && read(moved / L"kept.bin") == "abc", "Expiry followed a junction in place of its transfer folder");
  require(RemoveDirectoryW(root.c_str()) != FALSE, "Junction fixture cleanup");
  // A folder tree: received relative to the held transfer folder; a discarded one leaves nothing.
  {
    clip::Bounds bounds;
    std::vector<clip::Item> items(3);
    items[0].kind = "dir"; items[0].name = L"a";
    items[1].kind = "dir"; items[1].name = L"a\\b";
    items[2].kind = "file"; items[2].name = L"a\\b\\c.txt"; items[2].size = 3;
    clip::Incoming incoming("tree", items, parent, &storage, {}, bounds);
    const auto staging = incoming.stagingRoot();
    require(std::filesystem::is_directory(staging / L"a" / L"b"), "A received tree's folders were not created");
    incoming.append(0, 0, {'a', 'b', 'c'}); incoming.finish(0, abcDigest);
    require(read(staging / L"a" / L"b" / L"c.txt") == "abc", "A received tree file lost its bytes");
  }
  for (const auto& entry : std::filesystem::directory_iterator(parent)) if (entry.path().filename().wstring().rfind(L"transfer-", 0) == 0) throw std::runtime_error("A discarded tree left its transfer folder");
  // expireTree with a junction in place of a listed folder: refused, the target survives.
  {
    const auto tree = parent / L"transfer-0123456789abcdef0123456789abcdef";
    std::filesystem::create_directories(tree);
    const auto outside = base / L"outside"; std::filesystem::create_directories(outside / L"b"); write(outside / L"b" / L"c.txt", "outside");
    require(junction(tree / L"a", outside), "Junction fixture unavailable");
    std::vector<clip::TreeEntry> nodes(3);
    nodes[0].relative = L"a"; nodes[0].directory = true; nodes[1].relative = L"a\\b"; nodes[1].directory = true; nodes[2].relative = L"a\\b\\c.txt";
    require(!clip::expireTree(parent, tree, nodes) && read(outside / L"b" / L"c.txt") == "outside", "Tree expiry followed a junction");
    // An unfinished folder with a junction inside is removed with the junction itself; what it pointed to stays.
    require(clip::Storage::removeWithoutFollowing(tree, 0) && !std::filesystem::exists(tree) && read(outside / L"b" / L"c.txt") == "outside", "Removing a folder followed a junction inside it");
  }
  // A read-only file in nested folders is removed too.
  {
    const auto tree = parent / L"transfer-fedcba9876543210fedcba9876543210";
    std::filesystem::create_directories(tree / L"x" / L"y"); write(tree / L"x" / L"y" / L"z.bin", "z");
    SetFileAttributesW((tree / L"x" / L"y" / L"z.bin").c_str(), FILE_ATTRIBUTE_READONLY);
    require(clip::removeWithoutFollowing(parent, tree.filename().wstring()) && !std::filesystem::exists(tree), "A plain folder tree was not removed");
  }
  require(std::filesystem::is_empty(parent), "Storage fixture left files");
}
}

int main() {
  try {
    const auto base = std::filesystem::temp_directory_path() / (L"multitap-clipboard-files-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    require(CreateDirectoryW(base.c_str(), nullptr) != FALSE, "Fixture folder");
    struct Cleanup { std::filesystem::path base; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(base, ignored); } } cleanup{base};
    receiveAndExpire(base);
    std::cout << "{\"kind\":\"clipboard-files-relative\",\"checks\":" << checks << ",\"elevated\":" << (clip::processAboveMedium() ? "true" : "false") << ",\"clipboardTouched\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
