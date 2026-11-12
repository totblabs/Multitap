// GPU presentation plumbing on the WARP software rasterizer: the D3D11 video
// processor NV12 -> BGRA conversion against the CPU reference, the pooled
// shared textures (opened by a second device the way Flutter's renderer opens
// them), scaling, the lossless overlay copy and the adapter selection
// fallbacks. WARP stands in for a GPU; it proves the plumbing and the colour
// maths, not a real GPU's speed or driver behaviour.
#include "gpu_video.h"
#include "../../../../native/windows_video/color.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace multitap;
namespace {
unsigned checks = 0;
void require(bool ok, const char* reason) { ++checks; if (!ok) throw std::runtime_error(reason); }

// Smooth gradients plus flat blocks. Chroma is constant over every 2x2 block,
// as in decoded video; the maths below compares like with like.
std::vector<uint8_t> makeNv12(uint32_t width, uint32_t height, bool full_range, uint32_t seed) {
  std::mt19937 random(seed);
  std::vector<uint8_t> data(size_t(width) * height * 3 / 2);
  const int low = full_range ? 0 : 16, high = full_range ? 255 : 235;
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) data[size_t(y) * width + x] = uint8_t(low + (high - low) * ((x + y) % 64) / 63);
  uint8_t* uv = data.data() + size_t(width) * height;
  for (uint32_t y = 0; y < height / 2; ++y) for (uint32_t x = 0; x < width / 2; ++x) {
    const bool flat = (x / 8 + y / 8) % 2 == 0;
    uv[size_t(y) * width + x * 2] = uint8_t(flat ? 128 + int(random() % 3) - 1 : 128 + int((x * 3) % 60) - 30);
    uv[size_t(y) * width + x * 2 + 1] = uint8_t(flat ? 128 + int(random() % 3) - 1 : 128 + int((y * 3) % 60) - 30);
  }
  return data;
}
struct Diff { int max = 0; double mean = 0; };
Diff compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t width, uint32_t height, uint32_t margin) {
  Diff d; double sum = 0; size_t n = 0;
  for (uint32_t y = margin; y + margin < height; ++y) for (uint32_t x = margin; x + margin < width; ++x) for (int c = 0; c < 3; ++c) {
    const int delta = std::abs(int(a[(size_t(y) * width + x) * 4 + c]) - int(b[(size_t(y) * width + x) * 4 + c]));
    d.max = std::max(d.max, delta); sum += delta; ++n;
  }
  d.mean = n ? sum / double(n) : 0; return d;
}

std::string conversion(const std::shared_ptr<gpu::Device>& device) {
  constexpr uint32_t width = 160, height = 96;
  gpu::Converter converter(device);
  std::string out;
  int worst = 0;
  for (const bool bt709 : {false, true}) for (const bool full : {false, true}) {
    const auto nv12 = makeNv12(width, height, full, bt709 ? 1 : 2);
    std::vector<uint8_t> reference(size_t(width) * height * 4);
    video::nv12ToRgba(nv12.data(), reference.data(), width, height, width, height, bt709, full);
    std::shared_ptr<gpu::Frame> frame;
    const HRESULT hr = converter.FromSystem(nv12.data(), width, height, width, height, width, height, gpu::Colour{bt709, full}, frame);
    require(SUCCEEDED(hr) && frame, "NV12 system memory conversion failed");
    std::vector<uint8_t> gpuRgba;
    require(frame->ReadRgba(gpuRgba), "Readback failed");
    for (size_t i = 3; i < gpuRgba.size(); i += 4) if (gpuRgba[i] != 255) throw std::runtime_error("Output alpha is not opaque");
    const auto d = compare(reference, gpuRgba, width, height, 0);
    worst = std::max(worst, d.max);
    out += std::string(bt709 ? "709" : "601") + (full ? "F" : "L") + ":max" + std::to_string(d.max) + "/mean" + std::to_string(d.mean).substr(0, 5) + " ";
    require(d.max == 0, "GPU conversion is not pixel-exact against the CPU reference");
  }
  // Cropped visible area: the coded height is larger than the picture.
  {
    constexpr uint32_t codedHeight = 112;
    auto nv12 = makeNv12(width, codedHeight, false, 9);
    std::vector<uint8_t> reference(size_t(width) * height * 4);
    video::nv12ToRgba(nv12.data(), reference.data(), width, height, width, codedHeight, false, false);
    std::shared_ptr<gpu::Frame> frame;
    require(SUCCEEDED(converter.FromSystem(nv12.data(), width, codedHeight, width, height, width, height, gpu::Colour{}, frame)), "Cropped conversion failed");
    std::vector<uint8_t> gpuRgba; require(frame->ReadRgba(gpuRgba) && frame->width == width && frame->height == height, "Cropped frame size");
    require(compare(reference, gpuRgba, width, height, 0).max == 0, "Cropped conversion differs from the CPU reference");
  }
  return "{\"case\":\"conversion\",\"worstChannelError\":" + std::to_string(worst) + ",\"detail\":\"" + out + "\"}";
}

