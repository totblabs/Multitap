// The Host installer's own checks (installer/host_installer.cpp), compiled
// into this fixture with its entry point renamed. Runs
// unelevated in a new folder under %TEMP%: nothing is installed, no msiexec runs,
// ProgramData is not touched.
//   - a squatted single-instance lock is "foreign", never "another installer";
//   - a cache folder that is not exactly the installer's is moved aside with its
//     contents, never adopted (an unelevated run cannot create the replacement,
//     which needs the Administrators owner, so it reports failure);
//   - a package is written only as a new file, and while it is held nobody can
//     change, replace or delete it.
#define wWinMain hostInstallerMain
#include "../installer/host_installer.cpp"
#undef wWinMain
#include <cstdio>
#include <fstream>

namespace {
unsigned checks = 0, failed = 0;
void check(bool value, const char* name) { ++checks; if (!value) { ++failed; std::printf("FAIL %s\n", name); } }
std::string read(const fs::path& path) { std::ifstream file(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(file)), {}); }
}

int wmain() {
  // The real lock name, held for a moment (an installer started meanwhile would see a foreign owner and go on).
  {
    HANDLE first = nullptr, second = nullptr;
    const auto a = takeSingleInstance(first), b = takeSingleInstance(second);
    check(a == SingleInstance::ours, "the first installer owns the lock");
    // This fixture is not elevated: the lock it holds is owned by its own account, as a squatter's would be.
    check(elevated() ? b == SingleInstance::running : b == SingleInstance::foreign, "a lock owned by a standard account counted as another installer");
    if (first) CloseHandle(first);
    if (second) CloseHandle(second);
  }
  const fs::path root = fs::temp_directory_path() / (L"multitap-installer-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  check(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "fixture folder");
  // A folder someone else made first is moved aside with what is in it.
  {
    const fs::path cache = root / L"Installer";
    check(CreateDirectoryW(cache.c_str(), nullptr) != FALSE, "planted folder");
    { std::ofstream planted(cache / L"package.msi", std::ios::binary); planted << "planted"; }
    check(!exactProtectedFolder(cache), "a user's folder looked like the installer's");
    const bool made = protectedFolder(cache);
    size_t aside = 0;
    for (const auto& entry : fs::directory_iterator(root))
      if (entry.path().filename().wstring().rfind(L"Installer.untrusted-{", 0) == 0) { ++aside; check(read(entry.path() / L"package.msi") == "planted", "a moved-aside folder lost its contents"); }
    check(aside == 1, "the planted folder was not moved aside once");
    if (!elevated()) check(!made && !fs::exists(cache / L"package.msi"), "a planted folder was adopted");
    else check(made && exactProtectedFolder(cache) && fs::is_empty(cache), "the replacement folder is not exactly protected");
  }
  // New files only; a held package cannot be written, replaced or deleted.
  {
    const fs::path package = root / L"package.msi";
    const char bytes[] = "package";
    check(writeNewFile(package, bytes, DWORD(sizeof(bytes) - 1), nullptr), "a new package was not written");
    check(!writeNewFile(package, bytes, DWORD(sizeof(bytes) - 1), nullptr), "an existing package was overwritten");
    const HANDLE held = holdPackage(package);
    check(held != nullptr, "the package could not be held");
    const HANDLE writer = CreateFileW(package.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(writer == INVALID_HANDLE_VALUE, "the held package could be opened for writing");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    const fs::path other = root / L"other.msi"; { std::ofstream file(other, std::ios::binary); file << "other"; }
    check(!MoveFileExW(other.c_str(), package.c_str(), MOVEFILE_REPLACE_EXISTING) && !DeleteFileW(package.c_str()), "the held package was replaced or deleted");
    const HANDLE reader = CreateFileW(package.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(reader != INVALID_HANDLE_VALUE, "msiexec could not read the held package");
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    if (held) CloseHandle(held);
    check(read(package) == "package", "the package changed");
  }
  std::error_code ignored; fs::remove_all(root, ignored);
  std::printf("checks=%u failed=%u\n", checks, failed);
  return failed ? 1 : 0;
}
