// FramePacer event wake (cap respected, early wake, plain deadline),
// CaptureTopologyWatch (failure reaches the capture thread with the check's
// message, clean stop), StreamPriority (raise, reference count, restore) and
// the capture session's update interval on a real Windows Graphics Capture of
// this test's own window. Timing bounds are generous: run alone, the machine
// may be shared.
#include "frame_pacer.hpp"
#include "capture_topology_watch.hpp"
#include "capture_session_interval.hpp"
#include "stream_priority.hpp"
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
int checks = 0;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); ++checks; }
double ms(Clock::duration value) { return std::chrono::duration<double, std::milli>(value).count(); }
struct Event { HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr); ~Event() { CloseHandle(handle); } };

void pacer() {
  host::FramePacer pacer; Event frame; const HANDLE events[] = {frame.handle};
  // No frame: the plain deadline.
  auto began = Clock::now();
  require(!pacer.wait(began + std::chrono::milliseconds(30), events, began), "Woke without a frame");
  require(ms(Clock::now() - began) >= 29.5, "Returned before the deadline");
  // A pending frame and no cap: at once, and the auto-reset event is consumed.
  SetEvent(frame.handle); began = Clock::now();
  require(pacer.wait(began + std::chrono::milliseconds(200), events, began), "Pending frame did not wake");
  require(ms(Clock::now() - began) < 100, "Pending frame woke late");
  require(WaitForSingleObject(frame.handle, 0) == WAIT_TIMEOUT, "Frame event was not consumed");
  // A pending frame before the cap waits for the cap, not the deadline.
  SetEvent(frame.handle); began = Clock::now();
  require(pacer.wait(began + std::chrono::milliseconds(300), events, began + std::chrono::milliseconds(40)), "Capped frame did not wake");
  const double capped = ms(Clock::now() - began);
  require(capped >= 39.5 && capped < 250, "Capped frame ignored the cap or waited for the deadline");
  // A frame arriving later wakes at its arrival.
  began = Clock::now();
  std::thread later([&] { Sleep(40); SetEvent(frame.handle); });
  require(pacer.wait(began + std::chrono::milliseconds(400), events, began), "Arriving frame did not wake");
  const double arrived = ms(Clock::now() - began); later.join();
  require(arrived >= 35 && arrived < 300, "Arriving frame woke at the wrong time");
  // An input wake (second event, as the capture loops pass it) ends an idle wait
  // at once, still not before the frame-rate cap, and is consumed.
  {
    Event wake; const HANDLE both[] = {frame.handle, wake.handle};
    began = Clock::now();
    std::thread input([&] { Sleep(10); SetEvent(wake.handle); });
    require(pacer.wait(began + std::chrono::milliseconds(400), both, began), "Input wake did not end the idle wait");
    input.join();
    require(ms(Clock::now() - began) < 100, "Input wake woke late");
    require(WaitForSingleObject(wake.handle, 0) == WAIT_TIMEOUT, "Input wake was not consumed");
    SetEvent(wake.handle); began = Clock::now();
    require(pacer.wait(began + std::chrono::milliseconds(300), both, began + std::chrono::milliseconds(40)), "Capped input wake did not wake");
    require(ms(Clock::now() - began) >= 39.5, "Input wake ignored the frame-rate cap");
  }
  // No events: exactly the plain wait.
  began = Clock::now();
  require(!pacer.wait(began + std::chrono::milliseconds(20), std::span<const HANDLE>(), began), "Plain wait reported a frame");
  require(ms(Clock::now() - began) >= 19.5, "Plain wait returned early");
  require(host::captureMinimumGap(std::chrono::microseconds(16666)) == std::chrono::microseconds(14999), "Minimum gap is not 90 % of the period");
}

void watch() {
  std::atomic<int> calls{0}; std::atomic<bool> broken{false};
  {
    host::CaptureTopologyWatch topology([&] { ++calls; if (broken) throw std::runtime_error("Selected virtual monitor changed; reconnect the session"); }, std::chrono::milliseconds(10));
    Sleep(60); topology.verify();
    require(calls.load() >= 2, "Watch did not re-run its check");
    broken = true;
    const auto began = Clock::now(); bool thrown = false; std::string message;
    while (!thrown && Clock::now() - began < std::chrono::seconds(5)) { try { topology.verify(); Sleep(2); } catch (const std::exception& error) { thrown = true; message = error.what(); } }
    require(thrown && message == "Selected virtual monitor changed; reconnect the session", "Failure did not reach the capture thread with its message");
    const int after = calls.load(); Sleep(40); require(calls.load() == after, "Watch kept checking after a failure");
  }
  // Destruction stops a healthy watch promptly.
  const auto began = Clock::now();
  { host::CaptureTopologyWatch topology([] {}, std::chrono::seconds(30)); Sleep(5); }
  require(ms(Clock::now() - began) < 2000, "Watch did not stop promptly");
}

