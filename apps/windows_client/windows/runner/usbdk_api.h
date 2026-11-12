#pragma once
// The real UsbDk (https://github.com/daynix/UsbDk, Red Hat/Daynix, Apache-2.0)
// behind usb::Api: UsbDkHelper.dll from the UsbDk Runtime Library, loaded only
// after its Authenticode signature (Red Hat, Inc.) is verified, with the C
// interface and data layout of UsbDkHelper.h / UsbDkData.h (1.0.22). Wire
// semantics checked against UsbDkHelper and the driver's RedirectorStrategy:
// a control transfer's buffer is the 8-byte setup packet followed by the data
// and its result counts the data stage only; the driver's file handle is the
// OVERLAPPED owner.
//
// Not exercised against a real device in development (a test PC with UsbDk is
// needed); everything above this layer is tested with usb::Api substitutes.
#include "usb_capture_core.h"
#include <cfgmgr32.h>
#include <softpub.h>
#include <wintrust.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <limits>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace multitap::usb {
namespace usbdk {
// UsbDkData.h (1.0.22). The 64-bit pointer fields are plain 64-bit integers here.
struct DeviceId { WCHAR DeviceID[MAX_DEVICE_ID_LEN]; WCHAR InstanceID[MAX_DEVICE_ID_LEN]; };
#pragma pack(push, 1)
struct DeviceDescriptor { uint8_t bytes[18]; };
#pragma pack(pop)
struct DeviceInfo { DeviceId ID; ULONG64 FilterID; ULONG64 Port; ULONG64 Speed; DeviceDescriptor Descriptor; };
struct ConfigRequest { DeviceId ID; ULONG64 Index; };
struct GenResult { ULONG64 BytesTransferred; ULONG64 UsbdStatus; };
struct TransferRequest {
  ULONG64 EndpointAddress; ULONG64 Buffer; ULONG64 BufferLength; ULONG64 TransferType;
  ULONG64 IsochronousPacketsArraySize; ULONG64 IsochronousPacketsArray;
  GenResult Result; ULONG64 IsochronousResultsArray;
};
static_assert(sizeof(DeviceId) == 800 && sizeof(DeviceInfo) == 848 && sizeof(ConfigRequest) == 808 && sizeof(TransferRequest) == 72, "UsbDk data layout changed");
enum Transfer { Failure = 0, Success = 1, SuccessAsync = 2 };
enum Type { Control = 0, Bulk = 1, Interrupt = 2 };

inline std::wstring RuntimeDirectory() {
  PWSTR known = nullptr;
  std::wstring base;
  if (SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &known) == S_OK) { base = known; CoTaskMemFree(known); }
  return base.empty() ? base : base + L"\\UsbDk Runtime Library";
}
inline std::wstring HelperPath() { const auto directory = RuntimeDirectory(); return directory.empty() ? directory : directory + L"\\UsbDkHelper.dll"; }

// A valid Authenticode signature by "Red Hat, Inc." (UsbDk is signed with the
// project's vendor certificate, timestamped; its kernel driver additionally
// carries a Microsoft attestation signature).
inline bool SignedByRedHat(const std::wstring& path) {
  WINTRUST_FILE_INFO file{sizeof(file), path.c_str(), nullptr, nullptr};
  WINTRUST_DATA data{}; data.cbStruct = sizeof(data); data.dwUIChoice = WTD_UI_NONE; data.fdwRevocationChecks = WTD_REVOKE_NONE; data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file; data.dwStateAction = WTD_STATEACTION_VERIFY; data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result = WinVerifyTrust(nullptr, &action, &data);
  bool ok = false;
  if (result == ERROR_SUCCESS) {
    if (auto* provider = WTHelperProvDataFromStateData(data.hWVTStateData)) if (auto* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) if (auto* cert = WTHelperGetProvCertFromChain(signer, 0)) {
      wchar_t name[256]{};
      ok = CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256) && wcscmp(name, L"Red Hat, Inc.") == 0;
    }
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &data);
  return ok;
}
}  // namespace usbdk

