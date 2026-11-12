#pragma once
#include "cursor_patch.hpp"
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

// gpu-capture-v1: the capture's picture stays on the GPU for a hardware encoder.
//
// The CPU path reads every new WGC frame back (CopyResource -> staging -> Map,
// 3.5-4 ms per 1080p frame on a test VM), converts it to NV12 on the CPU and
// hands the bytes to the encoder, whose NVENC session uploads them again. On a
// Host whose encoder is NVENC on the capture's own D3D11 device, a tick instead
// copies the WGC frame into a texture of its own (GPU to GPU), converts it to
// NV12 with the CPU path's exact integer BT.601 limited-range formulas in a
// pixel shader, and NVENC encodes that texture (registered, mapped) directly.
//
// Per feature (Capture::frame decides per tick; anything that reads the CPU
// picture keeps the CPU path while it is active):
//  - lossless tiles (tiles-v1) read the composed BGRA: CPU path while the client
//    wants tiles. A retired tile source (게임 마우스 paused them) allows GPU
//    ticks; its forced re-retirement runs on the GPU picture (losslessRetireTick).
//  - cursor in the video (no cursor-v1, the unrepresentable fallback,
//    cursor-compose-v1 while 게임 마우스 holds the mouse): composed on the GPU
//    from DrawIconEx canvases over solid bases (CursorOverlayRenderer below).
//    A cursor whose pixels are not affine in their base (an XOR with a colour)
//    makes that tick, and the ticks while it is shown, take the CPU path.
//  - capture-follow: a rebuilt capture keeps the device (Capture constructor),
//    so the encoder keeps taking its textures; when it cannot, CPU path.
//  - change detection, refresh, refinement: FrameEncodePolicy::prepareChanged
//    with the capture's own evidence (a surface or the cursor was redrawn).
//  - resize / stream-size-v1 rescale: a new capture and encoder, armed again.
//    A scaled capture (encoder smaller or larger than the output, also after a
//    follow to another mode) keeps the CPU path (its area scaler).
//  - tracks-v1 (two outputs): CPU path (not armed).
// Every GPU-less VM (x264/MFT encoder) never arms and runs exactly the CPU path.

