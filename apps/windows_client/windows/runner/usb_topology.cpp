#include "usb_topology.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <winioctl.h>
#pragma comment(lib, "cfgmgr32.lib")

namespace multitap::usb {
namespace {
const GUID kUsbDeviceInterface = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
const GUID kDiskInterface = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
const GUID kVolumeInterface = {0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

std::vector<uint8_t> Property(DEVINST node, const DEVPROPKEY& key, DEVPROPTYPE& type) {
  ULONG size = 0;
  type = 0;
  if (CM_Get_DevNode_PropertyW(node, &key, &type, nullptr, &size, 0) != CR_BUFFER_SMALL || size == 0 || size > 4096) return {};
  std::vector<uint8_t> buffer(size);
  if (CM_Get_DevNode_PropertyW(node, &key, &type, buffer.data(), &size, 0) != CR_SUCCESS) return {};
  buffer.resize(size);
  return buffer;
}
std::wstring StringProperty(DEVINST node, const DEVPROPKEY& key) {
  DEVPROPTYPE type = 0;
  const auto bytes = Property(node, key, type);
  if (type != DEVPROP_TYPE_STRING || bytes.size() < 2) return {};
  std::wstring text(reinterpret_cast<const wchar_t*>(bytes.data()), bytes.size() / 2);
  while (!text.empty() && text.back() == L'\0') text.pop_back();
  return text;
}

struct VolumeInfo {
  DWORD disk = 0; std::wstring path, label, fileSystem, letters; uint64_t bytes = 0;
};

// Every volume that lives on a disk (not optical drives), with its label,
// drive letters. Device discovery never inspects certificate folders.
std::vector<VolumeInfo> EnumerateVolumes() {
  std::vector<VolumeInfo> out;
  DWORD previousMode = 0;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previousMode);   // no "insert a disk" dialogs
  wchar_t name[MAX_PATH]{};
  HANDLE find = FindFirstVolumeW(name, MAX_PATH);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      std::wstring root = name;   // the volume GUID path, ending in a backslash
      if (root.size() < 5 || root.back() != L'\\') continue;
      HANDLE handle = CreateFileW(root.substr(0, root.size() - 1).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (handle == INVALID_HANDLE_VALUE) continue;
      STORAGE_DEVICE_NUMBER number{}; DWORD got = 0;
      const bool ok = DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof(number), &got, nullptr) != FALSE;
      CloseHandle(handle);
      if (!ok || number.DeviceType != FILE_DEVICE_DISK) continue;
      VolumeInfo info; info.disk = number.DeviceNumber; info.path = root;
      wchar_t label[MAX_PATH + 1]{}, system[MAX_PATH + 1]{};
      if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, system, MAX_PATH)) { info.label = label; info.fileSystem = system; }
      ULARGE_INTEGER total{};
      if (GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, nullptr)) info.bytes = total.QuadPart;
      DWORD needed = 0;
      GetVolumePathNamesForVolumeNameW(root.c_str(), nullptr, 0, &needed);
      if (needed > 1 && needed < 4096) {
        std::vector<wchar_t> paths(needed);
        if (GetVolumePathNamesForVolumeNameW(root.c_str(), paths.data(), needed, &needed)) {
          for (const wchar_t* at = paths.data(); *at; at += wcslen(at) + 1) {
            if (wcslen(at) == 3 && at[1] == L':' && at[2] == L'\\') info.letters += wchar_t(towupper(at[0]));
          }
        }
      }
      out.push_back(std::move(info));
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
  }
  SetThreadErrorMode(previousMode, nullptr);
  return out;
}

void Collect(DEVINST node, int depth, std::set<std::wstring>& classes, std::vector<DEVINST>& disks) {
  DEVINST child = 0;
  if (CM_Get_Child(&child, node, 0) != CR_SUCCESS) return;
  for (int guard = 0; guard < 64; ++guard) {
    const auto cls = StringProperty(child, DEVPKEY_Device_Class);
    if (!cls.empty()) classes.insert(cls);
    if (cls == L"DiskDrive") disks.push_back(child);
    if (depth < 6) Collect(child, depth + 1, classes, disks);
    DEVINST next = 0;
    if (CM_Get_Sibling(&next, child, 0) != CR_SUCCESS) break;
    child = next;
  }
}

// The disk number (and size, when Windows allows reading it) of a disk device.
bool DiskNumberOf(DEVINST disk, DWORD& number, uint64_t& bytes) {
  wchar_t id[MAX_DEVICE_ID_LEN]{};
  if (CM_Get_Device_IDW(disk, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
  ULONG length = 0;
  if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<GUID*>(&kDiskInterface), id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length < 2 || length > 4096) return false;
  std::vector<wchar_t> list(length);
  if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&kDiskInterface), id, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || !list[0]) return false;
  HANDLE handle = CreateFileW(list.data(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  STORAGE_DEVICE_NUMBER value{}; DWORD got = 0;
  const bool ok = DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &value, sizeof(value), &got, nullptr) != FALSE;
  if (ok) {
    number = value.DeviceNumber;
    GET_LENGTH_INFORMATION size{};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &size, sizeof(size), &got, nullptr)) bytes = uint64_t(size.Length.QuadPart);
  }
  CloseHandle(handle);
  return ok;
}