std::string sharing(const std::shared_ptr<gpu::Device>& device) {
  // The reader is another device on the same adapter, like Flutter's ANGLE.
  Microsoft::WRL::ComPtr<ID3D11Device> reader; Microsoft::WRL::ComPtr<ID3D11DeviceContext> readerContext;
  require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &reader, nullptr, &readerContext)), "Second device");
  constexpr uint32_t width = 64, height = 64;
  gpu::Converter converter(device);
  std::vector<std::shared_ptr<gpu::Frame>> held;
  const auto nv12 = makeNv12(width, height, false, 3);
  std::vector<uint8_t> reference(size_t(width) * height * 4);
  video::nv12ToRgba(nv12.data(), reference.data(), width, height, width, height, false, false);
  for (size_t i = 0; i < gpu::Pool::kMaximum; ++i) {
    std::shared_ptr<gpu::Frame> frame;
    require(SUCCEEDED(converter.FromSystem(nv12.data(), width, height, width, height, width, height, gpu::Colour{}, frame)), "Pooled conversion");
    require(frame->shared != nullptr, "No shared handle");
    held.push_back(frame);
  }
  std::shared_ptr<gpu::Frame> none;
  const HRESULT exhausted = converter.FromSystem(nv12.data(), width, height, width, height, width, height, gpu::Colour{}, none);
  require(exhausted == E_PENDING && !none, "An exhausted pool did not drop the frame");
  // Open the last frame's handle on the reader and read it back.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> opened;
  require(SUCCEEDED(reader->OpenSharedResource(held.back()->shared, IID_PPV_ARGS(&opened))), "Flutter-style device cannot open the shared handle");
  D3D11_TEXTURE2D_DESC desc{}; opened->GetDesc(&desc);
  require(desc.Width == width && desc.Height == height && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "Shared texture description");
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; require(SUCCEEDED(reader->CreateTexture2D(&desc, nullptr, &staging)), "Staging");
  readerContext->CopyResource(staging.Get(), opened.Get());
  D3D11_MAPPED_SUBRESOURCE map{}; require(SUCCEEDED(readerContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)), "Map");
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const uint8_t* p = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch + x * 4; uint8_t* t = &rgba[(size_t(y) * width + x) * 4];
    t[0] = p[2]; t[1] = p[1]; t[2] = p[0]; t[3] = p[3];
  }
  readerContext->Unmap(staging.Get(), 0);
  require(compare(reference, rgba, width, height, 0).max == 0, "The other device does not see the converted picture");
  // Releasing returns a texture to the pool (first released, first reused).
  const HANDLE firstHandle = held.front()->shared;
  held.front().reset();
  std::shared_ptr<gpu::Frame> again;
  require(SUCCEEDED(converter.FromSystem(nv12.data(), width, height, width, height, width, height, gpu::Colour{}, again)) && again->shared == firstHandle, "A released texture was not reused");
  return "{\"case\":\"sharing\",\"pool\":" + std::to_string(gpu::Pool::kMaximum) + ",\"openedByOtherDevice\":true}";
}

