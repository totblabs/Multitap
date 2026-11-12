// drive-v1 PC side (docs/drive-redirection.md): the client runner's drive file
// core in a temporary folder, without impersonation. Paths stay inside the
// share, links (junctions, symbolic links) are refused and never listed,
// every operation's semantics and read-only shares. Built by the Host CMake
// project (MultitapClientDriveFilesTest); never touches anything outside its
// own temporary folder.
#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>
#include <string>
#include <vector>

#include "drive_files_core.h"

using multitap::drive::Entry;
using multitap::drive::Failure;
using multitap::drive::Files;

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}
std::string Code(const std::function<void()>& action) {
  try {
    action();
    return "ok";
  } catch (const Failure& failure) {
    if (GetEnvironmentVariableW(L"MULTITAP_DRIVE_TEST_VERBOSE", nullptr, 0)) std::printf("  native error: %lu\n", failure.win32());
    return failure.what();
  } catch (const std::exception& error) {
    return std::string("exception:") + error.what();
  }
}
void Expect(const char* what, const std::function<void()>& action, const char* code) {
  const auto got = Code(action);
  if (got != code) std::printf("  %s: expected %s, got %s\n", what, code, got.c_str());
  Check(got == code, what);
}
bool WriteFileText(const std::wstring& path, const std::string& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
  CloseHandle(file);
  return ok;
}
std::string ReadFileText(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return "<missing>";
  std::string out(4096, '\0');
  DWORD got = 0;
  ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
  CloseHandle(file);
  out.resize(got);
  return out;
}
// A directory junction (mount point) without any privilege.
bool MakeJunction(const std::wstring& link, const std::wstring& target, bool existing = false) {
  if (!existing && !CreateDirectoryW(link.c_str(), nullptr)) return false;
  HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, existing ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + target, print = target;
  const size_t path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::vector<BYTE> buffer(16 + path_bytes, 0);
  auto put16 = [&](size_t at, size_t value) { buffer[at] = BYTE(value & 0xff); buffer[at + 1] = BYTE((value >> 8) & 0xff); };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  memcpy(buffer.data(), &tag, 4);
  put16(4, 8 + path_bytes);                                     // ReparseDataLength
  put16(8, 0);                                                  // SubstituteNameOffset
  put16(10, substitute.size() * sizeof(wchar_t));               // SubstituteNameLength
  put16(12, (substitute.size() + 1) * sizeof(wchar_t));         // PrintNameOffset
  put16(14, print.size() * sizeof(wchar_t));                    // PrintNameLength
  memcpy(buffer.data() + 16, substitute.c_str(), substitute.size() * sizeof(wchar_t));
  memcpy(buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(), print.size() * sizeof(wchar_t));
  DWORD returned = 0;
  const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
  CloseHandle(handle);
  return ok;
}
bool Listed(const std::vector<Entry>& entries, const std::wstring& name) {
  for (const auto& entry : entries) if (entry.name == name) return true;
  return false;
}
bool ClearJunction(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  BYTE request[8]{}; const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT; memcpy(request, &tag, sizeof(tag)); DWORD got = 0;
  const bool ok = DeviceIoControl(file, FSCTL_DELETE_REPARSE_POINT, request, sizeof(request), nullptr, 0, &got, nullptr) != FALSE;
  CloseHandle(file); return ok;
}
void RemoveTree(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return;
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") RemoveTree(path + L"\\" + name);
      } while (FindNextFileW(find, &data));
      FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
  } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    RemoveDirectoryW(path.c_str());  // a junction: only the link
  } else {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path.c_str());
  }
}
}  // namespace

