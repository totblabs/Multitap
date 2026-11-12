#pragma once
// The real device tree behind usb_describe_core.h: Windows' own names,
// arrival times, child devices (disks, network adapters, keyboards), the volumes
// of a USB disk with their labels and drive letters, and the notifications that
// say the list changed. Read-only except for CheckHandOver(), which locks the
// volumes of one USB disk to see whether files are open (and releases them).
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "usb_capture_core.h"
#include "usb_describe_core.h"

namespace multitap::usb {

// One RawDevice per entry of [devices] (same order).
std::vector<RawDevice> ReadTopology(const std::vector<Listed>& devices);

// The volumes of the disks of one USB device, fresh from Windows.
std::vector<std::wstring> VolumePathsOf(const std::wstring& deviceId, const std::wstring& instanceId);

// The real VolumeApi: opens a volume, locks it (FSCTL_LOCK_VOLUME), flushes and
// dismounts it, closing the handle unlocks.
std::unique_ptr<VolumeApi> MakeVolumeApi();

// Calls [changed] (from a Windows thread) when a USB device, disk or volume
// arrives or leaves. Destroying it unregisters.
class ChangeWatcher {
 public:
  explicit ChangeWatcher(std::function<void()> changed);
  ~ChangeWatcher();
  ChangeWatcher(const ChangeWatcher&) = delete;
  ChangeWatcher& operator=(const ChangeWatcher&) = delete;
 private:
  struct State; std::unique_ptr<State> state_;
};

}  // namespace multitap::usb