// An NV12 texture as a hardware decoder delivers it (plane views sampled by the
// converter), when the adapter can create one; WARP may not.
std::string textureInput(const std::shared_ptr<gpu::Device>& device) {
  constexpr uint32_t width = 96, height = 64;
  const auto nv12 = makeNv12(width, height, false, 11);
  std::vector<uint8_t> reference(size_t(width) * height * 4);
  video::nv12ToRgba(nv12.data(), reference.data(), width, height, width, height, false, false);
  gpu::Converter converter(device);
  int checked = 0;
  std::string notes;
  for (const UINT arraySize : {1u, 3u}) for (const UINT bind : {UINT(D3D11_BIND_SHADER_RESOURCE), UINT(0)}) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = arraySize; desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind;
    std::vector<D3D11_SUBRESOURCE_DATA> data(arraySize, D3D11_SUBRESOURCE_DATA{nv12.data(), width, width * height});
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->device->CreateTexture2D(&desc, data.data(), &texture))) { notes += "create-failed(bind=" + std::to_string(bind) + ",n=" + std::to_string(arraySize) + ") "; continue; }
    std::shared_ptr<gpu::Frame> frame;
    const HRESULT hr = converter.FromTexture(texture.Get(), arraySize - 1, width, height, width, height, gpu::Colour{}, frame);
    if (FAILED(hr)) { notes += "convert-failed(bind=" + std::to_string(bind) + ",n=" + std::to_string(arraySize) + ",hr=" + std::to_string(hr) + ") "; continue; }  // the driver cannot sample this layout: the real path then falls back to the CPU
    std::vector<uint8_t> rgba; require(frame->ReadRgba(rgba), "Readback");
    require(compare(reference, rgba, width, height, 0).max == 0, "An NV12 texture converts differently from the CPU reference");
    ++checked;
  }
  return "{\"case\":\"textureInput\",\"layoutsConverted\":" + std::to_string(checked) + ",\"notes\":\"" + notes + "\"}";
}
std::string scalingAndOverlay(const std::shared_ptr<gpu::Device>& device) {
  gpu::Converter scaler(device);
  constexpr uint32_t sw = 64, sh = 64, lw = 128, lh = 128;
  std::vector<uint8_t> nv12(size_t(sw) * sh * 3 / 2);
  std::fill(nv12.begin(), nv12.begin() + sw * sh, uint8_t(126)); std::fill(nv12.begin() + sw * sh, nv12.end(), uint8_t(128));  // flat grey
  std::shared_ptr<gpu::Frame> video;
  require(SUCCEEDED(scaler.FromSystem(nv12.data(), sw, sh, sw, sh, sw, sh, gpu::Colour{}, video)), "Video-size frame");
  std::shared_ptr<gpu::Frame> scaled;
  require(SUCCEEDED(scaler.Scale(*video, lw, lh, scaled)) && scaled->width == lw && scaled->height == lh, "Scaled frame");
  std::vector<uint8_t> up; require(scaled->ReadRgba(up), "Scaled readback");
  require(std::abs(int(up[(64 * lw + 64) * 4]) - 128) <= 3 && std::abs(int(up[(5 * lw + 5) * 4 + 1]) - 128) <= 3, "A flat picture changed in scaling");
  // Overlay: two random 64x64 tiles copied over the scaled picture.
  gpu::Overlay overlay(device);
  require(SUCCEEDED(overlay.Configure(lw, lh)), "Overlay");
  std::mt19937 random(5);
  std::vector<uint8_t> layer(size_t(lw) * lh * 4);
  for (auto& b : layer) b = uint8_t(random());
  for (size_t i = 3; i < layer.size(); i += 4) layer[i] = 255;
  for (uint32_t ty = 0; ty < 2; ++ty) for (uint32_t tx = 0; tx < 2; ++tx) overlay.UploadTile(tx * 64, ty * 64, 64, 64, layer.data() + (size_t(ty) * 64 * lw + tx * 64) * 4, size_t(lw) * 4);
  overlay.CopyRun(*scaled, 64, 0, 64, 64);   // top-right tile
  overlay.CopyRun(*scaled, 0, 64, 128, 64);  // the whole bottom row as one run
  require(SUCCEEDED(scaler.Finish()), "Finish");
  require(scaled->ReadRgba(up), "Overlay readback");
  for (uint32_t y = 0; y < lh; ++y) for (uint32_t x = 0; x < lw; ++x) {
    const bool tile = (y < 64 && x >= 64) || y >= 64;
    for (int c = 0; c < 4; ++c) {
      const uint8_t got = up[(size_t(y) * lw + x) * 4 + c];
      if (tile) { if (got != layer[(size_t(y) * lw + x) * 4 + c]) throw std::runtime_error("A copied lossless tile is not pixel exact"); }
      else if (c < 3 && std::abs(int(got) - 128) > 3) throw std::runtime_error("A pixel outside the tiles changed");
    }
  }
  ++checks;
  // Clone keeps the pixels.
  std::shared_ptr<gpu::Frame> copy; gpu::Converter cloner(device);
  require(SUCCEEDED(cloner.Clone(*scaled, copy)), "Clone");
  std::vector<uint8_t> clone; require(copy->ReadRgba(clone) && clone == up, "Cloned frame differs");
  return "{\"case\":\"scalingAndOverlay\",\"tilesExact\":true}";
}