// UsbDk frees its opaque redirector object on StopRedirect and can reuse that
// address on the next StartRedirect. A late worker must resolve its original
// identity, never mistake a replacement at the same native address for it.
// The owning UsbDkApi serializes every operation with nativeMutex_.
class UsbRedirectorIdentities {
 public:
  static constexpr uintptr_t maxIdentity = (std::numeric_limits<uintptr_t>::max)() - 1;
  explicit UsbRedirectorIdentities(uintptr_t lastIdentity = 0) : next_(lastIdentity) {}
  void* Add(HANDLE native) {
    if (!native || native == INVALID_HANDLE_VALUE || next_ >= maxIdentity) return nullptr;
    const uintptr_t identity = ++next_;
    live_.emplace(identity, native);
    return reinterpret_cast<void*>(identity);
  }
  HANDLE Find(void* identity) const {
    const auto found = live_.find(reinterpret_cast<uintptr_t>(identity));
    return found == live_.end() ? nullptr : found->second;
  }
  HANDLE Remove(void* identity) {
    const auto found = live_.find(reinterpret_cast<uintptr_t>(identity));
    if (found == live_.end()) return nullptr;
    const HANDLE native = found->second;
    live_.erase(found);
    return native;
  }
 private:
  uintptr_t next_ = 0;
  std::map<uintptr_t, HANDLE> live_;
};

// Tokens belong to one capture, not to the whole process. Keep the pending
// OVERLAPPED alive under this lock while cancellation uses it. Removing an
// old registration must not remove a different/replacement request.
class UsbPendingIo {
 public:
  bool Add(void* handle, int64_t token, OVERLAPPED* value) {
    std::lock_guard lock(mutex_);
    return pending_.emplace(Key(handle, token), value).second;
  }
  void Remove(void* handle, int64_t token, OVERLAPPED* value) {
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(Key(handle, token));
    if (found != pending_.end() && found->second == value) pending_.erase(found);
  }
  template <class F>
  bool Cancel(void* handle, int64_t token, F cancel) {
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(Key(handle, token));
    return found != pending_.end() && cancel(found->second);
  }
 private:
  static std::pair<uintptr_t, int64_t> Key(void* handle, int64_t token) { return {reinterpret_cast<uintptr_t>(handle), token}; }
  std::mutex mutex_;
  std::map<std::pair<uintptr_t, int64_t>, OVERLAPPED*> pending_;
};