void priority() {
  const DWORD before = GetPriorityClass(GetCurrentProcess());
  require(before == NORMAL_PRIORITY_CLASS, "Run this test at normal priority");
  {
    host::StreamPriority first;
    require(GetPriorityClass(GetCurrentProcess()) == ABOVE_NORMAL_PRIORITY_CLASS && host::StreamPriority::raised(), "Streaming did not raise the process");
    { host::StreamPriority second; }
    require(GetPriorityClass(GetCurrentProcess()) == ABOVE_NORMAL_PRIORITY_CLASS, "An inner stream end restored the class early");
    PROCESS_POWER_THROTTLING_STATE state{}; state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)))
      require((state.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) && !(state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED), "Streaming did not opt out of EcoQoS");
  }
  require(GetPriorityClass(GetCurrentProcess()) == before && !host::StreamPriority::raised(), "The previous class was not restored");
  // A process already above normal (e.g. started high) is left alone.
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  { host::StreamPriority stream; require(GetPriorityClass(GetCurrentProcess()) == HIGH_PRIORITY_CLASS, "A higher class was lowered"); }
  require(GetPriorityClass(GetCurrentProcess()) == HIGH_PRIORITY_CLASS, "A higher class was changed on restore");
  SetPriorityClass(GetCurrentProcess(), before);
}

// A real WGC session of this test's own small window: where the property
// exists the session is set to deliver every composed frame (1 ms) and still
// delivers frames; without it nothing is changed. Returns the OS default in ms
// (-1 without the property, -2 without WGC).
std::atomic<int> paintCount{0};
LRESULT CALLBACK ownWindow(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_PAINT) { PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); HBRUSH brush = CreateSolidBrush(RGB(paintCount.load() & 255, 90, 140)); RECT area{}; GetClientRect(window, &area); FillRect(dc, &area, brush); DeleteObject(brush); EndPaint(window, &paint); return 0; }
  return DefWindowProcW(window, message, w, l);
}
double sessionInterval() {
  using namespace winrt::Windows::Graphics::Capture;
  if (!GraphicsCaptureSession::IsSupported()) return -2;
  std::atomic<HWND> window{nullptr};
  std::thread ui([&] {
    WNDCLASSW type{}; type.lpfnWndProc = ownWindow; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MultitapCapturePacingTest"; RegisterClassW(&type);
    // Bottom-right corner, never activated, click-through, almost transparent.
    HWND created = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, type.lpszClassName, L"Multitap capture pacing test", WS_POPUP, GetSystemMetrics(SM_CXSCREEN) - 48, GetSystemMetrics(SM_CYSCREEN) - 48, 48, 48, nullptr, nullptr, type.hInstance, nullptr);
    SetLayeredWindowAttributes(created, 0, 1, LWA_ALPHA); ShowWindow(created, SW_SHOWNOACTIVATE); UpdateWindow(created); window = created;
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  });
  while (!window.load()) Sleep(1);
  struct Stop { std::thread& ui; HWND window; ~Stop() { PostMessageW(window, WM_CLOSE, 0, 0); PostThreadMessageW(GetThreadId(ui.native_handle()), WM_QUIT, 0, 0); ui.join(); } } stop{ui, window.load()};
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)))
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "No D3D11 device");
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; require(SUCCEEDED(device.As(&dxgi)), "No DXGI device");
  winrt::com_ptr<IInspectable> inspectable; require(SUCCEEDED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put())), "No WinRT device");
  GraphicsCaptureItem item{nullptr};
  const auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  require(SUCCEEDED(factory->CreateForWindow(window.load(), winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item))), "No capture item for the own window");
  auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(), winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
  std::atomic<int> frames{0};
  const auto token = pool.FrameArrived([&](const Direct3D11CaptureFramePool& sender, const auto&) { if (auto frame = sender.TryGetNextFrame()) { frame.Close(); ++frames; } });
  auto session = pool.CreateCaptureSession(item); session.IsCursorCaptureEnabled(false);
  double defaultMs = -1;
  if (const auto fifth = session.try_as<IGraphicsCaptureSession5>()) {
    defaultMs = double(fifth.MinUpdateInterval().count()) / 10000;
    require(host::deliverEveryComposedFrame(session), "The capture session refused its update interval");
    require(fifth.MinUpdateInterval() == std::chrono::milliseconds(1), "The capture session interval is not 1 ms");
  } else require(!host::deliverEveryComposedFrame(session), "An interval was reported on a session without the property");
  session.StartCapture();
  // Frames keep arriving while the window keeps changing.
  for (int n = 0; n < 60; ++n) { ++paintCount; RedrawWindow(window.load(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); Sleep(16); }
  pool.FrameArrived(token); session.Close(); pool.Close();
  require(frames.load() >= 5, "The capture session delivered no frames of a changing window");
  return defaultMs;
}
}

int main() {
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    pacer(); watch(); priority(); const double defaultMs = sessionInterval();
    std::cout << "{\"kind\":\"capture-pacing\",\"checks\":" << checks << ",\"wgcDefaultMinUpdateIntervalMs\":" << defaultMs << ",\"result\":\"pass\"}" << std::endl;
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
  catch (const winrt::hresult_error& error) { std::cerr << "FAIL: WinRT 0x" << std::hex << uint32_t(error.code()) << '\n'; return 1; }
}