std::string selection() {
  gpu::SelectionOptions options; options.warp_render = true; options.allow_software_render = true;
  auto result = gpu::Select(nullptr, 1920, 1080, options);
  require(result.render && result.render->info.software, "WARP presentation was not selected for the test");
  // WARP / Basic Render Driver are never a hardware decoder.
  require(!result.decode || !result.decode->info.software, "A software adapter was chosen as a hardware decoder");
  const int expectedTier = result.decode ? (result.tier == gpu::kTierOther ? gpu::kTierOther : gpu::kTierRender) : gpu::kTierCpu;
  require(result.tier == expectedTier, "Tier does not follow the decoder");
  options.hardware_decode = false;
  result = gpu::Select(nullptr, 1920, 1080, options);
  require(!result.decode && result.tier == gpu::kTierCpu, "The user's CPU-decode setting was ignored");
  options.gpu_present = false;
  result = gpu::Select(nullptr, 1920, 1080, options);
  require(!result.render && !result.decode, "CPU presentation still created a render device");
  // Software render adapter without the test override: no GPU presentation.
  options = {}; options.warp_render = true;
  result = gpu::Select(nullptr, 1920, 1080, options);
  require(!result.render, "A software render adapter was used for GPU presentation");
  return std::string("{\"case\":\"selection\",\"hardwareDecoderOnThisPc\":") + (gpu::Select(nullptr, 1920, 1080, gpu::SelectionOptions{}).decode ? "true" : "false") + "}";
}
// tracks-v1: one output's own adapter choice, the conversion self-test the track
// engine runs at start, and the adapter lookup of a monitor.
std::string trackSelection(const std::shared_ptr<gpu::Device>& device) {
  gpu::Converter converter(device);
  require(gpu::ConversionSelfTest(converter), "The conversion self-test failed on a correct converter");
  gpu::SelectionOptions options; options.warp_render = true; options.allow_software_render = true;
  const auto plain = gpu::Select(nullptr, 1280, 720, options);
  // No monitor adapter: exactly Select.
  auto result = gpu::SelectForTrack(nullptr, nullptr, 1280, 720, options);
  require(bool(result.render) == bool(plain.render) && bool(result.decode) == bool(plain.decode) && result.tier == plain.tier, "Without a monitor adapter the track choice differs from Select");
  // A software monitor adapter (WARP / Basic Render Driver) never decodes or presents.
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory; require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
  unsigned hardware = 0, software = 0, chosenOnMonitorAdapter = 0;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    const auto info = gpu::Describe(adapter.Get());
    result = gpu::SelectForTrack(adapter.Get(), nullptr, 1280, 720, options);
    if (info.software) {
      ++software;
      require(bool(result.render) == bool(plain.render) && result.tier == plain.tier && (!result.decode || !result.decode->info.software), "A software monitor adapter changed the track choice");
    } else {
      ++hardware;
      // Chosen only if it really has an H.264 decoder; otherwise the usual order.
      if (result.decode && gpu::SameLuid(result.decode->info.luid, info.luid) && result.tier == gpu::kTierRender) {
        ++chosenOnMonitorAdapter;
        require(result.decode->SupportsH264(1280, 720) && result.render == result.decode && !result.decode->info.software, "The monitor adapter was chosen without a decoder");
      } else require(!result.decode || result.decode->SupportsH264(1280, 720), "A decoder device without an H.264 decoder was chosen");
    }
    // The user's setting off: the monitor adapter is not used for decoding.
    auto off = options; off.hardware_decode = false;
    result = gpu::SelectForTrack(adapter.Get(), nullptr, 1280, 720, off);
    require(!result.decode && result.tier == gpu::kTierCpu, "The CPU-decode setting was ignored for a track");
  }
  // The adapter that drives a monitor (null in a session without DXGI outputs).
  const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitor{sizeof(monitor)}; require(primary && GetMonitorInfoW(primary, &monitor), "Primary monitor");
  const auto owner = gpu::AdapterForRect(monitor.rcMonitor);
  if (owner) require(gpu::Describe(owner.Get()).vendor != 0 || gpu::Describe(owner.Get()).software, "The monitor adapter cannot be described");
  require(!gpu::AdapterForRect(RECT{-30000, -30000, -29990, -29990}), "A rectangle outside every monitor found an adapter");
  return std::string("{\"case\":\"trackSelection\",\"hardwareAdapters\":") + std::to_string(hardware) + ",\"softwareAdapters\":" + std::to_string(software) +
         ",\"chosenOnMonitorAdapter\":" + std::to_string(chosenOnMonitorAdapter) + ",\"primaryMonitorAdapterFound\":" + (owner ? "true" : "false") + "}";
}
}  // namespace

int main() {
  try {
    HRESULT failure = S_OK;
    const auto device = gpu::Device::Create(nullptr, true, &failure);
    if (!device) { std::cout << "{\"kind\":\"gpu-video-smoke\",\"skipped\":\"no D3D11 WARP device\",\"hr\":" << failure << "}\n"; return 0; }
    const auto a = conversion(device), b = sharing(device), c = scalingAndOverlay(device), t = textureInput(device), d = selection(), e = trackSelection(device);
    std::cout << "{\"kind\":\"gpu-video-smoke\",\"checks\":" << checks << ",\"adapter\":\"warp\",\"cases\":[" << a << ',' << b << ',' << c << ',' << t << ',' << d << ',' << e << "],\"realGpu\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "gpu video smoke: " << error.what() << '\n'; return 1; }
}
