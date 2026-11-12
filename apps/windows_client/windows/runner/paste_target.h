#pragma once
// Where a paste on this PC goes (lib/data/paste_target.dart). Explorer writes pasted virtual files itself and never
// tells the source the folder. The session asks which folder the File Explorer window (or the desktop) in front shows
// as the paste starts, and claims it only after one of the pasted items was then created there (checked in Dart).
// Reading a shell view and opening a folder are COM calls into Explorer: they run on a worker thread and answer on
// the window's thread.
#include <windows.h>

#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_result.h>

#include <memory>
#include <optional>
#include <string>

#include "paste_target_policy.h"

namespace multitap {

struct PasteFolder {
  std::wstring path, name;  // the folder on disk, and the name Explorer shows for it
};

// The folder shown by the File Explorer window or desktop that `window` belongs to; nothing for any other window, an
// ambiguous view or a view that is no folder on disk (This PC, a library, a zip). COM must be initialized.
std::optional<PasteFolder> ShellFolderOf(HWND window);

// Opens `folder` in File Explorer with `select` (an item directly in it) selected, or the folder alone when that item
// is gone. False for anything that is not an existing folder. COM must be initialized.
bool OpenShellFolder(const std::wstring& folder, const std::wstring& select);

// The runner's `multitap/paste-target` channel: `capture` (the folder in front now, or null) and `open`.
class PasteTargetChannel {
 public:
  static constexpr UINT completed_message = WM_APP + 196;
  explicit PasteTargetChannel(HWND window);
  ~PasteTargetChannel();
  PasteTargetChannel(const PasteTargetChannel&) = delete;
  PasteTargetChannel& operator=(const PasteTargetChannel&) = delete;

  void Handle(const flutter::MethodCall<flutter::EncodableValue>& call,
              std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  // On the window's thread after completed_message: answers what the workers finished.
  void Complete();

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace multitap