class UsbDkApi final : public Api {
 public:
  // nullptr when UsbDk is not installed or the helper's signature is not Red Hat's.
  static std::unique_ptr<UsbDkApi> Load() {
    const auto path = usbdk::HelperPath();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES || !usbdk::SignedByRedHat(path)) return nullptr;
    // The helper loads its own driver files from its directory; nothing is searched on the path.
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return nullptr;
    std::unique_ptr<UsbDkApi> api(new UsbDkApi(module));
    return api->complete() ? std::move(api) : nullptr;
  }
  ~UsbDkApi() override { if (module_) FreeLibrary(module_); }

  bool Devices(std::vector<DeviceInfo>& out) override {
    out.clear();
    usbdk::DeviceInfo* list = nullptr; ULONG count = 0;
    if (!getDevices_(&list, &count)) return false;
    for (ULONG i = 0; list && i < count && i < 512; ++i) {
      DeviceInfo device;
      device.deviceId.assign(list[i].ID.DeviceID, wcsnlen(list[i].ID.DeviceID, MAX_DEVICE_ID_LEN));
      device.instanceId.assign(list[i].ID.InstanceID, wcsnlen(list[i].ID.InstanceID, MAX_DEVICE_ID_LEN));
      std::memcpy(device.descriptor, list[i].Descriptor.bytes, 18);
      device.speed = list[i].Speed;
      out.push_back(std::move(device));
    }
    if (list) releaseDevices_(list);
    return true;
  }
  bool ConfigDescriptor(const DeviceInfo& device, unsigned index, std::vector<uint8_t>& out) override {
    usbdk::ConfigRequest request{};
    Fill(request.ID, device);
    request.Index = index;
    uint8_t* descriptor = nullptr; ULONG length = 0;
    if (!getConfiguration_(&request, &descriptor, &length) || !descriptor) return false;
    out.assign(descriptor, descriptor + length);
    releaseConfiguration_(descriptor);
    return true;
  }
  void* Start(const DeviceInfo& device, DWORD& error) override {
    usbdk::DeviceId id{};
    Fill(id, device);
    SetLastError(0);
    HANDLE native = startRedirect_(&id);
    error = GetLastError();
    if (!native || native == INVALID_HANDLE_VALUE) return nullptr;
    std::lock_guard lock(nativeMutex_);
    void* identity = nullptr;
    try { identity = identities_.Add(native); }
    catch (...) { stopRedirect_(native); error = ERROR_NOT_ENOUGH_MEMORY; return nullptr; }
    if (!identity) { stopRedirect_(native); error = ERROR_TOO_MANY_OPEN_FILES; }
    return identity;
  }
  void Stop(void* handle) override {
    std::lock_guard lock(nativeMutex_);
    if (const HANDLE native = identities_.Remove(handle)) stopRedirect_(native);
  }
  DWORD Transfer(void* handle, uint8_t address, int type, std::vector<uint8_t>& buffer, uint32_t& usbd, size_t& actual, int64_t token) override {
    usbdk::TransferRequest request{};
    static uint8_t empty = 0;
    request.EndpointAddress = address;
    request.Buffer = reinterpret_cast<ULONG_PTR>(buffer.empty() ? &empty : buffer.data());
    request.BufferLength = buffer.size();
    request.TransferType = type == 0 ? usbdk::Control : type == 3 ? usbdk::Interrupt : usbdk::Bulk;
    const bool input = type == 0 ? !buffer.empty() && (buffer[0] & 0x80) : (address & 0x80) != 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return GetLastError();
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{overlapped.hEvent};
    // CancelRequest finds only this capture's pending I/O of [token].
    struct Registered {
      UsbDkApi* api; void* handle; int64_t token; OVERLAPPED* value;
      ~Registered() { api->pending_.Remove(handle, token, value); }
    } registered{this, handle, token, &overlapped};
    // The driver reports a failed transfer's USBD status in the request too.
    const auto report = [&] { usbd = uint32_t(request.Result.UsbdStatus); actual = size_t(request.Result.BytesTransferred); };
    HANDLE io = nullptr;
    int started = usbdk::Failure;
    DWORD submissionError = 0;
    {
      // StopRedirect frees UsbDk's opaque redirector object. Snapshot the
      // system HANDLE and submit while it is live, never across the wait.
      std::lock_guard lock(nativeMutex_);
      const HANDLE native = identities_.Find(handle);
      if (!native) return ERROR_DEVICE_NOT_CONNECTED;
      io = systemHandle_(native);
      if (!io || io == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
      if (!pending_.Add(handle, token, &overlapped)) return ERROR_INVALID_PARAMETER;
      SetLastError(0);
      started = input ? readPipe_(native, &request, &overlapped) : writePipe_(native, &request, &overlapped);
      submissionError = GetLastError();
    }
    if (started == usbdk::Failure) { report(); return submissionError ? submissionError : ERROR_GEN_FAILURE; }
    if (started == usbdk::SuccessAsync) {
      // Stop can close [io]. The event still belongs to this request: do not
      // free OVERLAPPED or its buffer before the actual I/O has completed.
      if (WaitForSingleObject(overlapped.hEvent, INFINITE) != WAIT_OBJECT_0) return GetLastError();
      DWORD bytes = 0;
      if (!GetOverlappedResult(io, &overlapped, &bytes, FALSE)) { const DWORD error = GetLastError(); report(); return error; }
    }
    report();
    return 0;
  }
  bool Abort(void* handle, uint8_t address) override {
    std::lock_guard lock(nativeMutex_);
    const HANDLE native = identities_.Find(handle);
    return native && abortPipe_(native, address) != FALSE;
  }
  bool CancelRequest(void* handle, int64_t token) override {
    std::lock_guard lock(nativeMutex_);
    const HANDLE native = identities_.Find(handle);
    if (!native) return false;
    return pending_.Cancel(handle, token, [&](OVERLAPPED* value) { return CancelIoEx(systemHandle_(native), value) != FALSE; });
  }
  int Returned(const DeviceInfo& device, bool wantChild, uint32_t& why) override {
    const std::wstring instance = device.deviceId + L"\\" + device.instanceId;
    DEVINST node = 0;
    why = kBackAbsent;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return 0;   // not (yet) in the tree
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS) return 0;
    why = problem != 0 ? kBackProblemBase + (std::min<ULONG>)(problem, 899) : kBackNotStarted;
    if (problem != 0 || (status & DN_STARTED) == 0) return 0;
    DEVINST child = 0;
    why = kBackNoChild;
    return wantChild && CM_Get_Child(&child, node, 0) != CR_SUCCESS ? 0 : 1;
  }
  bool Reset(void* handle, uint8_t address) override {
    std::lock_guard lock(nativeMutex_);
    const HANDLE native = identities_.Find(handle);
    return native && resetPipe_(native, address) != FALSE;
  }
  bool SetAltSetting(void* handle, unsigned interfaceIndex, unsigned alternate) override {
    std::lock_guard lock(nativeMutex_);
    const HANDLE native = identities_.Find(handle);
    return native && setAlt_(native, interfaceIndex, alternate) != FALSE;
  }

 private:
  explicit UsbDkApi(HMODULE module) : module_(module) {}
  template <class T> bool bind(T& target, const char* name) { target = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module_, name))); return target != nullptr; }
  bool complete() {
    return bind(getDevices_, "UsbDk_GetDevicesList") && bind(releaseDevices_, "UsbDk_ReleaseDevicesList") && bind(getConfiguration_, "UsbDk_GetConfigurationDescriptor") &&
        bind(releaseConfiguration_, "UsbDk_ReleaseConfigurationDescriptor") && bind(startRedirect_, "UsbDk_StartRedirect") && bind(stopRedirect_, "UsbDk_StopRedirect") &&
        bind(writePipe_, "UsbDk_WritePipe") && bind(readPipe_, "UsbDk_ReadPipe") && bind(abortPipe_, "UsbDk_AbortPipe") && bind(resetPipe_, "UsbDk_ResetPipe") &&
        bind(setAlt_, "UsbDk_SetAltsetting") && bind(systemHandle_, "UsbDk_GetRedirectorSystemHandle");
  }
  static void Fill(usbdk::DeviceId& id, const DeviceInfo& device) {
    wcsncpy_s(id.DeviceID, device.deviceId.c_str(), _TRUNCATE);
    wcsncpy_s(id.InstanceID, device.instanceId.c_str(), _TRUNCATE);
  }
  HMODULE module_ = nullptr;
  std::mutex nativeMutex_;
  UsbRedirectorIdentities identities_;
  UsbPendingIo pending_;
  BOOL (*getDevices_)(usbdk::DeviceInfo**, PULONG) = nullptr;
  void (*releaseDevices_)(usbdk::DeviceInfo*) = nullptr;
  BOOL (*getConfiguration_)(usbdk::ConfigRequest*, uint8_t**, PULONG) = nullptr;
  void (*releaseConfiguration_)(uint8_t*) = nullptr;
  HANDLE (*startRedirect_)(usbdk::DeviceId*) = nullptr;
  BOOL (*stopRedirect_)(HANDLE) = nullptr;
  int (*writePipe_)(HANDLE, usbdk::TransferRequest*, LPOVERLAPPED) = nullptr;
  int (*readPipe_)(HANDLE, usbdk::TransferRequest*, LPOVERLAPPED) = nullptr;
  BOOL (*abortPipe_)(HANDLE, ULONG64) = nullptr;
  BOOL (*resetPipe_)(HANDLE, ULONG64) = nullptr;
  BOOL (*setAlt_)(HANDLE, ULONG64, ULONG64) = nullptr;
  HANDLE (*systemHandle_)(HANDLE) = nullptr;
};
}  // namespace multitap::usb
