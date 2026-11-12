// The rules of where a paste on this PC goes (runner/paste_target_policy.h): only a File Explorer window or the
// desktop is ever named, only the tab on screen, and only names that stay inside the folder are looked up. No
// window, COM or file system call.
#include <cstdio>

#include "paste_target_policy.h"

using namespace multitap;

namespace {
int failures = 0;
void Check(bool ok, const char* what, int line) {
  if (ok) return;
  ++failures;
  std::printf("FAIL line %d: %s\n", line, what);
}
#define CHECK(x) Check((x), #x, __LINE__)
}  // namespace

int main() {
  CHECK(ClassifyPasteWindow(L"CabinetWClass", false) == PasteWindow::explorer);
  CHECK(ClassifyPasteWindow(L"ExploreWClass", false) == PasteWindow::explorer);
  CHECK(ClassifyPasteWindow(L"Progman", true) == PasteWindow::desktop);
  CHECK(ClassifyPasteWindow(L"WorkerW", true) == PasteWindow::desktop);
  // A wallpaper WorkerW without the desktop's view, a file dialog, this application, a mail client: nothing.
  CHECK(ClassifyPasteWindow(L"WorkerW", false) == PasteWindow::other);
  CHECK(ClassifyPasteWindow(L"#32770", true) == PasteWindow::other);
  CHECK(ClassifyPasteWindow(L"FLUTTER_RUNNER_WIN32_WINDOW", false) == PasteWindow::other);
  CHECK(ClassifyPasteWindow(L"rctrl_renwnd32", false) == PasteWindow::other);
  CHECK(ClassifyPasteWindow(L"", false) == PasteWindow::other);

  // Tabs: the one shown, never a guess between two or among none.
  CHECK(!ShownView({}).has_value());
  CHECK(ShownView({true}) == size_t(0));
  CHECK(ShownView({false, false, true}) == size_t(2));
  CHECK(!ShownView({false, false}).has_value());
  CHECK(!ShownView({true, false, true}).has_value());

  CHECK(PlainItemName(L"사진"));
  CHECK(PlainItemName(L"report final.pdf"));
  CHECK(!PlainItemName(L""));
  CHECK(!PlainItemName(L"."));
  CHECK(!PlainItemName(L".."));
  CHECK(!PlainItemName(L"a\\b"));
  CHECK(!PlainItemName(L"a/b"));
  CHECK(!PlainItemName(L"C:x"));
  CHECK(!PlainItemName(L"*.txt"));

  std::printf(failures ? "paste target FAILED (%d)\n" : "paste target OK\n", failures);
  return failures ? 1 : 0;
}