namespace host::gpucapture {
// Fixed record, worker stage 43 (hostAction 200 stage 243). error bits 0-7: the
// reason; 8-15: the step of a converter failure; 16-31: the low 16 bits of its
// HRESULT. Only Hosts whose encoder is a hardware backend record anything.
enum GpuPathReason : uint32_t {
  gpuPathActive = 1,             // armed: the encoder takes the capture's textures
  gpuPathNoHardwareCapture = 2,  // WARP or GDI capture
  gpuPathLayout = 3,             // scaled or odd-placed capture (the CPU path scales it)
  gpuPathOtherDevice = 4,        // the encoder is not on the capture's device (another adapter)
  gpuPathConverter = 5,          // the converter could not be created
  gpuPathConvertFailed = 6,      // a GPU tick failed: CPU path for the rest of this capture
  gpuPathEncoderLeft = 7,        // the encoder fell back to a CPU backend (its gpuFallbackCode says why)
  gpuPathDeviceChanged = 8,      // a capture rebuild could not keep the encoder's device
  gpuPathBackendUnsupported = 9, // a hardware backend without texture input (none at present: NVENC, QSV and AMF take textures)
};
enum class GpuStep : uint32_t { none = 0, compiler = 1, shaders = 2, formatSupport = 3, target = 4, views = 5, states = 6, surface = 7, cursor = 8, convert = 9, readback = 10 };
inline constexpr uint32_t gpuPathStage = 43;
inline constexpr uint32_t gpuPathRecord(GpuPathReason reason, GpuStep step = GpuStep::none, HRESULT result = S_OK) {
  return uint32_t(reason) | (uint32_t(step) << 8) | ((uint32_t(result) & 0xffffu) << 16);
}
inline constexpr GpuPathReason gpuPathRecordReason(uint32_t record) { return GpuPathReason(record & 0xffu); }

// A converter step that failed (resource creation, shader compilation, Map).
struct GpuFailure : std::runtime_error {
  GpuFailure(GpuStep failed, HRESULT hr, const char* what) : std::runtime_error(what), step(failed), result(hr) {}
  GpuStep step; HRESULT result;
};
// The cursor cannot be composed on the GPU (see CursorOverlayRenderer).
struct CursorUnrepresentable : std::runtime_error { CursorUnrepresentable() : std::runtime_error("cursor_not_affine") {} };

// Whether a capture/encoder pair may use the GPU path. nullopt: the encoder is
// a CPU backend, nothing is armed and nothing is recorded (the released path).
struct GpuArmInput {
  bool hardwareEncoder = false;        // NVENC / AMF / QSV is the running backend
  bool textureBackend = false;         // that backend takes textures (NVENC)
  bool hardwareCapture = false;        // WGC on a non-WARP device
  bool directLayout = false;           // unscaled, even-placed surfaces
  bool encoderOnCaptureDevice = false; // the encoder's texture device is the capture's
};
inline std::optional<GpuPathReason> gpuArmDecision(const GpuArmInput& in) {
  if (!in.hardwareEncoder) return std::nullopt;
  if (!in.textureBackend) return gpuPathBackendUnsupported;
  if (!in.hardwareCapture) return gpuPathNoHardwareCapture;
  if (!in.directLayout) return gpuPathLayout;
  if (!in.encoderOnCaptureDevice) return gpuPathOtherDevice;
  return gpuPathActive;
}

// The stream's GPU path across its capture and encoder rebuilds: which ticks
// may keep the picture on the GPU and which fixed records are due (each
// distinct record once in a row; `record` must not throw).
class GpuPathSession {
 public:
  explicit GpuPathSession(std::function<void(uint32_t)> record = {}) : record_(std::move(record)) {}
  // After a capture and/or encoder (re)build: the arm result of the pair now
  // running (nullopt: CPU encoder). A pair that was active and now finds the
  // encoder on another device lost it to the rebuild (gpuPathDeviceChanged).
  void armed(std::optional<uint32_t> result) {
    if (!result) { active_ = false; return; }
    auto value = *result;
    if (active_ && gpuPathRecordReason(value) == gpuPathOtherDevice) value = gpuPathRecord(gpuPathDeviceChanged);
    active_ = gpuPathRecordReason(value) == gpuPathActive;
    emit(value);
  }
  // Before each frame(): whether this tick may keep its picture on the GPU.
  // An encoder that no longer takes the capture's textures (its GPU fallback)
  // ends the GPU path of this pair.
  bool tick(bool tilesWanted, bool encoderOnCaptureDevice) {
    if (!active_) return false;
    if (!encoderOnCaptureDevice) { active_ = false; emit(gpuPathRecord(gpuPathEncoderLeft)); return false; }
    return !tilesWanted;
  }
  // The capture gave up its GPU path (Capture::takeGpuFailure).
  void failed(uint32_t value) { active_ = false; emit(value); }
  bool active() const { return active_; }
 private:
  void emit(uint32_t value) {
    if (last_ == value) return;
    last_ = value;
    if (record_) try { record_(value); } catch (...) { }
  }
  std::function<void(uint32_t)> record_;
  std::optional<uint32_t> last_;
  bool active_ = false;
};

// floor((base * (white - black) + 127) / 255) + black: what a DrawIconEx pixel
// channel gives over `base` when it gave `black` over 0 and `white` over 255.
// Exact for transparent (0,255), black (0,0), white (255,255) and inverting
// (255,0) pixels; within 1 of DrawIconEx's alpha blending. The pixel shader
// (gpu_capture.cpp) computes the same integers.
inline int cursorAffine(int black, int white, int base) {
  const int n = base * (white - black) + 127;
  return black + (n >= 0 ? n / 255 : -((254 - n) / 255));
}
// BGRA rows: the pixel at (0,0) and the row pitch in bytes.
struct CanvasView { const uint8_t* pixels = nullptr; size_t stride = 0; };
// Every colour channel of the grey canvases within `tolerance` of the affine
// prediction from the black and white canvases (width x height pixels).
inline bool cursorCanvasesAffine(CanvasView black, CanvasView white, CanvasView grey1, uint8_t grey1Base,
                                 CanvasView grey2, uint8_t grey2Base, int width, int height, int tolerance = 1) {
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) for (int c = 0; c < 3; ++c) {
    const auto at = [&](const CanvasView& view) { return int(view.pixels[size_t(y) * view.stride + size_t(x) * 4 + size_t(c)]); };
    const int b = at(black), w = at(white);
    if (std::abs(cursorAffine(b, w, grey1Base) - at(grey1)) > tolerance) return false;
    if (std::abs(cursorAffine(b, w, grey2Base) - at(grey2)) > tolerance) return false;
  }
  return true;
}

