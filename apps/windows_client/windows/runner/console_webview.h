#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include "console_webview_policy.h"

namespace multitap {
std::string ConsoleWebViewOwnerId(const std::string& code);
// A same-process browser console. It never receives the native login token.
class ConsoleWebViews {
 public:
  using Closed=std::function<void(const std::string&,const std::string&,const std::string&)>;
  ConsoleWebViews(HWND owner,Closed closed);
  ~ConsoleWebViews();
  // Empty = launched/focused, not proof of a connected or visible guest.
  // |dark| is the app's theme: the caption and the page follow it.
  std::string Open(ConsoleWebViewTarget target,bool dark);
  void SetDark(bool dark);
  void Close(const std::string& origin,const std::string& desktop_id);
  void CloseAll();
  void HideAll();
  void SetHuman(std::function<void()> callback);
  size_t Count()const;
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}
