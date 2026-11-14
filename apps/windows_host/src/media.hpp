#pragma once
#include "encoder_gpu_failure.hpp"
#include "encoder_gpu_retry.hpp"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <mferror.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include "pixel_conversion_cache.hpp"
#include "capture_compose.hpp"
#include "stream_memory.hpp"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace host {
namespace cursor { class VideoCursor; }
template<class T> using Com = Microsoft::WRL::ComPtr<T>;
using Clock = std::chrono::steady_clock;
void check(HRESULT result, const char* operation);
bool interactiveDesktopAvailable();
// Dynamic availability check before allocating source/picture workspaces.
// Representation is checked independently; this is never a resolution claim.
void validateFrameMemory(int width,int height,uint64_t bgraCopies,uint64_t nv12Copies,bool even=true);
// One fresh availability sample for the complete known simultaneous forecast.
// This is admission, not a reservation; individual allocation checks remain.
void validateStreamMemory(std::span<const StreamMemoryShape> shapes);
struct Runtime { Runtime(); ~Runtime(); };
struct Frame { std::vector<uint8_t> bytes; uint64_t ptsUs = 0; bool key = false; };
// Explicit Microsoft MFT experiment settings. Production retains Baseline/
// default complexity until the measured codec and real-session acceptance
// results justify a change. In particular, High/quality100/CABAC produced
// corrupt output on the tested Windows26100 software MFT; it is a diagnostic
// case, not a shipping preset. A non-default tuning always selects the MFT.
struct EncoderTuning {
  eAVEncH264VProfile profile = eAVEncH264VProfile_Base;
  int qualityVsSpeed = -1;
  bool cabac = false;
};
// Encoder bitrate bounds. 20 Mbps was the released ceiling; hosts that
// advertise bitrate-adaptive-v1 accept up to 100 Mbps (docs/adaptive-bitrate.md).
constexpr int encoderMinimumBitrate = 500000, encoderMaximumBitrate = 100000000;
// Largest encoded frame both media framings carry (legacy 12-byte and tracks-v1
// 28-byte headers inside the 4 MiB packet and client decoder bounds).
constexpr size_t encoderMaximumFrameBytes = 4 * 1024 * 1024 - 64;

