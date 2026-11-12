// Faster stream start (media_warmup.hpp): which device the next capture of a
// worker starts with, the warm-up beside the display preparation, the cached
// WGC support answer, a real hardware D3D11 device on this PC (created and
// compared only: no capture, display, audio stream or input) and the worker's
// call sites.
#include "media_warmup.hpp"
#include <d3d11.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace host;
namespace {
int checks = 0;
void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
// Stand-ins: never dereferenced, every device operation goes through the access.
struct FakeDevices {
  std::vector<int> objects = std::vector<int>(8);
  std::shared_ptr<ID3D11Device> device(size_t index) { return std::shared_ptr<ID3D11Device>(reinterpret_cast<ID3D11Device*>(&objects[index]), [](ID3D11Device*) {}); }
  size_t index(ID3D11Device* device) { return size_t(reinterpret_cast<int*>(device) - objects.data()); }
};
struct World {
  FakeDevices fakes; std::atomic<int> created{0}, warmedCapture{0}, warmedAudio{0};
  std::atomic<uint64_t> current{7}; std::vector<uint64_t> adapters = std::vector<uint64_t>(8, 7); std::vector<bool> removed = std::vector<bool>(8, false);
  std::atomic<bool> block{false}, throwOnCreate{false}; std::atomic<ID3D11Device*> capturedWith{nullptr};
  CaptureDeviceAccess access() {
    CaptureDeviceAccess value;
    value.create = [this] { if (throwOnCreate) throw std::runtime_error("no device"); while (block) std::this_thread::sleep_for(std::chrono::milliseconds(1)); ++created; return fakes.device(5); };
    value.adapter = [this](ID3D11Device* device) -> std::optional<uint64_t> { const auto at = fakes.index(device); if (adapters[at] == 0) return std::nullopt; return adapters[at]; };
    value.current = [this]() -> std::optional<uint64_t> { if (!current) return std::nullopt; return current.load(); };
    value.removed = [this](ID3D11Device* device) { return bool(removed[fakes.index(device)]); };
    value.warmCapture = [this](ID3D11Device* device) { capturedWith = device; ++warmedCapture; };
    value.warmAudio = [this] { ++warmedAudio; };
    return value;
  }
};

void handOver() {
  {  // A kept device is the next capture's, once.
    World w; CaptureDevices devices(w.access());
    require(!devices.take(), "Nothing kept, yet a device was handed out");
    devices.keep(w.fakes.device(1));
    require(devices.take().get() == w.fakes.device(1).get(), "A resize restart did not get the previous capture's device");
    require(!devices.take() && !devices.holding(), "A kept device was handed out twice");
  }
  {  // Only on the adapter a new device would be created on now.
    World w; CaptureDevices devices(w.access());
    devices.keep(w.fakes.device(1)); w.current = 9;
    require(!devices.take() && !devices.holding(), "A device on another adapter than the current default was handed out");
    w.current = 0; devices.keep(w.fakes.device(1));
    require(!devices.take(), "A device was handed out while the default adapter was unreadable");
    w.current = 7; w.adapters[2] = 0; devices.keep(w.fakes.device(2));
    require(!devices.take(), "A device whose adapter was unreadable was handed out");
  }
  {  // Never a removed device; strict without a removal check.
    World w; CaptureDevices devices(w.access());
    w.removed[3] = true; devices.keep(w.fakes.device(3));
    require(!devices.take(), "A removed device was handed out");
    auto strict = w.access(); strict.removed = nullptr; CaptureDevices unchecked(strict);
    unchecked.keep(w.fakes.device(1));
    require(!unchecked.take(), "A device was handed out without a removal check");
  }
  {  // clear() and an empty keep() drop the device.
    World w; CaptureDevices devices(w.access());
    devices.keep(w.fakes.device(1)); devices.clear();
    require(!devices.holding() && !devices.take(), "clear() kept the device");
    devices.keep(w.fakes.device(1)); devices.keep({});
    require(!devices.take(), "An empty keep() (a WARP or GDI capture) left the older device");
  }
}

void warmUp() {
  {  // The warm-up creates the device and loads WGC on its own thread; audio only when asked.
    World w; CaptureDevices devices(w.access());
    devices.warm(true); devices.join();
    require(w.created == 1 && w.warmedCapture == 1 && w.capturedWith.load() == w.fakes.device(5).get() && w.warmedAudio == 1, "The warm-up did not create the device, load WGC with it and open audio");
    require(devices.take().get() == w.fakes.device(5).get(), "The first capture did not get the warmed device");
    World quiet; CaptureDevices silent(quiet.access());
    silent.warm(false); silent.join();
    require(quiet.created == 1 && quiet.warmedAudio == 0, "A stream without audio opened the loopback endpoint");
  }
  {  // A capture that arrives while the device is still being created waits for it.
    World w; w.block = true; CaptureDevices devices(w.access());
    devices.warm(false);
    std::shared_ptr<ID3D11Device> taken; std::thread capture([&] { taken = devices.take(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    w.block = false; capture.join(); devices.join();
    require(taken.get() == w.fakes.device(5).get(), "A capture during the warm-up did not wait for its device");
  }
  {  // ... but only within the bound; the late device is then dropped, not stored.
    World w; w.block = true; CaptureDevices devices(w.access(), std::chrono::milliseconds(20));
    devices.warm(false);
    require(!devices.take(), "A capture waited past the bound or got an unfinished device");
    w.block = false; devices.join();
    require(!devices.holding(), "A warm-up that finished after the capture gave up stored its device");
  }
  {  // A device kept while the warm-up still creates wins.
    World w; w.block = true; CaptureDevices devices(w.access());
    devices.warm(false); devices.keep(w.fakes.device(2)); w.block = false; devices.join();
    require(devices.take().get() == w.fakes.device(2).get(), "A warm-up replaced a device kept meanwhile");
  }
  {  // Nothing to warm while a device is held; a failed creation is only a cold start.
    World w; CaptureDevices devices(w.access());
    devices.keep(w.fakes.device(1)); devices.warm(true); devices.join();
    require(w.created == 0 && w.warmedAudio == 0, "A warm-up ran although a device was held");
    World broken; broken.throwOnCreate = true; CaptureDevices failing(broken.access());
    failing.warm(true); failing.join();
    require(!failing.take() && broken.warmedCapture == 0 && broken.warmedAudio == 1, "A failed device creation was not a plain cold start");
  }
}

// Windows' "capture supported" answer: asked until it is yes, then never again.
void supportCache() {
  CaptureSupportCache cache; int asked = 0; bool answer = false;
  const auto ask = [&] { ++asked; return answer; };
  require(!cache.supported(ask) && !cache.supported(ask) && asked == 2, "A no was kept instead of being asked again");
  answer = true;
  require(cache.supported(ask) && asked == 3, "A yes was not taken");
  answer = false;
  require(cache.supported(ask) && cache.supported(ask) && asked == 3, "Windows was asked again after a yes (18 ms per capture creation)");
}
// This PC's own hardware device (created and compared; nothing captured).
void realDevice() {
  Runtime runtime;  // the worker's process-wide apartment
  auto device = createCaptureDevice();
  if (!device) { std::cout << "{\"realDevice\":\"skipped: no hardware D3D11 device\"}\n"; return; }
  const auto adapter = captureDeviceAdapter(device.get()), current = defaultCaptureAdapter();
  require(adapter && current && *adapter == *current && !captureDeviceRemoved(device.get()), "A new capture device is not on the first DXGI adapter");
  CaptureDevices devices(windowsCaptureDeviceAccess());
  devices.keep(device);
  require(devices.take().get() == device.get(), "A real kept device was not handed on");
  devices.warm(false);
  const auto warmed = devices.take(); devices.join();
  require(warmed && warmed.get() != device.get() && captureDeviceAdapter(warmed.get()) == current, "The real warm-up did not produce a device on the default adapter");
  D3D_FEATURE_LEVEL level = warmed->GetFeatureLevel();
  require(level >= D3D_FEATURE_LEVEL_9_1 && (warmed->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT), "The warmed device is not the capture's BGRA device");
}

std::string code(const char* path) {
  std::ifstream file(path); std::string line, text;
  while (std::getline(file, line)) { const auto at = line.find("//"); text += (at == std::string::npos ? line : line.substr(0, at)) + '\n'; }
  return text;
}
size_t count(const std::string& text, const std::string& needle) { size_t n = 0; for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n; return n; }
// The worker's call sites (a capture cannot be created on this PC).
void callSites() {
  const auto agent = code(MULTITAP_AGENT_SOURCE), service = code(MULTITAP_SERVICE_SOURCE), media = code(MULTITAP_MEDIA_SOURCE);
  require(agent.find("class Agent") != std::string::npos && service.find("interactiveWorker") != std::string::npos && media.find("struct Capture::Modern") != std::string::npos, "the sources were read");
  require(count(media, "IsSupported()") == 1 && count(media, "static void requireSupported(){if(!graphicsCaptureSupported())") == 1, "A capture asks Windows whether capture is supported again instead of the process's answer");
  require(count(media, "hr=createCaptureHardwareDevice(device,context);") == 1 && count(media, "D3D_DRIVER_TYPE_HARDWARE") == 1, "A capture creates its device differently from the warmed one");
  require(count(agent, "captureSlot.emplace(width, height, source, captureDevices_.take())") == 1, "A stream's first capture does not start with the kept or warmed device");
  require(count(agent, "keepCaptureDevice{captureDevices_,captureSlot}") == 1 && count(agent, "devices.keep(capture->sharedDevice())") == 1, "A capture no longer leaves its device for the next one");
  require(count(agent, "captureDevices_.keep(captureSlot->sharedDevice());captureSlot.reset();") == 1 && count(agent, "captureSlot.emplace(width,height,current,captureDevices_.take())") == 1, "An encode-scale rebuild no longer keeps its device");
  const auto start = service.find("\"worker.v2.start\""), warm = service.find("worker.warmMedia(", start), change = service.find("displayV2->start(", start);
  require(start != std::string::npos && warm != std::string::npos && change != std::string::npos && warm < change, "A single-output start no longer warms media before its display change");
}
}  // namespace
int main() {
  try {
    handOver(); warmUp(); supportCache(); realDevice(); callSites();
    std::cout << "{\"kind\":\"fast-start\",\"checks\":" << checks << ",\"captured\":false,\"displayChanged\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "Fast start: " << error.what() << '\n'; return 1; }
}
