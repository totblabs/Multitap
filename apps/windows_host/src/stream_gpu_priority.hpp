#pragma once
#include <windows.h>
#include <bcrypt.h>  // NTSTATUS (winternl.h clashes with ntsecapi.h in the product)
#include <d3dkmthk.h>
#include <cstdint>
#include <functional>
#include <optional>

namespace host {
inline bool gpuStatusOk(NTSTATUS status) { return status >= 0; }
// GPU scheduling of the streaming worker while its stream runs. A game that
// fills the GPU (a test PC, League of Legends on an RTX 5080:
// 47-58 frames captured but 16-52 encoded a second, encodeMaxUs up to 55 ms)
// is scheduled at the process class NORMAL like this worker, so the capture
// copy and the encoder wait behind its frames. The approach is Sunshine's
// (src/platform/windows/display_base.cpp, GPLv3): the process GPU scheduling
// class (D3DKMTSetProcessSchedulingPriorityClass) and the capture device's GPU
// thread priority 7 (IDXGIDevice::SetGPUThreadPriority, media.cpp).
//
// Class: REALTIME without hardware-accelerated GPU scheduling, HIGH with it or
// when it cannot be read. Sunshine's own default keeps REALTIME with HAGS too,
// but documents an NVIDIA driver bug where REALTIME "can cause unrecoverable
// encoding freeze or outright driver crash", more often with HAGS, in DX12
// games or near full VRAM, and offers HIGH for it; this takes the safer one.
// REALTIME needs SeIncreaseBasePriorityPrivilege (an elevated token holds it,
// disabled; it is enabled here): when it is refused, HIGH is tried. A class
// already at or above the wanted one is left alone. Nothing here is required:
// every failure leaves the stream at its inherited scheduling. No adapter
// (WARP or GDI capture on a GPU-less VM) changes nothing. The previous class
// comes back when the stream ends (the worker process ends with its session).
struct GpuPriorityAccess {
  std::function<std::optional<bool>(uint64_t)> hags;  // HwSchEnabled of the adapter; nullopt: unknown
  std::function<bool()> privilege;                     // SeIncreaseBasePriorityPrivilege enabled on this process
  std::function<std::optional<int>()> get;             // this process's class now
  std::function<bool(int)> set;
};
inline std::optional<bool> adapterHardwareScheduling(uint64_t luid) {
  const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  if (!gdi) return std::nullopt;
  const auto open = reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
  const auto query = reinterpret_cast<PFND3DKMT_QUERYADAPTERINFO>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
  const auto close = reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
  if (!open || !query || !close) return std::nullopt;
  D3DKMT_OPENADAPTERFROMLUID adapter{};
  adapter.AdapterLuid.LowPart = DWORD(luid); adapter.AdapterLuid.HighPart = LONG(uint32_t(luid >> 32));
  if (!gpuStatusOk(open(&adapter))) return std::nullopt;
  D3DKMT_WDDM_2_7_CAPS caps{}; D3DKMT_QUERYADAPTERINFO info{};
  info.hAdapter = adapter.hAdapter; info.Type = KMTQAITYPE_WDDM_2_7_CAPS; info.pPrivateDriverData = &caps; info.PrivateDriverDataSize = sizeof(caps);
  const bool read = gpuStatusOk(query(&info));
  D3DKMT_CLOSEADAPTER closing{}; closing.hAdapter = adapter.hAdapter; close(&closing);
  if (!read) return std::nullopt;
  return caps.HwSchEnabled != 0;
}
inline bool enableIncreaseBasePriority() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
  TOKEN_PRIVILEGES privileges{}; privileges.PrivilegeCount = 1; privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  bool enabled = LookupPrivilegeValueW(nullptr, L"SeIncreaseBasePriorityPrivilege", &privileges.Privileges[0].Luid) &&
                 AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return enabled;
}
inline GpuPriorityAccess windowsGpuPriorityAccess() {
  GpuPriorityAccess access;
  access.hags = adapterHardwareScheduling;
  access.privilege = enableIncreaseBasePriority;
  const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  const auto get = gdi ? reinterpret_cast<NTSTATUS(APIENTRY*)(HANDLE, D3DKMT_SCHEDULINGPRIORITYCLASS*)>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass")) : nullptr;
  const auto set = gdi ? reinterpret_cast<NTSTATUS(APIENTRY*)(HANDLE, D3DKMT_SCHEDULINGPRIORITYCLASS)>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass")) : nullptr;
  access.get = [get]() -> std::optional<int> { D3DKMT_SCHEDULINGPRIORITYCLASS value{}; if (!get || !gpuStatusOk(get(GetCurrentProcess(), &value))) return std::nullopt; return int(value); };
  access.set = [set](int value) { return set && gpuStatusOk(set(GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS(value))); };
  return access;
}
class StreamGpuPriority {
 public:
  static constexpr int normal = D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL, high = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH, realtime = D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
  explicit StreamGpuPriority(std::optional<uint64_t> adapter, GpuPriorityAccess access = windowsGpuPriorityAccess()) : access_(std::move(access)) {
    if (!adapter) return;
    attempted_ = true;
    try {
      hags_ = access_.hags ? access_.hags(*adapter) : std::nullopt;
      privilege_ = access_.privilege && access_.privilege();
      previous_ = access_.get ? access_.get() : std::nullopt;
      wanted_ = hags_ == false ? realtime : high;
      if (previous_ && *previous_ >= wanted_) return;  // already there (or above): nothing to undo
      if (access_.set && access_.set(wanted_)) applied_ = wanted_;
      else if (wanted_ == realtime && access_.set && access_.set(high)) applied_ = high;
    } catch (...) { applied_.reset(); }
  }
  ~StreamGpuPriority() {
    try { if (applied_ && previous_ && access_.get && access_.get() == applied_ && access_.set) access_.set(*previous_); } catch (...) { }
  }
  StreamGpuPriority(const StreamGpuPriority&) = delete;
  StreamGpuPriority& operator=(const StreamGpuPriority&) = delete;
  bool attempted() const { return attempted_; }
  std::optional<int> applied() const { return applied_; }
  // Fixed diagnostic (hostAction 200 stage 242): bits 0-3 the wanted class,
  // 4-7 the class applied (15: none; also when already at or above it),
  // 8 HAGS readable, 9 HAGS on, 10 privilege enabled, 11 the capture device's
  // GPU thread priority raised (`thread`), 12 the class was already high enough.
  uint32_t diagnostic(bool thread) const {
    if (!attempted_) return 0;
    const bool already = previous_ && *previous_ >= wanted_;
    return uint32_t(wanted_ & 15) | (uint32_t(applied_ ? *applied_ & 15 : 15) << 4) | (hags_ ? 0x100u : 0u) | (hags_ == true ? 0x200u : 0u) |
           (privilege_ ? 0x400u : 0u) | (thread ? 0x800u : 0u) | (already ? 0x1000u : 0u);
  }
 private:
  GpuPriorityAccess access_;
  bool attempted_ = false, privilege_ = false;
  std::optional<bool> hags_;
  std::optional<int> previous_, applied_;
  int wanted_ = high;
};
}
