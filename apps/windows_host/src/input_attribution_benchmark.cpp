// Input -> captured-frame attribution on a real Windows Graphics Capture of this
// benchmark's OWN off-screen-ish window (no other window or desktop pixel is
// captured; nothing is saved or sent). It answers whether the host-side
// `hostInputCapture` figure (injection -> the capture tick of the first changed
// frame, host::InputFrameTagger) is a real delay or an attribution artefact on a
// mostly still screen.
//
// The window shows an "ambient" element that blinks every --ambient-ms (like a
// text caret) and an "effect" element that only an input trial changes. Two
// kinds of trial run after a random idle time (1.1-1.9 s):
//   effect:    the tagger is told about an injection, 10 ms later the "app"
//              repaints the effect element. Truth: the tagged frame must show it.
//   no-effect: the tagger is told about an injection and nothing is drawn (a key
//              that does nothing, a click on nothing). Truth: no frame tag.
// The same trial sequence runs through the previous tagger (1 s window, no
// periodicity filter) and the current one. The tagger is driven exactly like the
// production capture loop (beginCapture before taking the frame, submitted after,
// FramePacer woken by the frame event). SendInput is not used: this isolates
// capture + attribution, not the injector. Single run on one machine, not a
// field pass.
#include "frame_pacer.hpp"
#include "capture_session_interval.hpp"
#include "input_latency.hpp"
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
#include <map>
#include <mutex>
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
  void json(std::ostream& out) const { out << "{\"n\":" << values.size() << ",\"p50\":" << quantile(.5) << ",\"p95\":" << quantile(.95) << ",\"max\":" << quantile(1) << '}'; }
};
std::atomic<COLORREF> effectColour{RGB(0, 0, 0)};
std::atomic<bool> ambientOn{false};
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
    HBRUSH base = CreateSolidBrush(RGB(20, 20, 20)); RECT all{0, 0, 64, 64}; FillRect(dc, &all, base); DeleteObject(base);
    HBRUSH effect = CreateSolidBrush(effectColour.load()); RECT e{8, 8, 20, 20}; FillRect(dc, &e, effect); DeleteObject(effect);
    HBRUSH blink = CreateSolidBrush(ambientOn.load() ? RGB(250, 250, 250) : RGB(20, 20, 20)); RECT a{40, 40, 44, 56}; FillRect(dc, &a, blink); DeleteObject(blink);
    EndPaint(window, &paint); return 0;
  }
  if (message == WM_TIMER) { ambientOn = !ambientOn.load(); InvalidateRect(window, nullptr, FALSE); UpdateWindow(window); return 0; }
  return DefWindowProcW(window, message, w, l);
}

struct Observed { uint64_t captureUs = 0; bool effectSeen = false; };
struct Config { const char* name; host::InputFrameTagger::Options options; };