// The cursor's even patch (exactly the CPU path's, cursor_patch.hpp) and its
// DrawIconEx result over a black and over a white base (BGRA, width*4 per row).
struct CursorOverlay {
  capture::PixelRect patch;
  std::vector<uint8_t> black, white;
};
// Draws `draw` like Nv12CursorPatch (same canvas geometry, brush alignment and
// DrawIconEx call) over four solid bases. The greys check that every pixel is
// affine in its base (once per shape and size). Throws CursorUnrepresentable
// when it is not, or when the canvases cannot be made.
class CursorOverlayRenderer {
 public:
  static constexpr uint8_t grey1 = 0x55, grey2 = 0xaa;
  // nullptr: the cursor lies outside the frame (nothing to compose).
  const CursorOverlay* render(const capture::CursorDraw& draw, int frameWidth, int frameHeight, HDC compatible) {
    const auto patch = capture::evenCursorPatch(draw.x, draw.y, draw.width, draw.height, frameWidth, frameHeight);
    if (!patch) return nullptr;
    const int width = patch->width(), height = patch->height();
    const bool validate = !(validatedShape_ == draw.shape && validatedWidth_ == draw.width && validatedHeight_ == draw.height);
    const std::array<uint8_t, 4> bases{0, 255, grey1, grey2};
    const size_t count = validate ? 4 : 2;
    for (size_t k = 0; k < count; ++k) {
      auto& canvas = canvases_[k];
      if (!canvas.reserve(width, height, compatible)) throw CursorUnrepresentable();
      GdiFlush();
      for (int y = 0; y < height; ++y) {
        auto* row = canvas.pixels() + size_t(y) * canvas.stride();
        for (int x = 0; x < width; ++x) { row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = bases[k]; row[x * 4 + 3] = 255; }
      }
      canvas.align(patch->left, patch->top);
      (void)DrawIconEx(canvas.dc(), draw.x - patch->left, draw.y - patch->top, draw.shape, draw.width, draw.height, 0, nullptr, DI_NORMAL);
    }
    GdiFlush();
    if (validate) {
      validatedShape_ = nullptr;
      const auto view = [&](size_t k) { return CanvasView{canvases_[k].pixels(), canvases_[k].stride()}; };
      if (!cursorCanvasesAffine(view(0), view(1), view(2), grey1, view(3), grey2, width, height)) throw CursorUnrepresentable();
      // A cursor clipped by the frame edge was checked only where it shows.
      const bool whole = patch->left <= draw.x && patch->top <= draw.y && int64_t(patch->right) >= int64_t(draw.x) + draw.width && int64_t(patch->bottom) >= int64_t(draw.y) + draw.height;
      if (whole) { validatedShape_ = draw.shape; validatedWidth_ = draw.width; validatedHeight_ = draw.height; }
    }
    overlay_.patch = *patch;
    const size_t row = size_t(width) * 4;
    overlay_.black.resize(row * size_t(height)); overlay_.white.resize(row * size_t(height));
    for (int y = 0; y < height; ++y) {
      std::memcpy(overlay_.black.data() + size_t(y) * row, canvases_[0].pixels() + size_t(y) * canvases_[0].stride(), row);
      std::memcpy(overlay_.white.data() + size_t(y) * row, canvases_[1].pixels() + size_t(y) * canvases_[1].stride(), row);
    }
    return &overlay_;
  }
 private:
  std::array<capture::CursorCanvas, 4> canvases_;
  CursorOverlay overlay_;
  HCURSOR validatedShape_ = nullptr;
  int validatedWidth_ = 0, validatedHeight_ = 0;
};

// The GPU half of a direct (unscaled, even-placed) capture on one D3D11
// device: the newest frame of every surface in a texture of its own and the
// NV12 picture converted from them. Single thread (the capture's); the device
// may be multithread-protected for an encoder on it.
class FrameComposer {
 public:
  // `surfaces`: each surface's even rectangle in the width x height frame.
  // Throws GpuFailure (shaders, NV12 render targets, allocation).
  FrameComposer(ID3D11Device* device, int width, int height, std::vector<capture::PixelRect> surfaces);
  ~FrameComposer();
  FrameComposer(const FrameComposer&) = delete;
  FrameComposer& operator=(const FrameComposer&) = delete;
  // Keeps surface `index`'s newest frame: a GPU copy of `frame` (B8G8R8A8, the
  // surface's size, of this device; a staging texture also works). Never
  // allocates, so it cannot fail; `frame` may go back to its pool afterwards.
  void surfaceFrame(size_t index, ID3D11Texture2D* frame);
  bool hasFrame(size_t index) const;
  // Surface `index`'s copy (B8G8R8A8, DEFAULT usage): what the CPU path reads
  // back after GPU ticks.
  ID3D11Texture2D* latest(size_t index) const;
  // Brings the picture up to date: every surface with a newer frame, then the
  // cursor (`overlay`, or none: a previous one is removed). Returns whether the
  // picture may differ from the previous compose(). Throws GpuFailure.
  bool compose(const CursorOverlay* overlay);
  // The NV12 picture (DEFAULT usage, render target), valid while this lives.
  ID3D11Texture2D* picture() const;
  // The picture's bytes (width x height luma, then interleaved UV) for a CPU
  // encoder after a fallback, and tests. Waits for the GPU. Throws GpuFailure.
  void read(std::span<uint8_t> nv12);
  int width() const;
  int height() const;
  // Tests only: false skips removing the previous cursor (fail-without-fix).
  static inline bool restorePreviousCursor = true;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
