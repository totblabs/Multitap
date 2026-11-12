#pragma once
// drive-v1 method channel "multitap/drive" (docs/drive-redirection.md).
// pickFolder runs the folder picker on the platform thread; every file
// operation runs on a small worker pool whose threads impersonate the desktop
// user's ordinary token if this client is run elevated (it normally is not,
// since 2026-10-27), so the VM gets the rights of the person at the PC, never
// this process's administrator rights.
// Results are completed on the platform thread (completed_message).
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <windows.h>

#include <memory>

namespace multitap {

class ClientDriveChannel {
 public:
  static constexpr UINT completed_message = WM_APP + 190;
  explicit ClientDriveChannel(HWND reply_window);
  ~ClientDriveChannel();
  ClientDriveChannel(const ClientDriveChannel&) = delete;
  ClientDriveChannel& operator=(const ClientDriveChannel&) = delete;
  void Handle(const flutter::MethodCall<flutter::EncodableValue>& call,
              std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void Complete();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace multitap