int main() {
  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring base = std::wstring(temp) + L"multitap-drive-core-" + std::to_wstring(GetCurrentProcessId());
  RemoveTree(base);
  const std::wstring share = base + L"\\share", outside = base + L"\\outside";
  CreateDirectoryW(base.c_str(), nullptr);
  CreateDirectoryW(share.c_str(), nullptr);
  CreateDirectoryW(outside.c_str(), nullptr);
  CreateDirectoryW((share + L"\\sub").c_str(), nullptr);
  Check(WriteFileText(share + L"\\a.txt", "hello"), "fixture a.txt");
  Check(WriteFileText(share + L"\\sub\\b.bin", "bee"), "fixture b.bin");
  Check(WriteFileText(outside + L"\\secret.txt", "secret"), "fixture secret");
  const bool junction = MakeJunction(share + L"\\link", outside);
  Check(junction, "fixture junction (FSCTL_SET_REPARSE_POINT)");
  const bool symlink = CreateSymbolicLinkW((share + L"\\slink.txt").c_str(), (outside + L"\\secret.txt").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
  if (!symlink) std::printf("note: symbolic links unavailable (no privilege/developer mode); junction checks still run\n");

  Files files;
  std::wstring label;
  Expect("open share", [&] { label = files.Open("s1", share, false); }, "ok");
  Check(label == L"share", "open returns the folder name as label");
  Expect("open a file is refused", [&] { files.Open("bad", share + L"\\a.txt", false); }, "drive_not_directory");
  Expect("open a relative path is refused", [&] { files.Open("bad", L"share", false); }, "drive_invalid_path");
  Expect("open an invalid share id", [&] { files.Open("a b", share, false); }, "drive_invalid_path");
  Expect("unknown share", [&] { files.Stat("nope", L""); }, "drive_share_unknown");

  // stat / list
  Entry root;
  Expect("stat root", [&] { root = files.Stat("s1", L""); }, "ok");
  Check(root.dir && root.name.empty() && root.hasSpace && root.free <= root.total && root.total > 0, "root is a directory with space");
  Entry a;
  Expect("stat a.txt", [&] { a = files.Stat("s1", L"a.txt"); }, "ok");
  Check(!a.dir && a.size == 5 && a.name == L"a.txt" && a.modified > 0, "a.txt entry");
  std::vector<Entry> listed;
  Expect("list root", [&] { listed = files.List("s1", L""); }, "ok");
  Check(Listed(listed, L"a.txt") && Listed(listed, L"sub"), "list has files and folders");
  Check(!Listed(listed, L"link") && !Listed(listed, L"slink.txt"), "links are never listed");
  Expect("list a file", [&] { files.List("s1", L"a.txt"); }, "drive_not_directory");
  Expect("list missing", [&] { files.List("s1", L"missing"); }, "drive_not_found");
  // A failed open carries its Win32 error for the diagnostics (0 when none).
  try {
    files.Open("s9", L"C:\\multitap-no-such-folder-\\x", false);
    Check(false, "open of a missing folder fails");
  } catch (const Failure& failure) {
    Check(std::string(failure.what()) == "drive_not_found" && (failure.win32() == ERROR_PATH_NOT_FOUND || failure.win32() == ERROR_FILE_NOT_FOUND), "open failure carries the Win32 error");
  }

  // path validation (no escape, no Windows path syntax, no device names)
  for (const auto* bad : {L"..", L"a/../a.txt", L"./a.txt", L"sub\\b.bin", L"C:", L"C:/Windows", L"/a.txt", L"a.txt/", L"sub//b.bin",
                          L"CON", L"con.txt", L"NUL.tar.gz", L"COM1", L"lpt9.log", L"a.", L"a ", L"a:b", L"a*", L"a?b", L"\\\\server\\share"}) {
    const auto got = Code([&] { files.Stat("s1", bad); });
    Check(got == "drive_invalid_path", "invalid path refused");
    if (got != "drive_invalid_path") std::printf("  path %ls -> %s\n", bad, got.c_str());
  }
  std::wstring deep;
  for (int i = 0; i < 65; ++i) deep += (i ? L"/" : L"") + std::wstring(L"d");
  Expect("depth 65 refused", [&] { files.Stat("s1", deep); }, "drive_invalid_path");

  // links below the root are refused for every operation
  if (junction) {
    Expect("stat through junction", [&] { files.Stat("s1", L"link"); }, "drive_link_refused");
    Expect("read through junction", [&] { files.Read("s1", L"link/secret.txt", 0, 10); }, "drive_link_refused");
    Expect("list junction", [&] { files.List("s1", L"link"); }, "drive_link_refused");
    const std::string data = "x";
    Expect("write through junction", [&] { files.Write("s1", L"link/new.txt", 0, reinterpret_cast<const uint8_t*>(data.data()), 1, true); }, "drive_link_refused");
    Expect("create through junction", [&] { files.Create("s1", L"link/d", true, false); }, "drive_link_refused");
    Expect("rename into junction", [&] { files.Rename("s1", L"a.txt", L"link/a.txt", false); }, "drive_link_refused");
    Expect("delete junction", [&] { files.Delete("s1", L"link"); }, "drive_link_refused");
    Expect("touch through junction", [&] { files.Touch("s1", L"link/secret.txt", 0); }, "drive_link_refused");
    Check(GetFileAttributesW((outside + L"\\new.txt").c_str()) == INVALID_FILE_ATTRIBUTES, "nothing was written outside");
    Check(GetFileAttributesW((outside + L"\\d").c_str()) == INVALID_FILE_ATTRIBUTES, "nothing was created outside");
    Check(ReadFileText(outside + L"\\secret.txt") == "secret", "outside file untouched");
  }
  if (symlink) {
    Expect("read symlink", [&] { files.Read("s1", L"slink.txt", 0, 10); }, "drive_link_refused");
    Expect("stat symlink", [&] { files.Stat("s1", L"slink.txt"); }, "drive_link_refused");
  }

  // read
  std::vector<uint8_t> bytes;
  Expect("read whole", [&] { bytes = files.Read("s1", L"a.txt", 0, 100); }, "ok");
  Check(std::string(bytes.begin(), bytes.end()) == "hello", "read content");
  Expect("read range", [&] { bytes = files.Read("s1", L"a.txt", 1, 2); }, "ok");
  Check(std::string(bytes.begin(), bytes.end()) == "el", "read range content");
  Expect("read past end", [&] { bytes = files.Read("s1", L"a.txt", 50, 10); }, "ok");
  Check(bytes.empty(), "read past end is empty");
  Expect("read a folder", [&] { files.Read("s1", L"sub", 0, 10); }, "drive_is_directory");
  Expect("read missing", [&] { files.Read("s1", L"nope.txt", 0, 10); }, "drive_not_found");
  Expect("read zero length", [&] { files.Read("s1", L"a.txt", 0, 0); }, "drive_invalid_path");
  Expect("read over chunk", [&] { files.Read("s1", L"a.txt", 0, 262145); }, "drive_invalid_path");

  // write
  const auto write = [&](const wchar_t* path, uint64_t offset, const std::string& text, bool truncate) {
    files.Write("s1", path, offset, reinterpret_cast<const uint8_t*>(text.data()), text.size(), truncate);
  };
  Expect("write creates", [&] { write(L"new.txt", 0, "abc", true); }, "ok");
  Expect("write appends at offset", [&] { write(L"new.txt", 3, "de", false); }, "ok");
  Check(ReadFileText(share + L"\\new.txt") == "abcde", "written content");
  Expect("write truncates", [&] { write(L"new.txt", 0, "x", true); }, "ok");
  Check(ReadFileText(share + L"\\new.txt") == "x", "truncated content");
  Expect("write missing without truncate", [&] { write(L"none.txt", 0, "x", false); }, "drive_not_found");
  Expect("write to a folder", [&] { write(L"sub", 0, "x", true); }, "drive_is_directory");
  Expect("write in a missing folder", [&] { write(L"nofolder/x.txt", 0, "x", true); }, "drive_not_found");
  Expect("truncating write at an offset", [&] { write(L"new.txt", 1, "x", true); }, "drive_invalid_path");

  // create
  Expect("create folder", [&] { files.Create("s1", L"dir1", true, false); }, "ok");
  Expect("create folder again", [&] { files.Create("s1", L"dir1", true, false); }, "drive_exists");
  Expect("create file", [&] { files.Create("s1", L"dir1/f.txt", false, false); }, "ok");
  Expect("create file again", [&] { files.Create("s1", L"dir1/f.txt", false, false); }, "drive_exists");
  Check(WriteFileText(share + L"\\dir1\\f.txt", "data"), "fixture f.txt content");
  Expect("create file truncating", [&] { files.Create("s1", L"dir1/f.txt", false, true); }, "ok");
  Check(ReadFileText(share + L"\\dir1\\f.txt").empty(), "create truncate empties");
  Expect("create in missing folder", [&] { files.Create("s1", L"missing/x", true, false); }, "drive_not_found");
  Expect("create the root", [&] { files.Create("s1", L"", true, false); }, "drive_exists");
  const std::wstring longName(200, L'n');
  Expect("create a long name", [&] { files.Create("s1", longName, false, false); }, "ok");

  // Mark-of-the-Web on what the remote desktop creates or writes (the fixture
  // folder is on the temporary folder's volume; skipped where it has no
  // named streams).
  {
    DWORD flags = 0;
    GetVolumeInformationW((share.substr(0, 3)).c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0);
    if (flags & FILE_NAMED_STREAMS) {
      const std::string internet = "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=about:internet\r\nHostUrl=about:internet\r\n";
      const auto zone = [&](const std::wstring& name) { return ReadFileText(L"\\\\?\\" + share + L"\\" + name + L":Zone.Identifier"); };
      Check(zone(L"dir1\\f.txt") == internet, "a created file is marked Internet zone");
      Check(zone(longName) == internet, "a created empty file is marked");
      Check(zone(L"new.txt") == internet, "a written file is marked");
      Check(zone(L"dir1") == "<missing>", "a created folder carries no mark");
      Check(zone(L"a.txt") == "<missing>", "a file the remote only read stays unmarked");
      Check(WriteFileText(share + L"\\pc.txt", "pc"), "fixture pc.txt");
      Expect("append to a PC file", [&] { write(L"pc.txt", 2, "!", false); }, "ok");
      Check(zone(L"pc.txt") == internet, "a PC file the remote wrote into is marked");
      Check(WriteFileText(share + L"\\pc.txt:Zone.Identifier", "[ZoneTransfer]\r\nZoneId=0\r\n"), "fixture local-zone mark");
      Expect("overwrite a locally marked file", [&] { write(L"pc.txt", 0, "y", true); }, "ok");
      Check(zone(L"pc.txt") == internet, "a weaker zone is replaced by the Internet zone");
      const std::string restricted = "[ZoneTransfer]\r\nZoneId=4\r\n";
      Check(WriteFileText(share + L"\\pc.txt:Zone.Identifier", restricted), "fixture restricted-zone mark");
      Expect("write a restricted-zone file", [&] { write(L"pc.txt", 0, "z", true); }, "ok");
      Check(zone(L"pc.txt") == restricted, "a Restricted zone mark is kept");
      Check(ReadFileText(share + L"\\pc.txt") == "z", "the marked file holds the written content");
      Expect("remove pc.txt", [&] { files.Delete("s1", L"pc.txt"); }, "ok");
    } else {
      std::printf("note: skipped the Mark-of-the-Web checks (the temporary folder's volume has no named streams)\n");
    }
  }

  // Shell metadata names are never made by the remote desktop.
  for (const auto* name : {L"desktop.ini", L"Desktop.INI", L"autorun.inf", L"AUTORUN.INF", L"x.lnk", L"x.LNK", L".lnk", L"x.url",
                           L"x.scf", L"Documents.library-ms", L"Search.searchConnector-ms", L"x.SEARCHCONNECTOR-MS"}) {
    Check(multitap::drive::ShellMetadataName(name), "shell metadata name recognised");
    Check(Code([&] { files.Create("s1", name, false, false); }) == "drive_denied", "creating a shell metadata file is refused");
    Check(Code([&] { files.Create("s1", std::wstring(L"sub/") + name, true, false); }) == "drive_denied", "creating a shell metadata folder is refused");
    Check(Code([&] { files.Write("s1", name, 0, reinterpret_cast<const uint8_t*>("x"), 1, true); }) == "drive_denied", "writing a shell metadata file is refused");
    Check(Code([&] { files.Rename("s1", L"a.txt", name, false); }) == "drive_denied", "renaming to a shell metadata name is refused");
    Check(GetFileAttributesW((share + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES, "no shell metadata file was made");
  }
  for (const auto* name : {L"desktop.ini.txt", L"my desktop.ini", L"lnk", L".lnk.txt", L"x.lnkx", L"autorun.inf.bak", L"url", L"a.txt"}) {
    Check(!multitap::drive::ShellMetadataName(name), "an ordinary name is not shell metadata");
  }
  Check(WriteFileText(share + L"\\desktop.ini", "[.ShellClassInfo]"), "fixture PC desktop.ini");
  Expect("an existing PC desktop.ini is not overwritten", [&] { write(L"desktop.ini", 0, "x", true); }, "drive_denied");
  Check(ReadFileText(share + L"\\desktop.ini") == "[.ShellClassInfo]", "the PC desktop.ini is unchanged");
  Expect("an existing PC desktop.ini is still readable", [&] { files.Read("s1", L"desktop.ini", 0, 10); }, "ok");
  Expect("an existing PC desktop.ini may be deleted", [&] { files.Delete("s1", L"desktop.ini"); }, "ok");

  // rename
  Expect("rename file", [&] { files.Rename("s1", L"dir1/f.txt", L"dir1/g.txt", false); }, "ok");
  Check(GetFileAttributesW((share + L"\\dir1\\g.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "renamed file exists");
  Expect("rename onto existing", [&] { files.Rename("s1", L"new.txt", L"dir1/g.txt", false); }, "drive_exists");
  Expect("rename replacing", [&] { files.Rename("s1", L"new.txt", L"dir1/g.txt", true); }, "ok");
  Check(ReadFileText(share + L"\\dir1\\g.txt") == "x", "replaced content");
  Expect("rename missing", [&] { files.Rename("s1", L"gone.txt", L"x.txt", false); }, "drive_not_found");
  Expect("rename into missing folder", [&] { files.Rename("s1", L"a.txt", L"nofolder/a.txt", false); }, "drive_not_found");
  Expect("rename folder", [&] { files.Rename("s1", L"dir1", L"dir2", false); }, "ok");
  // Case-only rename (Explorer sends no Overwrite): the destination "exists"
  // as the same object, which is allowed; a different existing file is not.
  Check(WriteFileText(share + L"\\Report.txt", "case"), "fixture Report.txt");
  Expect("case-only rename of a file", [&] { files.Rename("s1", L"Report.txt", L"report.txt", false); }, "ok");
  {
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((share + L"\\report.txt").c_str(), &found);
    Check(find != INVALID_HANDLE_VALUE && std::wstring(found.cFileName) == L"report.txt", "case-only rename changed the stored capitalisation");
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
  }
  Check(ReadFileText(share + L"\\report.txt") == "case", "case-only rename kept the content");
  Expect("case-only rename of a folder", [&] { files.Rename("s1", L"dir2", L"DIR2", false); }, "ok");
  Expect("case-only rename back", [&] { files.Rename("s1", L"DIR2", L"dir2", false); }, "ok");
  Check(WriteFileText(share + L"\\other.txt", "o"), "fixture other.txt");
  Expect("different file onto existing stays refused", [&] { files.Rename("s1", L"other.txt", L"REPORT.txt", false); }, "drive_exists");
  Check(ReadFileText(share + L"\\report.txt") == "case", "refused rename left the target alone");
  Expect("remove report.txt", [&] { files.Delete("s1", L"report.txt"); }, "ok");
  Expect("remove other.txt", [&] { files.Delete("s1", L"other.txt"); }, "ok");

  // delete
  Expect("delete non-empty folder", [&] { files.Delete("s1", L"dir2"); }, "drive_not_empty");
  Expect("delete file", [&] { files.Delete("s1", L"dir2/g.txt"); }, "ok");
  Expect("delete empty folder", [&] { files.Delete("s1", L"dir2"); }, "ok");
  Check(GetFileAttributesW((share + L"\\dir2").c_str()) == INVALID_FILE_ATTRIBUTES, "folder deleted");
  Expect("delete missing", [&] { files.Delete("s1", L"dir2"); }, "drive_not_found");
  Expect("delete the root", [&] { files.Delete("s1", L""); }, "drive_denied");

  // touch
  Expect("touch", [&] { files.Touch("s1", L"a.txt", 1700000000000LL); }, "ok");
  Expect("stat after touch", [&] { a = files.Stat("s1", L"a.txt"); }, "ok");
  Check(a.modified == 1700000000000LL, "touch sets the modification time");
  Expect("touch a folder", [&] { files.Touch("s1", L"sub", 0); }, "drive_is_directory");

  // setsize / flush (the "fs" revision) and write beyond the end
  {
    const auto sizeOf = [&](const wchar_t* path) { return files.Stat("s1", path).size; };
    Expect("setsize fixture", [&] { files.Create("s1", L"sz.bin", false, true); write(L"sz.bin", 0, "0123456789", true); }, "ok");
    Expect("setsize shrink", [&] { files.SetSize("s1", L"sz.bin", 4); }, "ok");
    Check(ReadFileText(share + L"\\sz.bin") == "0123", "setsize shrink truncates");
    Expect("setsize grow", [&] { files.SetSize("s1", L"sz.bin", 8); }, "ok");
    Check(ReadFileText(share + L"\\sz.bin") == std::string("0123\0\0\0\0", 8), "setsize grow zero-fills");
    Expect("setsize same size", [&] { files.SetSize("s1", L"sz.bin", 8); }, "ok");
    Expect("setsize to zero", [&] { files.SetSize("s1", L"sz.bin", 0); }, "ok");
    Check(sizeOf(L"sz.bin") == 0, "setsize to zero empties");
    Expect("write beyond the end extends", [&] { write(L"sz.bin", 6, "ab", false); }, "ok");
    Check(ReadFileText(share + L"\\sz.bin") == std::string("\0\0\0\0\0\0ab", 8), "gap before a write beyond the end reads as zeros");
    // Above 4 GiB: only where the volume keeps the file sparse (else skipped).
    constexpr uint64_t big = 5ULL * 1024 * 1024 * 1024;
    HANDLE sparseFile = CreateFileW((share + L"\\sz.bin").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD sparseBytes = 0;
    const bool sparse = sparseFile != INVALID_HANDLE_VALUE && DeviceIoControl(sparseFile, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &sparseBytes, nullptr);
    if (sparseFile != INVALID_HANDLE_VALUE) CloseHandle(sparseFile);
    const auto grown = sparse ? Code([&] { files.SetSize("s1", L"sz.bin", big); }) : "sparse_not_supported";
    if (grown == "ok") {
      Check(sizeOf(L"sz.bin") == big, "size above 4 GiB is reported in 64 bits");
      Expect("write above 4 GiB", [&] { write(L"sz.bin", big + 3, "z", false); }, "ok");
      Check(sizeOf(L"sz.bin") == big + 4, "write above 4 GiB extends to offset + 1");
      Expect("read above 4 GiB", [&] { bytes = files.Read("s1", L"sz.bin", big + 3, 10); }, "ok");
      Check(std::string(bytes.begin(), bytes.end()) == "z", "read above 4 GiB");
    } else {
      std::printf("note: skipped the above-4-GiB checks (setsize of 5 GiB answered %s: no room or no sparse files)\n", grown.c_str());
    }
    Expect("setsize back to small", [&] { files.SetSize("s1", L"sz.bin", 1); }, "ok");
    Expect("setsize above the protocol bound", [&] { files.SetSize("s1", L"sz.bin", 9007199254740992ULL); }, "drive_invalid_path");
    Expect("read above the protocol bound", [&] { files.Read("s1", L"sz.bin", UINT64_MAX, 1); }, "drive_invalid_path");
    Expect("write offset overflow", [&] { write(L"sz.bin", UINT64_MAX, "x", false); }, "drive_invalid_path");
    Expect("write end above the protocol bound", [&] { write(L"sz.bin", 9007199254740991ULL, "x", false); }, "drive_invalid_path");
    Check(sizeOf(L"sz.bin") == 1, "refused offsets do not extend or overwrite the file");
    Expect("setsize missing", [&] { files.SetSize("s1", L"none-sz.bin", 1); }, "drive_not_found");
    Expect("setsize missing folder", [&] { files.SetSize("s1", L"nofolder/x.bin", 1); }, "drive_not_found");
    Expect("setsize a folder", [&] { files.SetSize("s1", L"sub", 1); }, "drive_is_directory");
    Expect("setsize the root", [&] { files.SetSize("s1", L"", 1); }, "drive_is_directory");
    Expect("flush a file", [&] { files.Flush("s1", L"sz.bin"); }, "ok");
    // Successful writes do not imply that a later flush can open the file.
    // A new local lock or read-only attribute must never become a false ACK.
    const std::wstring flushPath = share + L"\\sz.bin";
    HANDLE flushLock = CreateFileW(flushPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(flushLock != INVALID_HANDLE_VALUE, "create flush sharing-denial fixture");
    if (flushLock != INVALID_HANDLE_VALUE) {
      Expect("flush reports a new local sharing lock", [&] { files.Flush("s1", L"sz.bin"); }, "drive_busy");
      CloseHandle(flushLock);
    }
    const DWORD flushAttributes = GetFileAttributesW(flushPath.c_str());
    Check(flushAttributes != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(flushPath.c_str(), flushAttributes | FILE_ATTRIBUTE_READONLY), "create flush access-denial fixture");
    Expect("flush reports a new local write denial", [&] { files.Flush("s1", L"sz.bin"); }, "drive_denied");
    Check(SetFileAttributesW(flushPath.c_str(), flushAttributes) != FALSE, "restore private flush fixture attributes");
    Expect("flush succeeds after the local denial ends", [&] { files.Flush("s1", L"sz.bin"); }, "ok");
    Expect("flush a folder", [&] { files.Flush("s1", L"sub"); }, "ok");
    Expect("flush the root", [&] { files.Flush("s1", L""); }, "ok");
    Expect("flush missing", [&] { files.Flush("s1", L"none-sz.bin"); }, "drive_not_found");
    Expect("flush a bad path", [&] { files.Flush("s1", L"../x"); }, "drive_invalid_path");
    Expect("flush an unknown share", [&] { files.Flush("nope", L"a.txt"); }, "drive_share_unknown");
    Expect("setsize an unknown share", [&] { files.SetSize("nope", L"a.txt", 1); }, "drive_share_unknown");
    if (junction) {
      Expect("setsize through junction", [&] { files.SetSize("s1", L"link/secret.txt", 0); }, "drive_link_refused");
      Expect("flush through junction", [&] { files.Flush("s1", L"link/secret.txt"); }, "drive_link_refused");
      Check(ReadFileText(outside + L"\\secret.txt") == "secret", "setsize through a junction left the outside file alone");
    }
    if (symlink) {
      Expect("setsize symlink", [&] { files.SetSize("s1", L"slink.txt", 0); }, "drive_link_refused");
      Expect("flush symlink", [&] { files.Flush("s1", L"slink.txt"); }, "drive_link_refused");
      Check(ReadFileText(outside + L"\\secret.txt") == "secret", "setsize through a symlink left the outside file alone");
    }
    Expect("setsize cleanup", [&] { files.Delete("s1", L"sz.bin"); }, "ok");
  }

  // Deterministic mutation between validation and rename, inside this fixture.
  {
    const auto parent = share + L"\\pinned-parent", moved = share + L"\\parent-moved";
    CreateDirectoryW(parent.c_str(), nullptr); Check(WriteFileText(share + L"\\pinned-source.txt", "pinned"), "fixture pinned source");
    bool parentBlocked = false, sourceBlocked = false;
    Files guarded([&] {
      parentBlocked = !MoveFileExW(parent.c_str(), moved.c_str(), 0);
      sourceBlocked = !MoveFileExW((share + L"\\pinned-source.txt").c_str(), (outside + L"\\stolen-source.txt").c_str(), 0);
    });
    Expect("open anchored rename share", [&] { guarded.Open("guard", share, false); }, "ok");
    Expect("rename keeps source and destination parent pinned", [&] { guarded.Rename("guard", L"pinned-source.txt", L"pinned-parent/moved.txt", false); }, "ok");
    Check(parentBlocked && sourceBlocked, "checked rename handles prevent source or parent replacement");
    Check(ReadFileText(parent + L"\\moved.txt") == "pinned", "the checked source stayed inside the selected share");
    guarded.Close("guard");
    // Changing a checked empty parent's reparse tag must never redirect the
    // handle-relative target into the outside fixture directory.
    const auto linkParent = share + L"\\reparse-parent";
    CreateDirectoryW(linkParent.c_str(), nullptr); Check(WriteFileText(share + L"\\reparse-source.txt", "rooted"), "fixture reparse source");
    bool changed = false;
    Files reparse([&] { changed = MakeJunction(linkParent, outside, true); });
    Expect("open reparse race share", [&] { reparse.Open("guard", share, false); }, "ok");
    const auto outcome = Code([&] { reparse.Rename("guard", L"reparse-source.txt", L"reparse-parent/escaped.txt", false); });
    std::printf("note: late parent junction change %s; rename result %s\n", changed ? "applied" : "blocked", outcome.c_str());
    if (changed) Check(ClearJunction(linkParent), "remove only the fixture junction tag after the race");
    Check(GetFileAttributesW((outside + L"\\escaped.txt").c_str()) == INVALID_FILE_ATTRIBUTES, "a late parent reparse change cannot escape the share");
    Check(ReadFileText(outside + L"\\secret.txt") == "secret", "the outside fixture is unchanged");
    if (outcome == "ok") Check(ReadFileText(linkParent + L"\\escaped.txt") == "rooted", "relative rename used the original directory object");
    else Check(ReadFileText(share + L"\\reparse-source.txt") == "rooted", "refused reparse race preserves the original file");
    reparse.Close("guard");
  }

  // read-only share: reads only
  Expect("open read-only", [&] { files.Open("ro", share, true); }, "ok");
  Expect("read-only read", [&] { bytes = files.Read("ro", L"a.txt", 0, 5); }, "ok");
  Expect("read-only write", [&] { files.Write("ro", L"a.txt", 0, reinterpret_cast<const uint8_t*>("z"), 1, true); }, "drive_read_only");
  Expect("read-only create", [&] { files.Create("ro", L"x", true, false); }, "drive_read_only");
  Expect("read-only rename", [&] { files.Rename("ro", L"a.txt", L"b.txt", false); }, "drive_read_only");
  Expect("read-only delete", [&] { files.Delete("ro", L"a.txt"); }, "drive_read_only");
  Expect("read-only touch", [&] { files.Touch("ro", L"a.txt", 0); }, "drive_read_only");
  Expect("read-only setsize", [&] { files.SetSize("ro", L"a.txt", 1); }, "drive_read_only");
  Expect("read-only flush is a no-op", [&] { files.Flush("ro", L"a.txt"); }, "ok");
  Check(ReadFileText(share + L"\\a.txt") == "hello", "read-only share left the file unchanged");

  // A share root that is itself a link is resolved once; paths stay inside its target.
  if (junction) {
    Expect("open a linked root", [&] { label = files.Open("lr", share + L"\\link", false); }, "ok");
    Expect("read inside the linked root", [&] { bytes = files.Read("lr", L"secret.txt", 0, 6); }, "ok");
    Check(std::string(bytes.begin(), bytes.end()) == "secret", "linked root content");
    Expect("linked root has no way back", [&] { files.Stat("lr", L".."); }, "drive_invalid_path");
  }

  Expect("close the linked root and the read-only share", [&] { files.Close("lr"); files.Close("ro"); }, "ok");
  files.Close("s1");
  Expect("closed share", [&] { files.Stat("s1", L""); }, "drive_share_unknown");

  // The share follows the folder, not the path.
  {
    const std::wstring follow = base + L"\\follow", renamed = base + L"\\renamed", parent = base + L"\\elsewhere";
    CreateDirectoryW(follow.c_str(), nullptr);
    Check(WriteFileText(follow + L"\\f.txt", "follow"), "fixture follow f.txt");
    Expect("open the followed folder", [&] { files.Open("fl", follow, false); }, "ok");
    Files::Status status;
    Expect("status", [&] { status = files.StatusOf("fl"); }, "ok");
    Check(!status.deleted && status.label == L"follow", "status before any change");
    Check(!status.offline, "a folder on a working disk is not offline");

    // Renamed on the PC while shared: operations keep working, the path follows.
    Check(MoveFileW(follow.c_str(), renamed.c_str()) != FALSE, "rename the shared folder on the PC");
    Expect("read after the root was renamed", [&] { bytes = files.Read("fl", L"f.txt", 0, 100); }, "ok");
    Check(std::string(bytes.begin(), bytes.end()) == "follow", "content after rename");
    Expect("write after rename", [&] { files.Write("fl", L"g.txt", 0, reinterpret_cast<const uint8_t*>("g"), 1, true); }, "ok");
    Check(ReadFileText(renamed + L"\\g.txt") == "g", "written into the renamed folder");
    Check(GetFileAttributesW((follow + L"\\g.txt").c_str()) == INVALID_FILE_ATTRIBUTES, "nothing written at the old path");
    Expect("status after rename", [&] { status = files.StatusOf("fl"); }, "ok");
    Check(!status.deleted && status.label == L"renamed", "label follows the rename");
    Check(status.path.size() >= 8 && status.path.compare(status.path.size() - 8, 8, L"\\renamed") == 0 && status.path.rfind(L"\\\\?\\", 0) != 0, "path follows the rename");

    // Moved under another folder.
    CreateDirectoryW(parent.c_str(), nullptr);
    Check(MoveFileW(renamed.c_str(), (parent + L"\\moved").c_str()) != FALSE, "move the shared folder on the PC");
    Expect("list after the root was moved", [&] { listed = files.List("fl", L""); }, "ok");
    Check(Listed(listed, L"f.txt") && Listed(listed, L"g.txt"), "listing after move");
    Expect("status after move", [&] { status = files.StatusOf("fl"); }, "ok");
    Check(status.label == L"moved", "label follows the move");

    // Deleted on the PC while shared: the share ends and stays ended.
    Check(DeleteFileW((parent + L"\\moved\\f.txt").c_str()) && DeleteFileW((parent + L"\\moved\\g.txt").c_str()), "empty the shared folder");
    Check(RemoveDirectoryW((parent + L"\\moved").c_str()) != FALSE, "delete the shared folder on the PC");
    Expect("status after delete", [&] { status = files.StatusOf("fl"); }, "ok");
    Check(status.deleted, "deleted root is reported");
    Expect("stat after delete", [&] { files.Stat("fl", L""); }, "drive_not_found");
    Expect("list after delete", [&] { files.List("fl", L""); }, "drive_not_found");
    // A folder of the same name made again is not the shared one.
    CreateDirectoryW((parent + L"\\moved").c_str(), nullptr);
    Check(WriteFileText(parent + L"\\moved\\f.txt", "new"), "fixture recreated f.txt") ;
    Expect("read after the folder was recreated", [&] { files.Read("fl", L"f.txt", 0, 10); }, "drive_not_found");
    Expect("write after the folder was recreated", [&] { files.Write("fl", L"h.txt", 0, reinterpret_cast<const uint8_t*>("h"), 1, true); }, "drive_not_found");
    Check(GetFileAttributesW((parent + L"\\moved\\h.txt").c_str()) == INVALID_FILE_ATTRIBUTES, "recreated folder untouched");
    files.Close("fl");
    Expect("closed followed share", [&] { files.Stat("fl", L""); }, "drive_share_unknown");
  }

  // Non-ASCII names (Korean, an emoji beyond the BMP), a path longer than
  // MAX_PATH, and the root of a drive as the share.
  {
    const std::wstring wide = base + L"\\wide";
    CreateDirectoryW(wide.c_str(), nullptr);
    Expect("open the unicode share", [&] { files.Open("u1", wide, false); }, "ok");
    const std::wstring korean = L"한글 보고서 \U0001F600.txt";
    const std::string text = "안녕";
    Expect("create a Korean name", [&] { files.Write("u1", korean, 0, reinterpret_cast<const uint8_t*>(text.data()), text.size(), true); }, "ok");
    Check(GetFileAttributesW((wide + L"\\" + korean).c_str()) != INVALID_FILE_ATTRIBUTES, "the Korean file exists on disk");
    std::vector<Entry> names;
    Expect("list Korean names", [&] { names = files.List("u1", L""); }, "ok");
    Check(Listed(names, korean), "a Korean and emoji name is listed intact");
    std::vector<uint8_t> back;
    Expect("read a Korean name", [&] { back = files.Read("u1", korean, 0, 100); }, "ok");
    Check(std::string(back.begin(), back.end()) == text, "content under a Korean name");
    Expect("rename to another Korean name", [&] { files.Rename("u1", korean, L"새 이름.txt", false); }, "ok");
    Expect("stat the new Korean name", [&] { files.Stat("u1", L"새 이름.txt"); }, "ok");
    // Six levels of 50 characters: far beyond MAX_PATH (260) inside the share.
    std::wstring relative, absolute = wide;
    for (int level = 0; level < 6; ++level) {
      const std::wstring part = std::wstring(48, L'a' + static_cast<wchar_t>(level)) + L"한글";
      relative += (relative.empty() ? L"" : L"/") + part;
      absolute += L"\\" + part;
      Expect("create a deep folder", [&] { files.Create("u1", relative, true, false); }, "ok");
    }
    Check(absolute.size() > 300, "the fixture path is longer than MAX_PATH");
    Expect("write below a long path", [&] { files.Write("u1", relative + L"/deep.txt", 0, reinterpret_cast<const uint8_t*>("d"), 1, true); }, "ok");
    Expect("stat below a long path", [&] { files.Stat("u1", relative + L"/deep.txt"); }, "ok");
    Expect("list a long path", [&] { names = files.List("u1", relative); }, "ok");
    Check(Listed(names, L"deep.txt"), "the file below a long path is listed");
    Expect("rename below a long path", [&] { files.Rename("u1", relative + L"/deep.txt", relative + L"/renamed.txt", false); }, "ok");
    Expect("rename back below a long path", [&] { files.Rename("u1", relative + L"/renamed.txt", relative + L"/deep.txt", false); }, "ok");
    // A share whose own root path is long (the folder the user picked).
    Expect("open a share at a long root", [&] { files.Open("u2", absolute, false); }, "ok");
    Expect("list a long root", [&] { names = files.List("u2", L""); }, "ok");
    Check(Listed(names, L"deep.txt"), "a share at a path longer than MAX_PATH works");
    files.Close("u2");
    files.Close("u1");
    // The root of a drive (the volume holding the temporary folder): label is
    // the letter, space is reported, nothing is written.
    const std::wstring driveRoot = std::wstring(base.substr(0, 3));
    if (driveRoot.size() == 3 && driveRoot[1] == L':') {
      std::wstring rootLabel;
      Expect("open a drive root", [&] { rootLabel = files.Open("dr", driveRoot, true); }, "ok");
      Check(rootLabel.size() == 1, "a drive root is labelled by its letter");
      Entry rootEntry;
      Expect("stat a drive root", [&] { rootEntry = files.Stat("dr", L""); }, "ok");
      Check(rootEntry.dir && rootEntry.hasSpace && rootEntry.total >= rootEntry.free, "a drive root reports its space");
      Expect("list a drive root", [&] { files.List("dr", L"", 20); }, "ok");
      Expect("a read-only drive root refuses writes", [&] { files.Create("dr", L"multitap-never.txt", false, false); }, "drive_read_only");
      files.Close("dr");
    }
  }

  // Status polls and file operations of different shares run at the same time
  // (the handle queries are made outside the share table's lock, so a stalled
  // network folder cannot hold up the others).
  {
    const std::wstring second = base + L"\\second";
    CreateDirectoryW(second.c_str(), nullptr);
    Check(WriteFileText(second + L"\\s.txt", "two"), "fixture second s.txt");
    Expect("open the second share", [&] { files.Open("s2", second, false); }, "ok");
    Expect("open the first share again", [&] { files.Open("s1", share, false); }, "ok");
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < 150; ++i) {
          try {
            if (t % 2) {
              const auto status = files.StatusOf(i % 2 ? "s1" : "s2");
              if (status.deleted || status.offline) ++bad;
            } else {
              const auto data = files.Read("s2", L"s.txt", 0, 16);
              if (std::string(data.begin(), data.end()) != "two") ++bad;
              (void)files.List("s1", L"");
            }
          } catch (const std::exception& error) {
            std::printf("  thread %d: %s\n", t, error.what());
            ++bad;
          }
        }
      });
    }
    for (auto& thread : threads) thread.join();
    Check(bad == 0, "parallel status polls and operations on two shares");
    files.Close("s2");
  }

  RemoveTree(L"\\\\?\\" + base + L"\\wide");  // deeper than MAX_PATH: long form
  RemoveTree(base);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