std::vector<RawDisk> DisksOf(const std::vector<DEVINST>& nodes, const std::vector<VolumeInfo>& volumes) {
  std::vector<RawDisk> out;
  for (const auto node : nodes) {
    RawDisk disk;
    disk.name = StringProperty(node, DEVPKEY_Device_FriendlyName);
    if (disk.name.empty()) disk.name = StringProperty(node, DEVPKEY_Device_DeviceDesc);
    DWORD number = 0;
    if (DiskNumberOf(node, number, disk.bytes)) {
      for (const auto& volume : volumes) {
        if (volume.disk != number) continue;
        RawVolume item; item.path = volume.path; item.label = volume.label; item.fileSystem = volume.fileSystem; item.letters = volume.letters;
        item.bytes = volume.bytes;
        disk.volumes.push_back(std::move(item));
      }
    }
    out.push_back(std::move(disk));
  }
  return out;
}

class RealVolumes final : public VolumeApi {
 public:
  void* Open(const std::wstring& path) override {
    if (path.size() < 5 || path.back() != L'\\') return nullptr;
    const std::wstring device = path.substr(0, path.size() - 1);
    HANDLE handle = CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_WRITE_PROTECT) {
      handle = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
  }
  LockResult Lock(void* volume) override {
    DWORD got = 0;
    if (DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr)) return LockResult::Locked;
    switch (GetLastError()) {
      case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_BUSY: case ERROR_LOCK_VIOLATION: return LockResult::InUse;
      default: return LockResult::Skipped;
    }
  }
  void Dismount(void* volume) override {
    DWORD got = 0;
    FlushFileBuffers(volume);
    DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
  }
  void Close(void* volume) override { CloseHandle(volume); }
};
}  // namespace

std::vector<RawDevice> ReadTopology(const std::vector<Listed>& devices) {
  std::vector<RawDevice> out;
  std::vector<VolumeInfo> volumes;
  bool volumesRead = false;
  for (const auto& listed : devices) {
    RawDevice raw;
    raw.deviceId = listed.deviceId; raw.instanceId = listed.instanceId; raw.descriptors = listed.descriptors; raw.capturedHere = listed.busy;
    const std::wstring instance = listed.deviceId + L"\\" + listed.instanceId;
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
      raw.busReportedDesc = StringProperty(node, DEVPKEY_Device_BusReportedDeviceDesc);
      raw.friendlyName = StringProperty(node, DEVPKEY_Device_FriendlyName);
      raw.deviceDesc = StringProperty(node, DEVPKEY_Device_DeviceDesc);
      DEVPROPTYPE type = 0;
      const auto address = Property(node, DEVPKEY_Device_Address, type);
      if (type == DEVPROP_TYPE_UINT32 && address.size() == 4) std::memcpy(&raw.port, address.data(), 4);
      const auto arrival = Property(node, DEVPKEY_Device_LastArrivalDate, type);
      if (type == DEVPROP_TYPE_FILETIME && arrival.size() == 8) {
        uint64_t ticks = 0; std::memcpy(&ticks, arrival.data(), 8);
        if (ticks > 116444736000000000ull) raw.arrivalMs = (ticks - 116444736000000000ull) / 10000ull;
      }
      std::vector<DEVINST> disks;
      Collect(node, 0, raw.childClasses, disks);
      if (!disks.empty()) {
        if (!volumesRead) { volumes = EnumerateVolumes(); volumesRead = true; }
        raw.disks = DisksOf(disks, volumes);
      }
    }
    out.push_back(std::move(raw));
  }
  return out;
}

std::vector<std::wstring> VolumePathsOf(const std::wstring& deviceId, const std::wstring& instanceId) {
  std::vector<std::wstring> out;
  const std::wstring instance = deviceId + L"\\" + instanceId;
  DEVINST node = 0;
  if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return out;
  std::set<std::wstring> classes; std::vector<DEVINST> disks;
  Collect(node, 0, classes, disks);
  if (disks.empty()) return out;
  for (const auto& disk : DisksOf(disks, EnumerateVolumes())) for (const auto& volume : disk.volumes) out.push_back(volume.path);
  return out;
}

std::unique_ptr<VolumeApi> MakeVolumeApi() { return std::make_unique<RealVolumes>(); }

struct ChangeWatcher::State {
  std::function<void()> changed;
  HCMNOTIFICATION handles[3]{};
};

ChangeWatcher::ChangeWatcher(std::function<void()> changed) : state_(std::make_unique<State>()) {
  state_->changed = std::move(changed);
  const GUID* guids[3] = {&kUsbDeviceInterface, &kDiskInterface, &kVolumeInterface};
  for (int i = 0; i < 3; ++i) {
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = *guids[i];
    CM_Register_Notification(&filter, state_.get(), [](HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) -> DWORD {
      if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        auto* state = static_cast<State*>(context);
        if (state->changed) state->changed();
      }
      return ERROR_SUCCESS;
    }, &state_->handles[i]);
  }
}

ChangeWatcher::~ChangeWatcher() {
  for (auto& handle : state_->handles) if (handle) CM_Unregister_Notification(handle);   // waits for callbacks in flight
}
}  // namespace multitap::usb