void run(const Config& config, int trials, HWND window, int ambientMs, std::ostream& out) {
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
  auto session = pool.CreateCaptureSession(item); session.IsCursorCaptureEnabled(false); host::deliverEveryComposedFrame(session); session.StartCapture();
  D3D11_TEXTURE2D_DESC desc{}; desc.Width = 64; desc.Height = 64; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  host::Com<ID3D11Texture2D> probe; host::check(device->CreateTexture2D(&desc, nullptr, &probe), "Probe texture");

  std::mutex mutex; host::InputFrameTagger tagger(config.options);
  std::atomic<bool> stop{false}; std::atomic<uint32_t> wantedEffect{0};
  std::map<uint32_t, Observed> tagged;  // sample -> the tagged frame
  std::thread loop([&] {
    host::FramePacer pacer; const auto period = std::chrono::microseconds(1000000 / 60); auto next = Clock::now(); Clock::time_point lastWork{};
    uint64_t previous = 0, pts = 0;
    while (!stop) {
      const auto began = Clock::now(); ResetEvent(signal);
      { std::lock_guard lock(mutex); tagger.beginCapture(host::hostMonotonicUs()); }
      winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest{nullptr};
      for (int n = 0; n < 3; ++n) { auto taken = pool.TryGetNextFrame(); if (!taken) break; if (latest) latest.Close(); latest = std::move(taken); }
      bool changed = false; bool effectSeen = false;
      if (latest) {
        lastWork = began;
        auto access = latest.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(); host::Com<ID3D11Texture2D> texture;
        host::check(access->GetInterface(IID_PPV_ARGS(&texture)), "Frame texture");
        D3D11_BOX box{0, 0, 0, 64, 64, 1}; context->CopySubresourceRegion(probe.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{}; host::check(context->Map(probe.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Probe map");
        uint64_t hash = 1469598103934665603ull; for (int y = 0; y < 64; ++y) { const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch; for (int x = 0; x < 256; ++x) hash = (hash ^ row[x]) * 1099511628211ull; }
        uint32_t pixel; std::memcpy(&pixel, static_cast<const uint8_t*>(mapped.pData) + 10 * mapped.RowPitch + 10 * 4, 4);
        context->Unmap(probe.Get(), 0); latest.Close();
        changed = hash != previous; previous = hash;
        const uint32_t want = wantedEffect.load(); effectSeen = want && (pixel & 0xffffff) == want;
      }
      { std::lock_guard lock(mutex);
        ++pts; tagger.submitted(pts, changed);
        if (const auto tag = tagger.output(pts, host::hostMonotonicUs())) tagged[tag->sample] = {tag->captureUs.value_or(0), effectSeen}; }
      const auto now = Clock::now(); next += period; if (next < now) next = now;
      if (pacer.wait(next, std::span<const HANDLE>(&signal, 1), lastWork + host::captureMinimumGap(period))) next = Clock::now();
    }
  });
  Sleep(1500);  // settle: the first frames and the blink rhythm
  std::mt19937 random(12345);  // the same trial order for every config
  Series effectOk, effectDetect, falseDelay; int effectTrials = 0, effectCorrect = 0, effectMissed = 0, effectStolen = 0, noEffectTrials = 0, noEffectTagged = 0;
  for (int trial = 0; trial < trials; ++trial) {
    Sleep(1100 + random() % 800);
    const bool effect = (random() & 1) != 0;
    const uint32_t sample = uint32_t(trial + 1);
    const COLORREF colour = RGB(40 + (trial * 11) % 200, 60 + (trial * 29) % 190, 90 + (trial * 41) % 160);
    const uint32_t bgr = uint32_t(GetBValue(colour)) | (uint32_t(GetGValue(colour)) << 8) | (uint32_t(GetRValue(colour)) << 16);
    wantedEffect = bgr;
    { std::lock_guard lock(mutex); tagger.activity(host::hostMonotonicUs()); tagger.injected(sample, host::hostMonotonicUs()); }
    const auto injected = Clock::now();
    if (effect) { Sleep(10); effectColour = colour; RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); }
    Sleep(int(config.options.windowUs / 1000) + 150);  // until the tagger resolved it
    std::optional<Observed> found; { std::lock_guard lock(mutex); if (tagged.contains(sample)) found = tagged.at(sample); }
    if (effect) {
      ++effectTrials;
      if (!found) ++effectMissed; else if (found->effectSeen) { ++effectCorrect; effectOk.add(double(found->captureUs) / 1000); } else ++effectStolen;
    } else { ++noEffectTrials; if (found) { ++noEffectTagged; falseDelay.add(double(found->captureUs) / 1000); } }
    (void)injected; (void)ambientMs;
    effectColour = RGB(0, 0, 0); wantedEffect = 0;
  }
  stop = true; loop.join();
  size_t expired = 0, ambient = 0; for (const auto& event : tagger.misses()) (event.kind == host::InputFrameTagger::Miss::expired ? expired : ambient)++;
  out << "{\"tagger\":\"" << config.name << "\",\"windowMs\":" << config.options.windowUs / 1000 << ",\"periodicFilter\":" << (config.options.ambient ? "true" : "false")
      << ",\"effectTrials\":" << effectTrials << ",\"effectTaggedCorrectly\":" << effectCorrect << ",\"effectUntagged\":" << effectMissed << ",\"effectCreditedToAmbientChange\":" << effectStolen
      << ",\"effectInjectToCaptureMs\":"; effectOk.json(out);
  out << ",\"noEffectTrials\":" << noEffectTrials << ",\"noEffectFalselyTagged\":" << noEffectTagged << ",\"noEffectFakeLatencyMs\":"; falseDelay.json(out);
  out << ",\"expiredEvents\":" << expired << ",\"ambientSkipEvents\":" << ambient << "}" << std::endl;
  pool.FrameArrived(arrived); session.Close(); pool.Close(); CloseHandle(signal);
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    int trials = 60, ambientMs = 530;
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--trials" && i + 1 < argc) trials = std::stoi(argv[++i]);
      else if (arg == L"--ambient-ms" && i + 1 < argc) ambientMs = std::stoi(argv[++i]);
      else throw std::runtime_error("Usage: MultitapHostInputAttributionBenchmark [--trials N] [--ambient-ms N (0 = none)]");
    }
    require(trials >= 4 && trials <= 400 && ambientMs >= 0 && ambientMs <= 5000, "Invalid arguments");
    host::Runtime runtime;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::atomic<HWND> window{nullptr};
    std::thread ui([&] {
      WNDCLASSW type{}; type.lpfnWndProc = windowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MultitapAttributionBenchmark"; RegisterClassW(&type);
      const int x = GetSystemMetrics(SM_CXSCREEN) - 64, y = GetSystemMetrics(SM_CYSCREEN) - 64;
      HWND created = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, type.lpszClassName, L"Multitap attribution benchmark", WS_POPUP, x, y, 64, 64, nullptr, nullptr, type.hInstance, nullptr);
      SetLayeredWindowAttributes(created, 0, 1, LWA_ALPHA);
      ShowWindow(created, SW_SHOWNOACTIVATE); UpdateWindow(created); if (ambientMs) SetTimer(created, 1, UINT(ambientMs), nullptr); window = created;
      MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    });
    while (!window.load()) Sleep(1);
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\"kind\":\"input-attribution-benchmark\",\"ownWindowOnly\":true,\"pixelsSaved\":false,\"injector\":false,\"trials\":" << trials << ",\"ambientMs\":" << ambientMs << "}" << std::endl;
    host::InputFrameTagger::Options legacy; legacy.windowUs = 1000000; legacy.ambient = false;
    run({"previous", legacy}, trials, window.load(), ambientMs, std::cout);
    run({"current", {}}, trials, window.load(), ambientMs, std::cout);
    PostMessageW(window.load(), WM_QUIT, 0, 0); PostThreadMessageW(GetThreadId(ui.native_handle()), WM_QUIT, 0, 0); ui.join();
    return 0;
  } catch (const winrt::hresult_error& error) { std::cerr << "WinRT 0x" << std::hex << uint32_t(error.code()) << '\n'; return 1; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
