#pragma once
// Worker (user-session) Windows glue of the Role::secure bridge. The capture loop
// finds the stream's owned DXGI output, asks the service for the SYSTEM helper
// (worker.secure.request), and maps the read-only frame section whose handles
// the service duplicated into this process (worker.secure.ready). The switch
// itself is secure_desktop_switch.hpp. The worker never launches anything, never
// sends input to the helper and touches no policy.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "json_object.hpp"
#include "machine.hpp"  // host::WinHandle
#include "secure_desktop_helper.hpp"
#include "secure_desktop_ipc.hpp"
#include "secure_desktop_scale.hpp"
#include "secure_desktop_switch.hpp"

namespace host::secure::worker {

// The integrity level of a token (2 medium, 3 high, 4 system; secure::integrityLevel),
// or secure::integrityUnknown.
inline uint32_t tokenLevel(HANDLE token) {
  DWORD size = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  if (!size || size > 1024) return integrityUnknown;
  std::vector<BYTE> bytes(size);
  if (!GetTokenInformation(token, TokenIntegrityLevel, bytes.data(), size, &size)) return integrityUnknown;
  const auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid;
  if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) return integrityUnknown;
  return integrityLevel(*GetSidSubAuthority(sid, DWORD(*GetSidSubAuthorityCount(sid) - 1)));
}
inline uint32_t processLevel(HANDLE process) {
  HANDLE raw = nullptr;
  if (!process || !OpenProcessToken(process, TOKEN_QUERY, &raw)) return integrityUnknown;
  WinHandle token(raw);
  return tokenLevel(token.value);
}
// The level of the process whose window is in front (what injected input goes
// to). 0 when no window is in front; integrityUnknown when its process cannot be
// read, which for a window of this session means it is above this process.
inline uint32_t foregroundLevel() {
  const HWND window = GetForegroundWindow();
  if (!window) return 0;
  DWORD pid = 0;
  if (!GetWindowThreadProcessId(window, &pid) || !pid) return integrityUnknown;
  if (pid == GetCurrentProcessId()) return processLevel(GetCurrentProcess());
  WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.value) return integrityUnknown;
  return processLevel(process.value);
}

// The attached DXGI output whose desktop rectangle is exactly the captured one.
inline std::optional<OwnedOutput> ownedOutputForRect(const RECT& rect) {
  using Microsoft::WRL::ComPtr;
  if (rect.right <= rect.left || rect.bottom <= rect.top) return {};
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
    DXGI_ADAPTER_DESC1 ad{};
    if (FAILED(adapter->GetDesc1(&ad))) continue;
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC od{};
      if (FAILED(output->GetDesc(&od)) || !od.AttachedToDesktop) continue;
      if (od.DesktopCoordinates.left == rect.left && od.DesktopCoordinates.top == rect.top &&
          od.DesktopCoordinates.right == rect.right && od.DesktopCoordinates.bottom == rect.bottom) {
        OwnedOutput owned;
        owned.adapterLow = ad.AdapterLuid.LowPart;
        owned.adapterHigh = ad.AdapterLuid.HighPart;
        owned.left = rect.left;
        owned.top = rect.top;
        owned.width = uint32_t(rect.right - rect.left);
        owned.height = uint32_t(rect.bottom - rect.top);
        if (!ipc::sizeValid(owned.width, owned.height)) return {};
        return owned;
      }
    }
  }
  return {};
}

inline std::string requestJson(const std::string& sessionId, uint32_t seq, const OwnedOutput& output) {
  return std::string("{\"type\":\"") + controlRequest + "\",\"sessionId\":" + jsonQuote(sessionId) + ",\"seq\":" + std::to_string(seq) +
         ",\"adapter\":{\"low\":" + std::to_string(output.adapterLow) + ",\"high\":" + std::to_string(output.adapterHigh) +
         "},\"rect\":{\"left\":" + std::to_string(output.left) + ",\"top\":" + std::to_string(output.top) +
         ",\"width\":" + std::to_string(output.width) + ",\"height\":" + std::to_string(output.height) + "}}";
}
inline std::string controlJson(const char* type, const std::string& sessionId) {
  return std::string("{\"type\":\"") + type + "\",\"sessionId\":" + jsonQuote(sessionId) + '}';
}

inline std::optional<uint32_t> controlSeq(const Json& message) {
  const auto* number = std::get_if<double>(&message.get("seq").value);
  if (!number || !std::isfinite(*number) || std::floor(*number) != *number || *number < 1 || *number > 2147483647.0) return {};
  return uint32_t(*number);
}

// What worker.secure.ready carries: two handles the service duplicated into THIS
// process (a read-only section, a wait-only event). They are owned from the
// moment the message is parsed, so a grant nobody uses closes them.
struct Grant {
  WinHandle section, event;
  uint32_t width = 0, height = 0, seq = 0;
};
// Only for a message from the worker's own service pipe: the handle values named
// in it are adopted (and closed with the grant).
inline std::optional<Grant> adoptGrant(const Json& message) {
  const auto handle = [&](const char* name) -> HANDLE {
    const auto* number = std::get_if<double>(&message.get(name).value);
    if (!number || !std::isfinite(*number) || std::floor(*number) != *number || *number < 4 || *number > 4294967295.0) return nullptr;
    return reinterpret_cast<HANDLE>(uintptr_t(*number));
  };
  Grant grant;
  grant.section = WinHandle(handle("section"));
  grant.event = WinHandle(handle("event"));
  const auto side = [&](const char* name) -> uint32_t {
    const auto* number = std::get_if<double>(&message.get(name).value);
    return number && std::isfinite(*number) && std::floor(*number) == *number && *number >= ipc::minSide && *number <= ipc::maxSide ? uint32_t(*number) : 0u;
  };
  grant.width = side("width");
  grant.height = side("height");
  const auto seq = controlSeq(message);
  if (!grant.section.value || !grant.event.value || !grant.width || !grant.height || !seq) return {};
  grant.seq = *seq;
  return grant;
}

// The mapped, read-only view of a grant's section and its frame event.
struct Mapping {
  WinHandle event;
  const uint8_t* view = nullptr;
  uint32_t width = 0, height = 0;
  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  ~Mapping() { if (view) UnmapViewOfFile(view); }
};
inline std::shared_ptr<Mapping> mapGrant(Grant&& grant) {
  if (!grant.section.value || !ipc::sizeValid(grant.width, grant.height)) return {};
  auto mapping = std::make_shared<Mapping>();
  // A section smaller than the frame it must hold fails here.
  mapping->view = static_cast<const uint8_t*>(MapViewOfFile(grant.section.value, FILE_MAP_READ, 0, 0, ipc::sectionBytes(grant.width, grant.height)));
  if (!mapping->view) return {};
  mapping->event = std::move(grant.event);
  mapping->width = grant.width;
  mapping->height = grant.height;
  return mapping;
}

}  // namespace host::secure::worker
