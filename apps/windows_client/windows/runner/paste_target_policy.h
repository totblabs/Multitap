#pragma once
// Plain rules of paste_target.h: which window a paste lands in, which tab is on screen, which names stay inside a
// folder. No window, COM or file system call here.
#include <optional>
#include <string_view>
#include <vector>

namespace multitap {

enum class PasteWindow { other, explorer, desktop };

// What a paste into the top-level window of class `root_class` lands in: a File Explorer window, the desktop (Progman
// or WorkerW holding the desktop's view), or something else (a file dialog, another application: nothing is claimed).
inline PasteWindow ClassifyPasteWindow(std::wstring_view root_class, bool has_desktop_view) {
  if (root_class == L"CabinetWClass" || root_class == L"ExploreWClass") return PasteWindow::explorer;
  if ((root_class == L"Progman" || root_class == L"WorkerW") && has_desktop_view) return PasteWindow::desktop;
  return PasteWindow::other;
}

// Of one Explorer window's views (one per tab), the one on screen: exactly one visible, otherwise none.
inline std::optional<size_t> ShownView(const std::vector<bool>& visible) {
  std::optional<size_t> shown;
  for (size_t i = 0; i < visible.size(); ++i) {
    if (!visible[i]) continue;
    if (shown) return std::nullopt;
    shown = i;
  }
  return shown;
}

// A name that stays directly inside a folder: not empty, no separator, drive, wildcard or parent reference.
inline bool PlainItemName(std::wstring_view name) {
  if (name.empty() || name.size() > 255 || name == L"." || name == L"..") return false;
  return name.find_first_of(L"\\/:*?\"<>|") == std::wstring_view::npos;
}

}  // namespace multitap
