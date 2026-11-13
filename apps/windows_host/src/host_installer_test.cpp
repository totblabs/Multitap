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
  // The bar of the status window is the bytes written, in order, up to exactly the package's size.
  {
    const fs::path package = root / L"measured.msi";
    const std::vector<char> bytes(2 * 1024 * 1024 + 512 * 1024, 'm');
    std::vector<DWORD> seen; DWORD total = 0;
    check(writeNewFile(package, bytes.data(), DWORD(bytes.size()), nullptr, [&](DWORD done, DWORD size) { seen.push_back(done); total = size; }), "a measured package was not written");
    check(seen.size() == 3 && seen[0] == 1024 * 1024 && seen[1] == 2 * 1024 * 1024 && seen[2] == bytes.size() && total == bytes.size(), "the bytes written were not reported megabyte by megabyte up to the size");
    check(read(package).size() == bytes.size(), "the measured package is not complete");
    // A package that cannot be written reports nothing as written.
    seen.clear();
    check(!writeNewFile(package, bytes.data(), DWORD(bytes.size()), nullptr, [&](DWORD done, DWORD) { seen.push_back(done); }) && seen.empty(), "an unwritten package reported progress");
  }
  // The status window's bar: a measured fraction fills from the left and nothing else does;
  // without a measure a piece runs along the track and never leaves it.
  {
    StatusView view; int left = -1, right = -1;
    view.measured = true; view.fraction = 0.5; statusBarSpan(view, 400, 123456, left, right);
    check(left == 0 && right == 200, "half the bytes is half the bar");
    view.fraction = 0; statusBarSpan(view, 400, 999, left, right); check(left == 0 && right == 0, "no byte written is an empty bar");
    view.fraction = 7; statusBarSpan(view, 400, 0, left, right); check(left == 0 && right == 400, "the bar never passes its end");
    statusBarSpan(view, 400, 700, left, right); check(left == 0 && right == 400, "a measured bar moved with time");
    view.measured = false; bool inside = true, moved = false; int previous = -1;
    for (ULONGLONG tick = 0; tick < 1400; tick += 35) {
      statusBarSpan(view, 400, tick, left, right);
      inside = inside && left >= 0 && right <= 400 && left <= right;
      if (previous >= 0 && right != previous) moved = true;
      previous = right;
    }
    check(inside && moved, "the running piece left the track or stood still");
    // No window (service mode): every call is harmless and the state is still kept.
    StatusWindow none; none.stage(Text::StatusClosing); none.bytes(1, 4); none.hide();
    check(none.view().stage == Text::StatusClosing && none.view().measured && none.view().fraction == 0.25, "the status without a window lost its state");
    none.show(Text::StatusOpening);
    check(none.view().stage == Text::StatusOpening && !none.view().measured, "a new step kept the previous step's measure");
  }
  std::error_code ignored; fs::remove_all(root, ignored);
  // --render <folder>: the status window's picture of every step, in both languages, drawn off
  // screen into <folder>\host-installer-<language>-<step>.bmp. No window is shown.
  int count = 0; wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
  if (values && count == 3 && std::wstring(values[1]) == L"--render") {
    const fs::path out = values[2];
    const struct { Text stage; bool measured; double fraction; const wchar_t* name; } steps[] = {
      {Text::StatusUnpacking, true, 0.42, L"1-unpacking"}, {Text::StatusClosing, false, 0, L"2-closing"}, {Text::StatusStarting, false, 0, L"3-starting"},
      {Text::StatusFinishing, false, 0, L"4-finishing"}, {Text::StatusOpening, false, 0, L"5-opening"}};
    for (const auto language : {multitap::ui::Language::korean, multitap::ui::Language::english}) {
      multitap::ui::forceLanguageForTest(language);
      for (const auto& step : steps) {
        const UINT dpi = 144; const int width = MulDiv(kStatusWidth, dpi, 96), height = MulDiv(kStatusHeight, dpi, 96);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr; const HDC dc = CreateCompatibleDC(nullptr); const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const HGDIOBJ before = SelectObject(dc, bitmap);
        StatusView view; view.stage = step.stage; view.measured = step.measured; view.fraction = step.fraction;
        paintStatus(dc, RECT{0, 0, width, height}, view, dpi, 500);
        GdiFlush();
        BITMAPFILEHEADER file{}; file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader); file.bfSize = file.bfOffBits + DWORD(width) * DWORD(height) * 4;
        std::ofstream image(out / (std::wstring(L"host-installer-") + (language == multitap::ui::Language::korean ? L"ko-" : L"en-") + step.name + L".bmp"), std::ios::binary);
        image.write(reinterpret_cast<const char*>(&file), sizeof(file)); image.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        image.write(static_cast<const char*>(pixels), std::streamsize(width) * height * 4);
        check(image.good(), "a status picture was not written");
        SelectObject(dc, before); DeleteObject(bitmap); DeleteDC(dc);
      }
    }
    multitap::ui::forceLanguageForTest(std::nullopt);
  }
  // --window <folder>: the real status window for about a second (asked for by hand only; the
  // ordinary run shows nothing). The work runs on its own thread as an install does, and the
  // window must answer Windows the whole time: every step asks it and gets an answer at once.
  // Its picture as Windows draws it (title bar included) goes to <folder>\host-installer-window.bmp.
  if (values && count == 3 && std::wstring(values[1]) == L"--window") {
    const fs::path out = values[2];
    StatusWindow status; statusWindow = &status;
    bool answered = true, pictured = false;
    const int result = status.run([&]() -> int {
      const auto answers = [&] {
        const HWND window = FindWindowW(L"MultitapHostInstallerStatus", nullptr); DWORD_PTR ignored = 0;
        answered = answered && window && IsWindowVisible(window) && SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 500, &ignored) != 0;
      };
      status.stage(Text::StatusUnpacking);
      for (DWORD done = 0; done <= 40; ++done) { status.bytes(done, 40); Sleep(10); }
      answers();
      status.stage(Text::StatusClosing); Sleep(250); answers();
      status.hide();
      { const HWND window = FindWindowW(L"MultitapHostInstallerStatus", nullptr); answered = answered && window && !IsWindowVisible(window); }
      status.show(Text::StatusOpening); Sleep(250); answers();
      if (const HWND window = FindWindowW(L"MultitapHostInstallerStatus", nullptr)) {
        RECT frame{}; GetWindowRect(window, &frame); const int width = frame.right - frame.left, height = frame.bottom - frame.top;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr; const HDC dc = CreateCompatibleDC(nullptr); const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const HGDIOBJ before = SelectObject(dc, bitmap);
        if (PrintWindow(window, dc, 2 /* PW_RENDERFULLCONTENT */)) {
          GdiFlush();
          BITMAPFILEHEADER file{}; file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader); file.bfSize = file.bfOffBits + DWORD(width) * DWORD(height) * 4;
          std::ofstream image(out / L"host-installer-window.bmp", std::ios::binary);
          image.write(reinterpret_cast<const char*>(&file), sizeof(file)); image.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
          image.write(static_cast<const char*>(pixels), std::streamsize(width) * height * 4);
          pictured = image.good();
        }
        SelectObject(dc, before); DeleteObject(bitmap); DeleteDC(dc);
      }
      return 7;
    });
    statusWindow = nullptr;
    check(result == 7, "the status window did not return what the work returned");
    check(answered, "the status window did not answer while the work ran, or was not shown and hidden when asked");
    check(pictured, "the status window's picture was not written");
    check(FindWindowW(L"MultitapHostInstallerStatus", nullptr) == nullptr, "the status window stayed after the work ended");
  }
  if (values) LocalFree(values);
  std::printf("checks=%u failed=%u\n", checks, failed);
  return failed ? 1 : 0;
}