// H.264 backends (docs/x264-encoder.md, "Backends and fallback").
// Every backend emits the same contract: Annex B,
// 4:2:0 from the same NV12, BT.601 limited range, no B-frames or reordering,
// SPS/PPS before every IDR, an IDR as the first output of every encoder (stream
// start, resize epoch, new track topology) and otherwise only at the GOP length.
//  automatic: the existing CPU default, or explicitly selected GPU/adapter
//             from the protected machine config. A GPU failure falls back
//             to x264 when built, then to the Microsoft software MFT.
//             Any x264 failure falls back to
//             the Microsoft MFT for the rest of that encoder's life (one IDR,
//             same pts), recorded with a fixed EncoderFallback code.
//  x264:      x264 only; failures throw (tests, benchmarks).
//  mft:       the explicit Microsoft software H.264 MFT only.
//  nvenc/amf/qsvMft: corresponding hardware API with CPU fallback, never a
//             claim that inventory or initialization proves valid output.
enum class EncoderBackend : uint8_t { automatic, x264, mft, nvenc, amf, qsvMft };
inline constexpr bool hardwareEncoderBackend(EncoderBackend backend) { return backend == EncoderBackend::nvenc || backend == EncoderBackend::amf || backend == EncoderBackend::qsvMft; }
// GPU failures have their own field and fixed vocabulary; the released x264
// fallback codes below retain their original meaning and range.
enum EncoderGpuFallback : uint32_t {
  encoderGpuNone = 0,
  encoderGpuNotBuilt = 32,
  encoderGpuAdapterUnavailable = 33,
  encoderGpuVendorUnsupported = 34,
  encoderGpuDriverUnavailable = 35,
  encoderGpuApiUnsupported = 36,
  encoderGpuCapabilityUnsupported = 37,
  encoderGpuInitializationFailed = 38,
  encoderGpuAllocationFailed = 39,
  encoderGpuSubmissionFailed = 40,
  encoderGpuOutputTimeout = 41,
  encoderGpuOutputContract = 42,
  encoderGpuDeviceLost = 43,
  encoderGpuReconfigureFailed = 44,
};
struct EncoderHardwareInfo {
  bool initialized = false, hardwareVerified = false;
  uint64_t adapterLuid = 0;
  uint32_t vendorId = 0, deviceId = 0, gpuFallbackCode = encoderGpuNone;
  std::string adapterName;
  EncoderGpuFailure gpuFailure;
};
enum EncoderFallback : uint32_t {
  encoderFallbackNone = 0,
  encoderFallbackX264Parameters = 1,  // preset/profile/parameters refused
  encoderFallbackX264Open = 2,        // x264_encoder_open failed
  encoderFallbackX264Encode = 3,      // x264_encoder_encode reported an error
  encoderFallbackX264Output = 4,      // output broke the contract (pts, IDR, headers, delay)
  encoderFallbackX264Oversize = 5,    // a frame exceeded encoderMaximumFrameBytes
  encoderFallbackX264Memory = 6,      // allocation failure
  encoderFallbackX264Unknown = 7,
};
// Immutable metadata of a validated H264 output, before the output consumer
// can change this Encoder. This is not initialization/settings/readiness.
struct EncoderOutputObservation {
  const char* backend = "";
  EncoderHardwareInfo hardware;
  uint32_t softwareFallbackCode = encoderFallbackNone;
};
// x264 settings; production() is what automatic/x264 sessions use. Tests and
// benchmarks may pass other values through the Encoder(X264Settings) overload.
struct X264Settings {
  std::string preset = "ultrafast";
  std::string profile = "baseline";  // the MFT's own profile on the released client decoder path
  // Quality cap of the CRF + VBV rate control. 0 derives it from the target.
  double crf = 0;
  // VBV: maxrate = the negotiated target, buffer = this long at that rate,
  // never above maximumBufferKbit (keeps every frame below the packet bound).
  int bufferMs = 500;
  int maximumBufferKbit = 24000;
  int threads = 0;  // 0: one slice thread per processor of the process affinity, at most 8
  // Reference frames in the DPB (the SPS's num_ref_frames). 1 is x264's own: every client
  // decodes the stream as before. widenReferences() reopens with more for LOSS recovery.
  int referenceFrames = 1;
  // "" uses every SIMD extension x264 detects; "x86-64-v2" (benchmarks only)
  // limits it to the Proxmox default CPU type's set (no AVX/AVX2/AVX-512/BMI/FMA).
  std::string cpu;
  // Mark macroblocks whose pixels equal the previous picture's (x264 mb_info,
  // X264_MBINFO_CONSTANT) so x264 skips their analysis. false: benchmarks only.
  bool constantBlocks = true;
  // Benchmarks only: x264_param_parse(name, value) pairs applied after the
  // settings above (before the profile). Production never sets any.
  std::vector<std::pair<std::string, std::string>> parameters;
  static X264Settings production() { return {}; }
};
// The x264 rate control derived from a target (media.cpp; used at open and by
// every live setBitrate, also in MFT-only builds for tests and benchmarks).
struct X264RateControl { int maximumKbps = 0, bufferKbit = 0; double crf = 0; };
X264RateControl x264RateControl(int width, int height, int fps, int bitrate, const X264Settings& settings);
// gpu-capture-v1 (gpu_capture.hpp): one encoder input. Either the CPU path's
// NV12 bytes, or the capture's NV12 texture with a way to read it back for an
// encoder that cannot take it (CPU backends, another device, a fallback).
struct EncoderPicture {
  const uint8_t* nv12 = nullptr;
  ID3D11Texture2D* texture = nullptr;
  std::function<const uint8_t*()> read;
  // The CPU bytes, reading the texture back when there are none.
  const uint8_t* bytes() const {
    if (nv12) return nv12;
    const uint8_t* value = read ? read() : nullptr;
    if (!value) throw std::runtime_error("Encoder picture has no bytes");
    return value;
  }
};
// One backend instance. Outputs are appended to `frames` and handed to the
// caller only after the call returned, so a caller exception is never mistaken
// for an encoder failure.
class EncoderImplementation {
 public:
  virtual ~EncoderImplementation() = default;
  virtual void encode(const uint8_t* nv12, uint64_t ptsUs, std::vector<Frame>& frames) = 0;
  // Only a backend opened on the texture's device (textureDevice()) takes it
  // as it is; every other one encodes its bytes.
  virtual void encodePicture(const EncoderPicture& picture, uint64_t ptsUs, std::vector<Frame>& frames) { encode(picture.bytes(), ptsUs, frames); }
  // The device whose NV12 textures encodePicture() takes without a readback.
  virtual ID3D11Device* textureDevice() const { return nullptr; }
  virtual void finish(std::vector<Frame>& frames) = 0;
  virtual bool setBitrate(int bitrate) = 0;
  virtual int reportedBitrate() const = 0;
  virtual EncoderTuning reportedTuning() const = 0;
  // The next encode() codes an IDR (with SPS/PPS).
  virtual void requestKeyFrame() = 0;
  // udp-media-v1 LOSS: the next encode() references only the frame of `lastGoodPts` or
  // older frames of this instance's current IDR period (reference invalidation). False
  // when this encoder cannot: the caller asks for an IDR instead.
  virtual bool recover(uint64_t) { return false; }
  // A client that sent LOSS gets older references kept from the next frame on, which is
  // an IDR with the larger DPB in its SPS. Every other client keeps the stream it had: a
  // larger DPB lets some decoders hold frames back (unmeasured on Android). False: no change.
  virtual bool widenReferences() { return false; }
  virtual EncoderHardwareInfo hardwareInfo() const { return {}; }
};
class Encoder {
 public:
  // `captureDevice` (gpu-capture-v1): the capture's hardware D3D11 device; an
  // NVENC session on its adapter opens on it and then takes its textures.
  Encoder(int width, int height, int fps, int bitrate, EncoderTuning tuning = {}, EncoderBackend backend = EncoderBackend::automatic,
    std::optional<uint64_t> captureAdapter = {}, ID3D11Device* captureDevice = nullptr);
  // Explicit x264 settings (strict: failures throw).
  Encoder(int width, int height, int fps, int bitrate, const X264Settings& settings);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  void encode(const uint8_t* nv12, uint64_t ptsUs, const std::function<void(Frame)>& output);
  // gpu-capture-v1: a texture goes to a backend on its device as it is; any
  // other backend gets its bytes. A failed GPU submission falls back exactly as
  // encode() does (same pts, IDR), the CPU encoder reading the picture back.
  void encode(const EncoderPicture& picture, uint64_t ptsUs, const std::function<void(Frame)>& output);
  // The device whose textures the running backend takes, else null (CPU
  // backends, AMF/QSV, NVENC on its own device, and after a GPU fallback).
  ID3D11Device* textureDevice() const;
  void finish(const std::function<void(Frame)>& output);
  EncoderTuning reportedTuning() const;
  // Live target change on the running encoder (no re-initialisation, no forced
  // IDR): the MFT's CBR mean bitrate, or x264's VBV maxrate/buffer and CRF.
  // Returns false, keeping the previous target, if the encoder refuses it.
  bool setBitrate(int bitrate);
  int bitrate() const { return bitrate_; }
  // The encoder's own report of its target, or 0 when it cannot be read.
  int reportedBitrate() const;
  // The next encoded frame is an IDR with SPS/PPS (video was dropped on its
  // way to the broker; the decoder restarts there). A fallback MFT starts
  // with an IDR of its own.
  void requestKeyFrame() { if (impl_) impl_->requestKeyFrame(); }
  // See EncoderImplementation::recover. False: the caller asks for an IDR, which also widens
  // the DPB of an encoder that can recover, so the next LOSS can be answered without one.
  // A fallback encoder starts with its own IDR (and the narrow DPB).
  bool recover(uint64_t lastGoodPts) {
    if (!impl_) return false;
    if (impl_->recover(lastGoodPts)) return true;
    impl_->widenReferences(); return false;
  }
  // The actual current backend (never automatic). hardwareInfo separately
  // distinguishes successful initialization from validated encoded output.
  EncoderBackend backend() const { return backend_; }
  const char* backendName() const { return backend_ == EncoderBackend::x264 ? "x264" : backend_ == EncoderBackend::nvenc ? "nvenc" : backend_ == EncoderBackend::amf ? "amf" : backend_ == EncoderBackend::qsvMft ? "qsv-mft" : "mft"; }
  EncoderHardwareInfo hardwareInfo() const;
  using OutputObserver = std::function<void(const EncoderOutputObservation&)>;
  // Optional local-worker diagnostic. First actual output of each encoder
  // implementation only; observer failures cannot fail a media transaction.
  void setOutputObserver(OutputObserver observer) { outputObserver_=std::move(observer);observedOutputGeneration_=0; }
  // The EncoderFallback code of an automatic x264 -> MFT fallback, else 0.
  uint32_t fallbackCode() const { return fallback_; }
  // encoder_gpu_retry.hpp: each step of a way back to the GPU encoder (kind is
  // a GpuRetryRecord, attempt 1.., code an EncoderGpuFallback), on the encoding
  // thread. Observer failures cannot fail a media transaction.
  using GpuRetryObserver = std::function<void(uint32_t kind, uint32_t attempt, uint32_t code)>;
  void setGpuRetryObserver(GpuRetryObserver observer) { gpuRetryObserver_ = std::move(observer); }
 private:
  friend struct EncoderGpuTestAccess; // Injected in-memory backend fixtures only; no wire/configuration hook.
  void fallBack(uint32_t code);
  void fallBackGpu(uint32_t code, EncoderGpuFailure detail = {});
  // True: this picture was coded by the reopened GPU encoder, which is now the
  // running one, and its output waits in pending_. False: nothing changed.
  bool returnToGpu(const uint8_t* nv12, uint64_t ptsUs);
  void gpuRetryFailed(uint32_t kind, uint32_t code);
  void notifyGpuRetry(uint32_t kind, uint32_t code) noexcept;
  uint64_t gpuRetryNow() const;
  void deliverOutput(Frame frame,const std::function<void(Frame)>& output);
  std::unique_ptr<EncoderImplementation> impl_;
  int width_, height_, fps_, bitrate_;
  EncoderBackend backend_ = EncoderBackend::mft;
  bool strict_ = false;
  uint32_t fallback_ = encoderFallbackNone;
  uint32_t gpuFallback_ = encoderGpuNone;
  EncoderGpuFailure gpuFailure_;
  std::vector<Frame> pending_;
  OutputObserver outputObserver_;
  // GPU->x264->MFT within this instance; the only way back is the bounded
  // GPU retry below, each time with a new implementation.
  uint32_t outputGeneration_=1,observedOutputGeneration_=0;
  // encoder_gpu_retry.hpp. Opens the configured GPU encoder again at a bitrate
  // (empty: this encoder never ran on a GPU, or was given one explicitly by a
  // test); the clock (empty: steady_clock, microseconds); the attempt being
  // opened off the encoding thread, and the bitrate it was opened with. The
  // future is the last member: it is joined before anything it could use.
  using GpuReopen = std::function<std::unique_ptr<EncoderImplementation>(int bitrate)>;
  GpuReopen gpuReopen_;
  std::function<uint64_t()> gpuRetryClock_;
  GpuRetrySchedule gpuRetry_;
  GpuRetryObserver gpuRetryObserver_;
  int gpuCandidateBitrate_ = 0;
  std::future<std::unique_ptr<EncoderImplementation>> gpuCandidate_;
};
// True when this host was built with x264 (MULTITAP_X264).
bool x264EncoderAvailable();
// The existing CPU selection/fallback from the machine encoder config
// (%ProgramData%\Multitap\Host\encoder.json, {"videoEncoder":"mft"|"x264"},
// administrator-writable only), else x264 when available, else the MFT.
// gpu_h264.hpp handles explicit hardware selection from that same file.
EncoderBackend configuredEncoderBackend();
X264Settings configuredX264Settings();
// The encoder.json rule alone: "mft" selects the MFT, anything else x264.
EncoderBackend encoderBackendFromConfig(const std::string& bytes);
// Called once per automatic encoder that falls back, from its encoding thread,
// with the EncoderFallback code. Set before streaming; an empty function clears it.
void setEncoderFallbackObserver(std::function<void(uint32_t)> observer);
// A captured output's frames no longer have its size: its display mode changed
// under the capture (capture_follow.hpp). what() keeps the released text, so
// the fixed diagnostic code stays 2 (stream_prepare_diagnostics.hpp).
struct CaptureGeometryChanged : std::runtime_error { CaptureGeometryChanged() : std::runtime_error("Virtual display geometry changed; reconnect") {} };
class Capture {
 public:
  Capture(int width, int height);
  // `device` (gpu-capture-v1): a previous capture's sharedDevice() to keep (a
  // follow rebuild, so an encoder on it keeps taking this capture's textures).
  // A removed device is not kept; empty creates one as before.
  Capture(int width, int height, RECT source, std::shared_ptr<ID3D11Device> device = {});
  ~Capture();
  // On a GPU tick (gpuPicture() non-null) the returned bytes are stale; the
  // picture is the texture (encoderPicture()).
  const std::vector<uint8_t>& frame();
  int sourceWidth() const { return sourceWidth_; }
  int sourceHeight() const { return sourceHeight_; }
  bool frameUpdated() const { return frameUpdated_; }
  double lastCaptureMs() const { return lastCaptureMs_; }
  double lastConvertMs() const { return lastConvertMs_; }
  // Deep observability is opt-in; GDI has no GPU readback measurement.
  void observeReadback(bool enabled) { observeReadback_ = enabled; lastReadbackMs_.reset(); }
  std::optional<double> lastReadbackMs() const { return lastReadbackMs_; }
  const char* backend() const;
  const std::string& backendError() const { return backendError_; }
  // capture-dirty-v1 (capture_dirty.hpp): every captured monitor reports its
  // dirty rectangles and the CPU path reads back only those (WARP device on
  // Windows 11 24H2). false: whole-frame readback as before.
  bool dirtyReadback() const;
  // Tests only: false makes a new WARP capture read whole frames (a reference).
  static inline bool dirtyReadbackEnabled = true;
  // Auto-reset event WGC signals when it composed a new frame of a captured
  // output since the last frame() read; null on the GDI fallback. A capture
  // loop may wait on it to start its next tick early (FramePacer).
  HANDLE frameEvent() const;
  // Actual WGC D3D device identity; GDI/WARP have no physical GPU hint.
  std::optional<uint64_t> adapterLuid() const;
  // GPU thread priority 7 for the WGC device's work (stream_gpu_priority.hpp;
  // needs SeIncreaseBasePriorityPrivilege). false on GDI/WARP or a refusal.
  bool raiseGpuThreadPriority();
  // cursor-v1 (cursor_channel.hpp): false leaves the cursor out of every
  // capture path (direct NV12 patch, DIB, scaled source composition), so a
  // cursor-only move leaves frame(), composedDirty() and visitComposed()
  // unchanged. The default composes it exactly as before. Applies from the
  // next frame().
  void composeCursor(bool enabled) { composeCursor_ = enabled; }
  bool cursorComposed() const { return composeCursor_; }
  struct CursorSample { bool known = false; DWORD flags = 0; HCURSOR handle = nullptr; POINT position{}; };
  // The single GetCursorInfo result read by the last frame() (screen pixels).
  const CursorSample& cursorSample() const { return cursorSample_; }
  // Composed BGRA of the last frame() for lossless tiles, cursor included when composed:
  // exactly the pixels converted into its NV12, or, when the encoder is smaller
  // than the source (scaled WGC capture), the UNSCALED source those pixels were
  // scaled from. Pieces are disjoint and exactly cover each rectangle, which
  // must lie inside composedWidth() x composedHeight().
  void visitComposed(std::span<const capture::PixelRect> rects, const capture::ComposedVisitor& visitor);
  int composedWidth() const;
  int composedHeight() const;
  // Rectangles in which the composed frame can differ from the previous
  // frame(); nullopt means anywhere (new surface frame, GDI, first frame).
  std::optional<std::vector<capture::PixelRect>> composedDirty() const;
  // gpu-capture-v1 (gpu_capture.hpp). The hardware WGC device (null / empty
  // on WARP and GDI): gpuDevice() to compare, sharedDevice() to keep it.
  ID3D11Device* gpuDevice() const;
  std::shared_ptr<ID3D11Device> sharedDevice() const;
  // Arms the GPU path for an encoder whose textureDevice() is `encoderDevice`.
  // Returns the fixed record (gpu_capture.hpp), or nullopt for a CPU encoder
  // (`hardwareEncoder` false: nothing is armed, the CPU path is unchanged).
  std::optional<uint32_t> armGpuOutput(ID3D11Device* encoderDevice, bool hardwareEncoder, bool textureBackend);
  // Whether the next frame() may keep its picture on the GPU (only when armed;
  // a cursor that cannot be composed there still takes the CPU path).
  void gpuOutput(bool wanted) { gpuWanted_ = wanted; }
  // The last frame()'s NV12 texture when it ran on the GPU, else null.
  ID3D11Texture2D* gpuPicture() const;
  // Whether that GPU picture may differ from the previous frame()'s picture.
  bool gpuPictureChanged() const { return gpuChanged_; }
  // The last frame()'s picture for Encoder::encode (texture or bytes).
  EncoderPicture encoderPicture();
  // The record of a GPU path the last frame() gave up (gpu_capture.hpp), once.
  std::optional<uint32_t> takeGpuFailure() { return std::exchange(gpuFailure_, std::nullopt); }
 private:
  bool gpuFrame(Clock::time_point began);
  bool gpuWanted_ = false, gpuTick_ = false, gpuChanged_ = false;
  std::optional<uint32_t> gpuFailure_;
  void clear() noexcept;
  bool composedAtSource() const;
  // Scaled WGC path: the DIB pixels a DrawIconEx cursor is about to cover,
  // put back at the start of the next frame() (the base is not redrawn).
  void saveScaledCursor(int x, int y, int width, int height);
  void restoreScaledCursor();
  std::optional<capture::PixelRect> scaledCursor_;
  std::vector<uint8_t> scaledCursorPixels_;
  int width_, height_, sourceWidth_, sourceHeight_;
  int sourceLeft_ = 0, sourceTop_ = 0;
  bool frameUpdated_ = false, composedValid_ = false;
  bool composeCursor_ = true;
  // The size, hotspot and picture of the cursor drawn into the video (cursor_channel.hpp VideoCursor): as Windows shows it.
  std::unique_ptr<cursor::VideoCursor> videoCursor_;
  struct VideoCursorDraw { HCURSOR shape = nullptr; int x = 0, y = 0, width = 0, height = 0; };
  // The cursor showing at `position`, drawn into a picture of width x height made from the source rectangle: top-left and size in
  // that picture, and the picture of the cursor to draw at that size.
  VideoCursorDraw videoCursorDraw(HCURSOR handle, POINT position, const ICONINFO& icon, int width, int height);
  CursorSample cursorSample_;
  std::optional<capture::PixelRect> cursorBefore_, cursorAfter_;
  // Unscaled WGC path: convert readback directly and compose only the cursor.
  bool direct_ = false;
  double lastCaptureMs_ = 0, lastConvertMs_ = 0;
  bool observeReadback_ = false;
  std::optional<double> lastReadbackMs_;
  std::string backendError_;
  struct Modern;
  std::unique_ptr<Modern> modern_;
  HDC screen_ = nullptr, memory_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ previous_ = nullptr;
  uint8_t* pixels_ = nullptr;
  std::vector<uint8_t> nv12_;
  PixelConversionCache conversionCache_;
  std::array<uint64_t, 6> cursorStamp_{};
  bool cursorStampValid_ = false;
};
#ifdef MULTITAP_CAPTURE_PATH_PROBE
extern bool captureProbeLegacyPath; // Probe build only; never in the product.
#endif
// Media warm-up (media_warmup.hpp). A hardware D3D11 device on the default
// adapter with BGRA support, created exactly as a new Capture creates its own
// (no WARP fallback here: the Capture still falls back by itself); empty when
// Windows refuses one.
std::shared_ptr<ID3D11Device> createCaptureDevice();
// The adapter LUID `device` runs on, and the one a new capture device would be
// created on now (the first DXGI adapter); nullopt when unreadable.
std::optional<uint64_t> captureDeviceAdapter(ID3D11Device* device);
std::optional<uint64_t> defaultCaptureAdapter();
// A removed device (TDR, driver update) is never handed on.
bool captureDeviceRemoved(ID3D11Device* device);
// Loads Windows Graphics Capture and its D3D interop for this process. Nothing
// is captured: no monitor item, frame pool or capture session is created.
// Like a Capture, it needs a process whose multithreaded apartment outlives
// the call (the session worker's Runtime on its main thread).
void warmCaptureRuntime(ID3D11Device* device);
// Opens and closes the loopback endpoint the stream's audio uses (initialized,
// never started).
void warmLoopbackAudio();
// Windows' "screen capture supported" answer: a yes holds for the process, a
// no is asked again (the call cost 18 ms on every capture creation).
class CaptureSupportCache {
 public:
  template<class Ask> bool supported(Ask&& ask) {
    if (yes_.load()) return true;
    const bool value = bool(ask());
    if (value) yes_ = true;
    return value;
  }
 private:
  std::atomic<bool> yes_{false};
};
bool graphicsCaptureSupported();
void bgraToNv12(const uint8_t* source, uint8_t* target, int width, int height);
// `recovery` (flag bit 1) marks video that answers a udp-media-v1 LOSS. It exists only
// between the worker and its service, which takes it off before any byte leaves the Host.
std::vector<uint8_t> mediaPacket(uint8_t kind, uint64_t ptsUs, const std::vector<uint8_t>& payload, bool key = false, bool recovery = false);
class AudioCaptureWindow;
class OpusAudioEncoder;
class Loopback {
 public:
  Loopback();
  ~Loopback();
  bool available() const;
  // Why the endpoint did not open (an audio_capture_control.hpp failed record), 0 when it did.
  uint32_t failureRecord() const;
  // Until `stop`: a capture that fails or whose default render endpoint changes is opened again
  // (audio_capture_control.hpp), so the audio never ends for good before the stream does.
  // `window` (optional): each packet's capture -> handed-on time (audio_stats.hpp).
  // `opus` (optional, audio-opus-v1): packets are 10 ms Opus frames instead of PCM.
  // `record` (optional): each fixed audio_capture_control.hpp record (started, failed, restarting, device changed).
  void run(std::atomic<bool>& stop, Clock::time_point start, const std::function<void(std::vector<uint8_t>)>& output, AudioCaptureWindow* window = nullptr, OpusAudioEncoder* opus = nullptr, const std::function<void(uint32_t)>& record = {});
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
int encodeSelfTest(const std::wstring& outputPath, bool captureDesktop, int frames = 60);
}
