// Connect-start benchmark (tuning backlog #2, connect-timing-v1): the one-time
// setup a new stream pays on each side before its first picture, on this PC.
// Generated pixels only: no desktop capture, display, audio device, VM, relay
// or network. Each setup runs N times on fresh objects; p50/p90/max in ms.
//   host:    Runtime (COM + Media Foundation) startup, x264 Encoder at the
//            stream size, the first IDR's encode (worker capture thread);
//   client:  gpu::Select (cold, then cached devices), the GPU conversion
//            self-test, Media Foundation startup, RemoteDecoder::Initialize
//            and the first IDR's decode (RemoteEngine::Start / Run);
//   process: MultitapHostTool.exe without arguments, launch to exit (the
//            floor of a per-session worker process; the real worker also
//            loads a user environment, binds its pipe and starts MF).
// A pass here is NOT a real-session, relay or latency result.
//
// media=N (media_warmup.hpp): the part of a stream's media start that needs no
// prepared display, each run in a fresh process as a session worker is: the
// audio thread opening the loopback endpoint (started first), the capture's
// D3D11 device and Windows Graphics Capture's activation (no monitor item,
// frame pool or session: nothing is captured), the encoder (automatic
// backend), then the wait for audio, as stream.info waits. "before" as the
// capture thread did it; "after" with CaptureDevices::warm() run beside the
// 324 ms stand-in for the display preparation (the field p50) and WGC's cached
// answer. Also a resize restart's device + WGC step in one process, before
// (device released, WGC asked again) and after (device kept, cached answer),
// and a worker-like child's exit after its pipe ends (what the display probe
// waits for before it publishes an answer; a lower bound).
//
//   MultitapHostConnectStartBenchmark [runs=N] [size=WxH] [media=N]
#include "media.hpp"
#include "media_warmup.hpp"
#include <d3d11.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include "remote_decoder.h"
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <future>
#include <thread>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using host::Clock;
namespace {
double millis(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
double percentile(std::vector<double> v, double f) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); const size_t i = size_t(std::ceil(f * double(v.size()))); return v[std::max<size_t>(i, 1) - 1]; }
std::map<std::string, std::vector<double>> samples;
std::vector<std::string> order;
void note(const std::string& name, double ms) { if (!samples.count(name)) order.push_back(name); samples[name].push_back(ms); }
template <class F> auto timed(const std::string& name, F&& f) { const auto t0 = Clock::now(); auto value = f(); note(name, millis(Clock::now() - t0)); return value; }

std::vector<uint8_t> desktopLike(int w, int h, int seed) {
  // A window-like picture (flat fills, edges, text-like noise rows), BGRA.
  std::vector<uint8_t> bgra(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    uint8_t* p = &bgra[(size_t(y) * w + x) * 4];
    const bool bar = y < h / 18, panel = x > w / 5 && x < w * 4 / 5 && y > h / 6 && y < h * 5 / 6;
    const bool ink = panel && (y / 7) % 3 == 0 && ((x * 13 + y * 7 + seed) % 11) < 4;
    const uint8_t v = bar ? 40 : ink ? 30 : panel ? 245 : uint8_t(70 + (x + y + seed) % 40);
    p[0] = v; p[1] = uint8_t(bar ? 60 : v); p[2] = uint8_t(bar ? 90 : v); p[3] = 255;
  }
  return bgra;
}

