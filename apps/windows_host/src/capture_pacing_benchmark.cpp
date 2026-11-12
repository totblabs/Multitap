// Change -> capture latency of the capture loop pacing, measured with a real
// Windows Graphics Capture of this benchmark's OWN off-screen window (no other
// window or desktop pixel is captured; nothing is saved or sent).
//
// A trial repaints the window with a new colour (the app's response to an
// input), then measures when the capture loop starts the tick that sees the
// new frame (pacing + compositor delay), and optionally when that tick's
// synthetic 1080p NV12 conversion + x264 encode finished (--work). Trials
// start at random phases. Modes: fixed (previous: fixed 60 Hz tick) and event
// (FramePacer woken by the WGC frame-arrival event, capped by
// captureMinimumGap). --burn N runs N normal-priority busy threads in a child
// process (CPU contention like a busy guest); --boost applies StreamPriority.
#include "frame_pacer.hpp"
#include "capture_session_interval.hpp"
#include "stream_priority.hpp"
#include "media.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
double ms(Clock::duration value) { return std::chrono::duration<double, std::milli>(value).count(); }
struct Series {
  std::vector<double> values;
  void add(double value) { values.push_back(value); }
  double quantile(double q) const { if (values.empty()) return 0; auto sorted = values; std::sort(sorted.begin(), sorted.end()); return sorted[std::min(sorted.size() - 1, size_t(q * double(sorted.size() - 1) + 0.5))]; }
  double mean() const { return values.empty() ? 0 : std::accumulate(values.begin(), values.end(), 0.0) / double(values.size()); }
  void json(std::ostream& out) const { out << "{\"n\":" << values.size() << ",\"mean\":" << mean() << ",\"p50\":" << quantile(.5) << ",\"p95\":" << quantile(.95) << ",\"max\":" << quantile(1) << '}'; }
};
std::atomic<COLORREF> paintColour{RGB(10, 20, 30)};
// --wgc-default-interval keeps the OS session interval (16 ms on build 26100)
// instead of the product's deliverEveryComposedFrame, for an A/B.
bool keepWgcDefault = false;
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); HBRUSH brush = CreateSolidBrush(paintColour.load()); RECT area{}; GetClientRect(window, &area); FillRect(dc, &area, brush); DeleteObject(brush); EndPaint(window, &paint); return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
// Normal-priority busy threads in a child process (killed with its job).
int burner(int threads) {
  std::vector<std::thread> busy; std::atomic<bool> forever{true};
  for (int i = 0; i < threads; ++i) busy.emplace_back([&] { volatile uint64_t x = 0; while (forever) x = x * 6364136223846793005ull + 1; });
  for (auto& thread : busy) thread.join();
  return 0;
}

struct Result { Series detect, encoded; uint64_t ticks = 0, wakes = 0; double loopCpuMs = 0, wallMs = 0; int missed = 0; };