// Windows Graphics Capture's setup before a capture's monitor item, as every
// capture did before media_warmup.hpp: Windows asked each time if supported.
void wgcBefore(ID3D11Device* device) {
  if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) return;
  (void)winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; winrt::com_ptr<IInspectable> inspectable;
  if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) (void)CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
}
// One media start in this (fresh) process; prints {"step":ms,...}.
// before: the capture thread created its device and asked WGC cold.
// after:  CaptureDevices::warm() ran beside the display stand-in; the capture
//         takes its device and WGC's answer is the process's cached one.
int mediaStartChild(bool after, int w, int h) {
  host::Runtime runtime;  // the worker's main thread starts COM/MF at launch
  host::CaptureDevices devices(host::windowsCaptureDeviceAccess());
  if (after) devices.warm(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(324));  // the display preparation stand-in (field p50)
  struct Apartment { Apartment() { (void)RoInitialize(RO_INIT_MULTITHREADED); } ~Apartment() { RoUninitialize(); } } apartment;  // the capture thread's own
  const auto t0 = Clock::now();
  // As the capture thread: audio first, then device + WGC, encoder, audio wait.
  std::promise<bool> audioReady; auto ready = audioReady.get_future();
  std::thread audio([promise = std::move(audioReady)]() mutable {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try { host::Loopback loopback; promise.set_value(loopback.available()); } catch (...) { promise.set_value(false); }
    if (SUCCEEDED(com)) CoUninitialize();
  });
  auto at = Clock::now();
  std::shared_ptr<ID3D11Device> device = after ? devices.take() : nullptr;
  const bool reused = bool(device);
  if (!device) device = host::createCaptureDevice();
  const double deviceMs = millis(Clock::now() - at); at = Clock::now();
  if (after) host::warmCaptureRuntime(device.get()); else wgcBefore(device.get());
  const double wgcMs = millis(Clock::now() - at); at = Clock::now();
  auto encoder = std::make_unique<host::Encoder>(w, h, 60, 20000000, host::EncoderTuning{}, host::EncoderBackend::automatic, host::captureDeviceAdapter(device.get()), device.get());
  const double encoderMs = millis(Clock::now() - at); at = Clock::now();
  const bool audioAvailable = ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready && ready.get();
  const double audioWaitMs = millis(Clock::now() - at), totalMs = millis(Clock::now() - t0);
  audio.join();
  std::cout << "{\"deviceMs\":" << deviceMs << ",\"wgcMs\":" << wgcMs << ",\"encoderMs\":" << encoderMs << ",\"audioWaitMs\":" << audioWaitMs << ",\"totalMs\":" << totalMs
            << ",\"reused\":" << (reused ? "true" : "false") << ",\"audio\":" << (audioAvailable ? "true" : "false") << ",\"encoder\":\"" << encoder->backendName() << "\"}\n";
  encoder.reset(); device.reset(); devices.clear(); devices.join();
  return 0;
}
// A resize restart's device + WGC step in one process. before: the previous
// capture's device released, a new one, WGC asked again; after: kept and taken,
// WGC's cached answer.
void restartSteps(int runs) {
  host::CaptureDevices devices(host::windowsCaptureDeviceAccess());
  for (int run = 0; run < runs; ++run) {
    for (const bool after : {false, true}) {
      auto device = host::createCaptureDevice(); wgcBefore(device.get());  // the previous capture
      if (after) devices.keep(device);
      device.reset();  // the previous capture thread ends
      const auto t0 = Clock::now();
      auto next = after ? devices.take() : nullptr; if (!next) next = host::createCaptureDevice();
      if (after) host::warmCaptureRuntime(next.get()); else wgcBefore(next.get());
      note(after ? "restart.deviceWgc.after" : "restart.deviceWgc.before", millis(Clock::now() - t0));
    }
  }
}
// Where Windows Graphics Capture's setup before a monitor item goes, per call.
void wgcParts(int runs) {
  auto device = host::createCaptureDevice();
  for (int run = 0; run < runs; ++run) {
    auto t0 = Clock::now();
    const bool supported = winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
    note("wgc.isSupported.raw", millis(Clock::now() - t0)); t0 = Clock::now();
    (void)host::graphicsCaptureSupported();
    note("wgc.isSupported.cached", millis(Clock::now() - t0)); t0 = Clock::now();
    (void)winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    note("wgc.itemFactory", millis(Clock::now() - t0)); t0 = Clock::now();
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; winrt::com_ptr<IInspectable> inspectable;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) (void)CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
    note("wgc.d3dInterop", millis(Clock::now() - t0));
    if (!supported) note("wgc.unsupported", 1);
  }
}// A worker-like child for the probe's exit wait: COM/MF started (as the worker's
// main does), waits for its stdin pipe to end, then exits.
int pipeExitChild() {
  host::Runtime runtime;
  std::cout << "ready" << std::endl;
  char byte = 0; DWORD got = 0;
  while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &byte, 1, &got, nullptr) && got) {}
  return 0;
}
// From its pipe's end to the child's exit observed (a lower bound of a probe
// worker's exit wait: the real worker also tears down its Agent and loaded more).
void pipeExitWait(const std::wstring& self, int runs) {
  for (int run = 0; run < runs; ++run) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE}; HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &inherit, 0) || !CreatePipe(&outRead, &outWrite, &inherit, 0)) throw std::runtime_error("pipe");
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdInput = inRead; startup.hStdOutput = outWrite; startup.hStdError = outWrite; PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + self + L"\" --pipe-exit";
    if (!CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) throw std::runtime_error("child");
    CloseHandle(inRead); CloseHandle(outWrite);
    char buffer[16]; DWORD got = 0; (void)ReadFile(outRead, buffer, sizeof(buffer), &got, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = Clock::now(); CloseHandle(inWrite);
    WaitForSingleObject(child.hProcess, 30000);
    note("probe.workerExitAfterPipeEnd.lowerBound", millis(Clock::now() - t0));
    CloseHandle(outRead); CloseHandle(child.hThread); CloseHandle(child.hProcess);
  }
}
std::map<std::string, double> parseFlat(const std::string& line) {
  std::map<std::string, double> out; size_t at = 0;
  while ((at = line.find('"', at)) != std::string::npos) {
    const auto end = line.find('"', at + 1); if (end == std::string::npos) break;
    const auto key = line.substr(at + 1, end - at - 1); at = end + 1;
    if (at < line.size() && line[at] == ':') { char* stop = nullptr; const double v = std::strtod(line.c_str() + at + 1, &stop); if (stop != line.c_str() + at + 1) out[key] = v; }
  }
  return out;
}
std::string runChild(const std::wstring& self, const std::wstring& arguments) {
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE}; HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &inherit, 0)) throw std::runtime_error("pipe");
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = write; startup.hStdError = write; PROCESS_INFORMATION child{};
  std::wstring command = L"\"" + self + L"\" " + arguments;
  if (!CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) { CloseHandle(read); CloseHandle(write); throw std::runtime_error("child"); }
  CloseHandle(write); std::string out; char buffer[4096]; DWORD got = 0;
  while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) out.append(buffer, got);
  WaitForSingleObject(child.hProcess, 60000); CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(read);
  return out;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    int runs = 10, w = 1920, h = 1080, media = 0;
    if (argc == 5 && std::wstring(argv[1]) == L"--media-start") return mediaStartChild(std::wstring(argv[2]) == L"after", _wtoi(argv[3]), _wtoi(argv[4]));
    if (argc == 2 && std::wstring(argv[1]) == L"--pipe-exit") return pipeExitChild();
    for (int i = 1; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a.rfind(L"runs=", 0) == 0) runs = std::clamp(_wtoi(a.c_str() + 5), 1, 200);
      else if (a.rfind(L"size=", 0) == 0) { const auto x = a.find(L'x'); w = _wtoi(a.substr(5, x - 5).c_str()); h = _wtoi(a.substr(x + 1).c_str()); }
      else if (a.rfind(L"media=", 0) == 0) media = std::clamp(_wtoi(a.c_str() + 6), 1, 100);
      else throw std::runtime_error("Usage: [runs=N] [size=WxH] [media=N]");
    }
    if (media) {
      // Alternating before/after fresh processes, so drift affects both alike.
      std::string backend;
      for (int run = 0; run < media; ++run) for (const bool after : {false, true}) {
        const auto line = runChild(argv[0], std::wstring(L"--media-start ") + (after ? L"after " : L"before ") + std::to_wstring(w) + L" " + std::to_wstring(h));
        const auto values = parseFlat(line);
        if (!values.count("totalMs")) throw std::runtime_error("media start child failed: " + line);
        for (const auto& [key, value] : values) note(std::string(after ? "after." : "before.") + key, value);
        if (const auto at = line.find("\"encoder\":\""); at != std::string::npos) backend = line.substr(at + 11, line.find('"', at + 11) - at - 11);
      }
      {
        // The worker's process-wide apartment: C++/WinRT keeps its factories for it.
        host::Runtime runtime; restartSteps(media); wgcParts(media);
      }
      pipeExitWait(argv[0], media);
      std::cout << "{\"mode\":\"media-start\",\"size\":\"" << w << 'x' << h << "\",\"processes\":" << media * 2 << ",\"encoder\":\"" << backend << "\",\"displayPrepStandInMs\":324,\"captured\":false,\"ms\":{";
      bool comma = false;
      for (const auto& name : order) {
        const auto& v = samples[name];
        std::cout << (comma ? "," : "") << "\n  \"" << name << "\":{\"n\":" << v.size() << std::fixed << std::setprecision(2) << ",\"p50\":" << percentile(v, 0.5) << ",\"p90\":" << percentile(v, 0.9) << ",\"max\":" << *std::max_element(v.begin(), v.end()) << '}';
        comma = true;
      }
      std::cout << "\n}}\n";
      return 0;
    }
    if (w < 64 || h < 64 || w > 7680 || h > 4320 || w % 2 || h % 2) throw std::runtime_error("Invalid size");
    std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2);
    const auto bgra = desktopLike(w, h, 1);
    host::bgraToNv12(bgra.data(), nv12.data(), w, h);
    std::vector<uint8_t> idr;
    std::string adapter = "cpu";
    UINT decodeVendor = 0;
    for (int run = 0; run < runs; ++run) {
      // ---- Host worker: Runtime, encoder construction, first IDR.
      {
        timed("host.runtime", [&] { host::Runtime runtime; return 0; });
        host::Runtime runtime;
        auto encoder = timed("host.encoder.x264", [&] { return std::make_unique<host::Encoder>(w, h, 60, 20000000, host::EncoderTuning{}, host::EncoderBackend::x264); });
        std::vector<uint8_t> first;
        timed("host.firstIdr.encode", [&] { encoder->encode(nv12.data(), 0, [&](host::Frame frame) { if (first.empty() && frame.key) first = std::move(frame.bytes); }); return 0; });
        if (first.empty()) throw std::runtime_error("The encoder produced no first IDR");
        if (idr.empty()) idr = first;
        timed("host.encoder.destroy", [&] { encoder.reset(); return 0; });
      }
      // ---- Client engine: GPU selection, conversion self-test, decoder.
      {
        multitap::gpu::SelectionOptions choice;  // hardware decode + GPU presentation (the defaults)
        multitap::gpu::Selection selection = timed(run == 0 ? "client.select.first" : "client.select.cached", [&] {
          // No Flutter window here: the render adapter is the primary DXGI adapter.
          Microsoft::WRL::ComPtr<IDXGIFactory1> factory; Microsoft::WRL::ComPtr<IDXGIAdapter1> primary;
          if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) factory->EnumAdapters1(0, &primary);
          return multitap::gpu::Select(primary.Get(), uint32_t(w), uint32_t(h), choice); });
        std::unique_ptr<multitap::gpu::Converter> converter;
        if (selection.render) {
          converter = std::make_unique<multitap::gpu::Converter>(selection.render);
          const bool same = timed("client.conversionSelfTest", [&] { return multitap::gpu::ConversionSelfTest(*converter); });
          if (!same) converter.reset();
        }
        timed("client.mfStartup", [&] { return MFStartup(MF_VERSION); });
        {
          // Where a CPU decoder's start goes, step by step as RemoteDecoder::Create
          // takes them for the CPU: creation, low latency + 2 worker threads,
          // input type, NV12 output type, begin streaming, start of stream.
          Microsoft::WRL::ComPtr<IMFTransform> mft;
          host::check(timed("client.mft.create", [&] { return CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)); }), "H.264 MFT");
          timed("client.mft.codecSettings", [&] {
            Microsoft::WRL::ComPtr<ICodecAPI> codec; if (FAILED(mft.As(&codec))) return 0;
            VARIANT value; VariantInit(&value); value.vt = VT_UI4; value.ulVal = 0; codec->SetValue(&CODECAPI_AVDecVideoAcceleration_H264, &value);
            value.ulVal = 1; codec->SetValue(&CODECAPI_AVLowLatencyMode, &value); value.ulVal = 2; codec->SetValue(&CODECAPI_AVDecNumWorkerThreads, &value); return 0; });
          Microsoft::WRL::ComPtr<IMFMediaType> input; host::check(MFCreateMediaType(&input), "Media type");
          input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
          input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h)); MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1);
          host::check(timed("client.mft.inputType", [&] { return mft->SetInputType(0, input.Get(), 0); }), "H.264 MFT input");
          host::check(timed("client.mft.outputType", [&] {
            for (DWORD index = 0; index < 32; ++index) {
              Microsoft::WRL::ComPtr<IMFMediaType> output; HRESULT hr = mft->GetOutputAvailableType(0, index, &output); if (FAILED(hr)) return hr;
              GUID subtype{}; output->GetGUID(MF_MT_SUBTYPE, &subtype); if (subtype == MFVideoFormat_NV12) return mft->SetOutputType(0, output.Get(), 0);
            }
            return HRESULT(MF_E_INVALIDMEDIATYPE); }), "H.264 MFT output");
          timed("client.mft.beginStreaming", [&] { return mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); });
          timed("client.mft.startOfStream", [&] { return mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0); });
          timed("client.mft.release", [&] { mft.Reset(); return 0; });
        }
        if (selection.decode) {
          // The CPU decoder alone, for comparison with a hardware attempt that fell back.
          multitap::RemoteDecoder software;
          host::check(timed("client.decoder.initialize.cpuOnly", [&] { return software.Initialize(uint32_t(w), uint32_t(h)); }), "Client CPU decoder");
          decodeVendor = selection.decode->info.vendor;
        }
        {
          multitap::RemoteDecoder decoder;
          multitap::RemoteDecoder::Options options;
          options.decode_device = selection.decode; options.tier = selection.tier; options.converter = converter.get();
          const HRESULT initialized = timed("client.decoder.initialize", [&] { return decoder.Initialize(uint32_t(w), uint32_t(h), options); });
          host::check(initialized, "Client decoder");
          adapter = decoder.hardware() ? "hardware" : decoder.fallbacks() ? "software after a hardware attempt" : "software";
          bool decoded = false;
          const auto t0 = Clock::now();
          // Decode waits briefly for a late software picture itself (as Run does);
          // a hardware picture still pending comes out with the drain.
          host::check(decoder.Decode(idr.data(), idr.size(), 0, [&](multitap::DecodedFrame&&) { decoded = true; }), "Client decode");
          if (!decoded) host::check(decoder.Drain([&](multitap::DecodedFrame&&) { decoded = true; }), "Client decode drain");
          note("client.firstIdr.decode", millis(Clock::now() - t0));
          if (!decoded) note("client.firstIdr.missing", 1);
        }
        MFShutdown();
      }
    }
    // ---- Process launch floor of a per-session worker.
    const auto tool = std::filesystem::path(argv[0]).parent_path() / L"MultitapHostTool.exe";
    if (std::filesystem::exists(tool)) {
      for (int run = 0; run < runs; ++run) {
        std::wstring command = L"\"" + tool.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESTDHANDLES; PROCESS_INFORMATION child{};
        const auto t0 = Clock::now();
        if (!CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) throw std::runtime_error("Cannot launch MultitapHostTool.exe");
        WaitForSingleObject(child.hProcess, 30000);
        note("process.launchToExit", millis(Clock::now() - t0));
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
      }
    }
    std::cout << "{\"size\":\"" << w << 'x' << h << "\",\"runs\":" << runs << ",\"clientDecoder\":\"" << adapter << "\",\"decodeAdapterVendor\":" << decodeVendor << ",\"idrBytes\":" << idr.size() << ",\"ms\":{";
    bool comma = false;
    for (const auto& name : order) {
      const auto& v = samples[name];
      std::cout << (comma ? "," : "") << "\n  \"" << name << "\":{\"n\":" << v.size() << std::fixed << std::setprecision(2) << ",\"p50\":" << percentile(v, 0.5) << ",\"p90\":" << percentile(v, 0.9) << ",\"max\":" << *std::max_element(v.begin(), v.end()) << '}';
      comma = true;
    }
    std::cout << "\n}}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