Result run(bool event, int trials, bool work, HWND window, bool boost) {
  using namespace winrt::Windows::Graphics::Capture;
  host::Com<ID3D11Device> device; host::Com<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
    host::check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
  host::Com<IDXGIDevice> dxgi; host::check(device.As(&dxgi), "DXGI device"); winrt::com_ptr<IInspectable> inspectable;
  host::check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()), "WinRT device");
  const auto winDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  GraphicsCaptureItem item{nullptr};
  const auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  host::check(factory->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)), "Capture own window");
  HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(winDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
  auto arrived = pool.FrameArrived([signal](auto const&, auto const&) { SetEvent(signal); });
  auto session = pool.CreateCaptureSession(item); session.IsCursorCaptureEnabled(false); if (!keepWgcDefault) host::deliverEveryComposedFrame(session); session.StartCapture();
  D3D11_TEXTURE2D_DESC desc{}; desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  host::Com<ID3D11Texture2D> probe; host::check(device->CreateTexture2D(&desc, nullptr, &probe), "Probe texture");

  std::atomic<bool> stop{false}; std::atomic<uint32_t> expected{0}; std::atomic<int64_t> detectedAt{0}, encodedAt{0};
  Result result;
  std::thread loop([&] {
    std::unique_ptr<host::StreamPriority> priority; if (boost) priority = std::make_unique<host::StreamPriority>();
    const int width = 1920, height = 1080; std::vector<uint8_t> bgra(size_t(width) * height * 4), nv12(size_t(width) * height * 3 / 2);
    std::unique_ptr<host::Encoder> encoder; if (work) encoder = std::make_unique<host::Encoder>(width, height, 60, 12000000);
    host::FramePacer pacer; const auto period = std::chrono::microseconds(1000000 / 60); auto next = Clock::now(); const auto start = next; uint64_t pts = 0, frame = 0;
    Clock::time_point lastWork{};  // as the product loop: last tick with a new frame
    ULONG64 cycles0 = 0, cycles1 = 0; QueryThreadCycleTime(GetCurrentThread(), &cycles0);
    while (!stop) {
      const auto began = Clock::now(); ++result.ticks;
      ResetEvent(signal);
      winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest{nullptr};
      for (int n = 0; n < 3; ++n) { auto taken = pool.TryGetNextFrame(); if (!taken) break; if (latest) latest.Close(); latest = std::move(taken); }
      if (latest) {
        lastWork = began;
        auto access = latest.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(); host::Com<ID3D11Texture2D> texture;
        host::check(access->GetInterface(IID_PPV_ARGS(&texture)), "Frame texture");
        D3D11_BOX box{8, 8, 0, 9, 9, 1}; context->CopySubresourceRegion(probe.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(context->Map(probe.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Probe map");
        uint32_t bgrx; std::memcpy(&bgrx, mapped.pData, 4); context->Unmap(probe.Get(), 0); latest.Close();
        const uint32_t want = expected.load();
        const bool change = want && (bgrx & 0xffffff) == want && detectedAt.load() == 0;
        if (change) detectedAt = began.time_since_epoch().count();
        if (work) {
          // A frame's work: changing synthetic content -> NV12 -> x264.
          for (size_t i = 0; i < bgra.size(); i += 4096) bgra[i] = uint8_t(frame * 7 + i);
          ++frame; host::bgraToNv12(bgra.data(), nv12.data(), width, height);
          pts = std::max<uint64_t>(pts + 1, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count()));
          encoder->encode(nv12.data(), pts, [](host::Frame) {});
        }
        if (change) encodedAt = Clock::now().time_since_epoch().count();
      }
      const auto now = Clock::now(); next += period; if (next < now) next = now;
      if (event) { if (pacer.wait(next, std::span<const HANDLE>(&signal, 1), lastWork + host::captureMinimumGap(period))) { next = Clock::now(); ++result.wakes; } }
      else pacer.wait(next);
    }
    QueryThreadCycleTime(GetCurrentThread(), &cycles1); result.loopCpuMs = double(cycles1 - cycles0) / 1e6;  // megacycles
  });
  std::mt19937 random(event ? 11 : 7);
  Sleep(300);
  const auto began = Clock::now();
  for (int trial = 0; trial < trials; ++trial) {
    Sleep(25 + random() % 60);  // random phase against the 60 Hz loop and the compositor
    const COLORREF colour = RGB(40 + trial % 200, 200 - trial % 150, (trial * 37) % 256);
    detectedAt = 0; encodedAt = 0;
    paintColour = colour; expected = uint32_t(GetBValue(colour)) | (uint32_t(GetGValue(colour)) << 8) | (uint32_t(GetRValue(colour)) << 16);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);  // returns after WM_PAINT ran
    const auto painted = Clock::now();
    while (Clock::now() - painted < std::chrono::milliseconds(500) && (detectedAt.load() == 0 || encodedAt.load() == 0)) Sleep(1);
    const auto detected = detectedAt.load(), encoded = encodedAt.load();
    if (!detected || !encoded) { ++result.missed; continue; }
    result.detect.add(ms(Clock::time_point(Clock::duration(detected)) - painted));
    result.encoded.add(ms(Clock::time_point(Clock::duration(encoded)) - painted));
  }
  result.wallMs = ms(Clock::now() - began);
  stop = true; loop.join();
  pool.FrameArrived(arrived); session.Close(); pool.Close(); CloseHandle(signal);
  return result;
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    int trials = 150, burn = 0; bool work = false, boost = false; std::wstring mode = L"both";
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--burner" && i + 1 < argc) return burner(std::stoi(argv[++i]));
      if (arg == L"--trials" && i + 1 < argc) trials = std::stoi(argv[++i]);
      else if (arg == L"--burn" && i + 1 < argc) burn = std::stoi(argv[++i]);
      else if (arg == L"--mode" && i + 1 < argc) mode = argv[++i];
      else if (arg == L"--work") work = true;
      else if (arg == L"--boost") boost = true;
      else if (arg == L"--wgc-default-interval") keepWgcDefault = true;
      else throw std::runtime_error("Usage: MultitapHostCapturePacingBenchmark [--trials N] [--mode fixed|event|both] [--work] [--burn N] [--boost] [--wgc-default-interval]");
    }
    require(trials >= 10 && trials <= 2000 && burn >= 0 && burn <= 64, "Invalid arguments");
    host::Runtime runtime;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    // Own 64x64 window in the primary monitor's bottom-right corner (the
    // compositor does not compose an off-screen window), never activated,
    // click-through and almost fully transparent on screen (alpha 1/255).
    std::atomic<HWND> window{nullptr};
    std::thread ui([&] {
      WNDCLASSW type{}; type.lpfnWndProc = windowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MultitapPacingBenchmark"; RegisterClassW(&type);
      const int x = GetSystemMetrics(SM_CXSCREEN) - 64, y = GetSystemMetrics(SM_CYSCREEN) - 64;
      HWND created = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, type.lpszClassName, L"Multitap pacing benchmark", WS_POPUP, x, y, 64, 64, nullptr, nullptr, type.hInstance, nullptr);
      SetLayeredWindowAttributes(created, 0, 1, LWA_ALPHA);
      ShowWindow(created, SW_SHOWNOACTIVATE); UpdateWindow(created); window = created;
      MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    });
    while (!window.load()) Sleep(1);
    PROCESS_INFORMATION child{}; HANDLE job = nullptr;
    if (burn) {
      job = CreateJobObjectW(nullptr, nullptr); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
      wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH); std::wstring command = L"\"" + std::wstring(path) + L"\" --burner " + std::to_wstring(burn);
      STARTUPINFOW startup{sizeof(startup)}; require(CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE, "Burner process");
      AssignProcessToJobObject(job, child.hProcess); ResumeThread(child.hThread); Sleep(500);
    }
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\"kind\":\"capture-pacing-benchmark\",\"ownWindowOnly\":true,\"pixelsSaved\":false,\"trials\":" << trials << ",\"work\":" << (work ? "true" : "false") << ",\"burnThreads\":" << burn << ",\"boost\":" << (boost ? "true" : "false") << ",\"wgcInterval\":\"" << (keepWgcDefault ? "default" : "1ms") << '"' << ",\"logicalCpus\":" << std::thread::hardware_concurrency() << '}' << std::endl;
    for (const bool event : {false, true}) {
      if ((event && mode == L"fixed") || (!event && mode == L"event")) continue;
      const auto result = run(event, trials, work, window.load(), boost);
      std::cout << "{\"pacing\":\"" << (event ? "event" : "fixed") << "\",\"changeToTickMs\":"; result.detect.json(std::cout);
      std::cout << ",\"changeToEncodedMs\":"; result.encoded.json(std::cout);
      std::cout << ",\"missed\":" << result.missed << ",\"ticksPerSecond\":" << double(result.ticks) * 1000 / result.wallMs << ",\"frameWakes\":" << result.wakes << ",\"loopMegacyclesPerSecond\":" << result.loopCpuMs * 1000 / result.wallMs << '}' << std::endl;
    }
    if (job) { TerminateJobObject(job, 0); CloseHandle(job); CloseHandle(child.hThread); CloseHandle(child.hProcess); }
    PostMessageW(window.load(), WM_QUIT, 0, 0); PostThreadMessageW(GetThreadId(ui.native_handle()), WM_QUIT, 0, 0); ui.join();
    return 0;
  } catch (const winrt::hresult_error& error) { std::cerr << "WinRT 0x" << std::hex << uint32_t(error.code()) << '\n'; return 1; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
