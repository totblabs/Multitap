// x264 as the production H.264 backend of ToTBSeamless (docs/x264-encoder.md).
// x264 is GPLv2-or-later and is built from an unmodified, pinned upstream
// checkout (apps/windows_host/third_party/x264). Compiled only when the host is
// configured with MULTITAP_X264_SRC/MULTITAP_NASM; media.cpp falls back to the
// Microsoft software MFT for anything this file rejects.
//
// Bitstream contract shared with the MFT path (media.cpp): Annex B; 4:2:0 from
// the same bgraToNv12 NV12; BT.601 limited range declared in the VUI; no
// B-frames, no reordering, no frame delay (zerolatency + sliced threads); SPS
// and PPS before every IDR; IDR as the first output and otherwise only at the
// one-minute GOP (no scene-cut IDR) or when requested after the broker channel
// dropped video (requestKeyFrame); output pts = input pts, synchronously.
#include "media.hpp"
#include "x264_encoder_testing.hpp"
#include "support.hpp"
#include "x264_policy.hpp"
#include "x264_constant_blocks.hpp"
#include "reference_history.hpp"
#include <x264.h>
#include <algorithm>
#include <bit>
#include <cstdarg>

static_assert(X264_MBINFO_CONSTANT == 1, "markConstantMacroblocks writes 1 for a constant macroblock");
namespace host {
namespace x264testing {
std::atomic<SettingsProvider> settingsProvider{nullptr};
std::atomic<OpenObserver> openObserver{nullptr};
std::atomic<uint32_t> failOpenCode{0};
std::atomic<int64_t> failEncodeAtFrame{-1};
std::atomic<uint32_t> failEncodeCode{encoderFallbackX264Encode};
}
namespace {
struct X264Failure : std::runtime_error {
  uint32_t code;
  X264Failure(uint32_t value, const char* what) : std::runtime_error(what), code(value) {}
};
void quiet(void*, int, const char*, va_list) {}
int processors() {
  DWORD_PTR process = 0, system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !process) return 1;
  return std::popcount(uint64_t(process));
}
eAVEncH264VProfile profileOf(int idc) { return idc == 100 ? eAVEncH264VProfile_High : idc == 77 ? eAVEncH264VProfile_Main : eAVEncH264VProfile_Base; }

class X264Encoder final : public EncoderImplementation {
 public:
  X264Encoder(int width, int height, int fps, int bitrate, const X264Settings& settings)
      : width_(width), height_(height), fps_(fps), settings_(settings) {
    validateFrameMemory(width,height,0,settings.constantBlocks ? 13 : 12);  // +1: the previous picture kept for mb_info
    if (const uint32_t injected = x264testing::failOpenCode.load()) throw X264Failure(injected, "x264 open failure (test)");
    if (settings.preset.empty() || settings.profile.empty() || settings.threads < 0 || settings.threads > 16 || settings.bufferMs < 1 || settings.maximumBufferKbit < 1) throw X264Failure(encoderFallbackX264Parameters, "Invalid x264 settings");
    rate_ = x264RateControl(width, height, fps, bitrate, settings);
    open(std::clamp(settings.referenceFrames, 1, 16));
  }
  ~X264Encoder() override { if (handle_) x264_encoder_close(handle_); }
  // x264 cannot change its DPB live: a new instance at the current rate, whose first
  // frame is an IDR with the new SPS. The client's decoder goes on through it, as through
  // the MFT fallback's IDR. One that fails to open leaves the running one as it was.
  bool widenReferences() override {
    const int frames = recoveryReferenceFrames(width_, height_);
    if (history_->frames() >= size_t(frames)) return false;
    x264_t* running = handle_; auto history = std::move(history_); const bool cabac = cabac_;
    try { open(frames); } catch (...) { handle_ = running; history_ = std::move(history); cabac_ = cabac; return false; }
    x264_encoder_close(running); frames_ = 0;
    return true;
  }
  void open(int referenceFrames) {
    const auto& settings = settings_;
    x264_param_t p;
    if (x264_param_default_preset(&p, settings.preset.c_str(), "zerolatency") < 0) throw X264Failure(encoderFallbackX264Parameters, "x264 preset");
    p.pf_log = quiet; p.i_log_level = X264_LOG_NONE;
    p.i_width = width_; p.i_height = height_; p.i_csp = X264_CSP_NV12;
    // Constant frame rate: rate control budgets 1/fps per SUBMITTED frame like
    // the MFT (change-only submission never exceeds that rate); pts only pass through.
    p.i_fps_num = uint32_t(fps_); p.i_fps_den = 1; p.i_timebase_num = 1; p.i_timebase_den = 1000000; p.b_vfr_input = 0;
    p.i_threads = settings.threads ? settings.threads : std::clamp(processors(), 1, 8);
    p.b_sliced_threads = 1; p.i_bframe = 0; p.rc.i_lookahead = 0; p.i_sync_lookahead = 0; p.rc.b_mb_tree = 0;
    // IDR placement = the MFT path: the forced first frame and the one-minute
    // GOP only. A scene cut (page switch) never inserts an extra IDR.
    p.i_keyint_max = fps_ * 60; p.i_scenecut_threshold = 0; p.b_intra_refresh = 0; p.b_open_gop = 0;
    // udp-media-v1 LOSS: older frames to fall back on (x264_encoder_invalidate_reference).
    p.i_dpb_size = referenceFrames;
    p.b_repeat_headers = 1; p.b_annexb = 1; p.b_aud = 0;
    // What bgraToNv12 produced and the MFT output type states: square pixels,
    // BT.601 (SMPTE 170M) matrix, limited range. The MFT writes no colour
    // description (the client then assumes exactly this); x264 declares it.
    // Primaries/transfer stay unspecified, as in the MFT stream.
    p.vui.i_sar_width = 1; p.vui.i_sar_height = 1;
    p.vui.i_colmatrix = 6; p.vui.b_fullrange = 0;
    // Unchanged macroblocks (x264_constant_blocks.hpp): a P frame codes one as a
    // skip without analysing it while the reference macroblock was coded at a
    // QP no worse than this frame's; otherwise it is analysed as before, so
    // refinement still sharpens an unchanged picture. x264 does not write the flags.
    if (settings.constantBlocks) { p.analyse.b_mb_info = 1; p.analyse.b_mb_info_update = 0; }
    p.rc.i_rc_method = X264_RC_CRF; p.rc.f_rf_constant = float(rate_.crf);
    p.rc.i_vbv_max_bitrate = rate_.maximumKbps; p.rc.i_vbv_buffer_size = rate_.bufferKbit;
    if (settings.cpu == "x86-64-v2") p.cpu &= ~(X264_CPU_LZCNT | X264_CPU_AVX | X264_CPU_XOP | X264_CPU_FMA4 | X264_CPU_FMA3 | X264_CPU_BMI1 | X264_CPU_BMI2 | X264_CPU_AVX2 | X264_CPU_AVX512);
    else if (!settings.cpu.empty()) throw X264Failure(encoderFallbackX264Parameters, "Unknown x264 CPU limit");
    for (const auto& [name, value] : settings.parameters)
      if (x264_param_parse(&p, name.c_str(), value.c_str()) != 0) throw X264Failure(encoderFallbackX264Parameters, "x264 parameter");
    if (x264_param_apply_profile(&p, settings.profile.c_str()) < 0) throw X264Failure(encoderFallbackX264Parameters, "x264 profile");
    // Allocated before the handle, so a failed allocation cannot leak it.
    if (settings.constantBlocks) { previous_.resize(size_t(width_) * height_ * 3 / 2); constant_.resize(size_t(width_ + 15) / 16 * (size_t(height_ + 15) / 16)); }
    handle_ = x264_encoder_open(&p);
    if (!handle_) throw X264Failure(encoderFallbackX264Open, "x264_encoder_open failed");
    x264_param_t applied; x264_encoder_parameters(handle_, &applied);
    cabac_ = applied.b_cabac != 0;
    // The SPS's num_ref_frames without B-frames: the sliding window both sides keep.
    history_.emplace(size_t(std::max(applied.i_frame_reference, applied.i_dpb_size)));
    if (x264_encoder_maximum_delayed_frames(handle_) != 0 || applied.i_bframe != 0) { x264_encoder_close(handle_); handle_ = nullptr; throw X264Failure(encoderFallbackX264Output, "x264 would delay frames"); }
    if (const auto observer = x264testing::openObserver.load()) observer(settings.threads, applied.i_threads);
  }
  void encode(const uint8_t* nv12, uint64_t ptsUs, std::vector<Frame>& frames) override {
    if (!nv12 || ptsUs > uint64_t(INT64_MAX)) throw X264Failure(encoderFallbackX264Output, "Invalid x264 input");
    if (x264testing::failEncodeAtFrame.load() == int64_t(frames_)) throw X264Failure(x264testing::failEncodeCode.load(), "x264 encode failure (test)");
    x264_picture_t input; x264_picture_init(&input);
    input.img.i_csp = X264_CSP_NV12; input.img.i_plane = 2;
    input.img.plane[0] = const_cast<uint8_t*>(nv12); input.img.i_stride[0] = width_;
    input.img.plane[1] = const_cast<uint8_t*>(nv12) + size_t(width_) * height_; input.img.i_stride[1] = width_;
    input.i_pts = int64_t(ptsUs);
    // The first frame, and one after requestKeyFrame (dropped video), is an IDR.
    const bool forceIdr = frames_ == 0 || keyFrameRequested_;
    input.i_type = forceIdr ? X264_TYPE_IDR : X264_TYPE_AUTO;
    // Flags against the previous input (frames are coded synchronously); the first
    // picture and an IDR have none to use. x264 uses a flag only while its first
    // reference is the previous frame (analyse.c), so a recovery frame coded from an
    // older one (recover) analyses every macroblock as before.
    // x264 reads the flags during this call only (no mb_info_free: kept here).
    if (!previous_.empty()) {
      markConstantMacroblocks(nv12, previous_.data(), width_, height_, constant_.data());
      if (frames_ > 0 && !forceIdr) input.prop.mb_info = constant_.data();
    }
    x264_nal_t* nals = nullptr; int count = 0; x264_picture_t output;
    const int size = x264_encoder_encode(handle_, &nals, &count, &input, &output);
    if (size < 0) throw X264Failure(encoderFallbackX264Encode, "x264_encoder_encode failed");
    // zerolatency: every input is coded by this call, in order.
    if (size == 0 || count <= 0 || !nals || output.i_pts != input.i_pts) throw X264Failure(encoderFallbackX264Output, "x264 output does not match its input");
    if (size_t(size) > encoderMaximumFrameBytes) throw X264Failure(encoderFallbackX264Oversize, "x264 frame exceeds the media packet bound");
    // x264.h: without nalu_process the payloads are sequential in memory.
    if (nals[count - 1].p_payload + nals[count - 1].i_payload != nals[0].p_payload + size) throw X264Failure(encoderFallbackX264Output, "x264 NAL payloads are not contiguous");
    bool idr = false, sps = false, pps = false;
    for (int n = 0; n < count; ++n) {
      idr |= nals[n].i_type == NAL_SLICE_IDR; sps |= nals[n].i_type == NAL_SPS; pps |= nals[n].i_type == NAL_PPS;
      if (nals[n].i_type == NAL_SPS && nals[n].i_payload > 5 && !profileIdc_) profileIdc_ = nals[n].p_payload[nals[n].p_payload[2] == 1 ? 4 : 5];
    }
    if ((forceIdr && !idr) || (idr && !(sps && pps))) throw X264Failure(encoderFallbackX264Output, "x264 IDR without SPS/PPS");
    Frame frame; frame.bytes.assign(nals[0].p_payload, nals[0].p_payload + size); frame.ptsUs = ptsUs; frame.key = idr;
    ++frames_; keyFrameRequested_ = false; history_->encoded(ptsUs, idr);
    frames.push_back(std::move(frame));
  }
  // x264 marks every reference at or after the first stale PTS corrupt and codes the next
  // frame from what is left, or as an IDR when nothing is (encoder.c). It ignores PTS
  // before its last IDR, which history_ already refused.
  bool recover(uint64_t lastGood) override {
    const auto stale = history_->after(lastGood);
    if (!stale) return false;
    return stale->empty() || x264_encoder_invalidate_reference(handle_, int64_t(stale->front())) == 0;
  }
  void finish(std::vector<Frame>&) override {
    // Nothing is ever delayed (checked at open); flush defensively, bounded.
    for (int n = 0; n < 64 && x264_encoder_delayed_frames(handle_) > 0; ++n) {
      x264_nal_t* nals = nullptr; int count = 0; x264_picture_t output;
      if (x264_encoder_encode(handle_, &nals, &count, nullptr, &output) < 0) throw X264Failure(encoderFallbackX264Encode, "x264 flush failed");
    }
  }
  bool setBitrate(int bitrate) override {
    // Live: x264 applies VBV maxrate/buffer and CRF from its next frame, without
    // an IDR or a new SPS (docs: vbv-maxrate, vbv-bufsize and crf reconfigure).
    const auto rate = x264RateControl(width_, height_, fps_, bitrate, settings_);
    x264_param_t p; x264_encoder_parameters(handle_, &p);
    p.rc.i_vbv_max_bitrate = rate.maximumKbps; p.rc.i_vbv_buffer_size = rate.bufferKbit; p.rc.f_rf_constant = float(rate.crf);
    if (x264_encoder_reconfig(handle_, &p) < 0) return false;
    rate_ = rate; return true;
  }
  int reportedBitrate() const override { return rate_.maximumKbps * 1000; }
  void requestKeyFrame() override { keyFrameRequested_ = true; }
  EncoderTuning reportedTuning() const override {
    EncoderTuning value; value.profile = profileOf(profileIdc_); value.cabac = cabac_; return value;
  }
 private:
  x264_t* handle_ = nullptr;
  int width_, height_, fps_;
  X264Settings settings_;
  X264RateControl rate_;
  std::vector<uint8_t> previous_, constant_;  // constantBlocks: the last input picture, one flag per macroblock
  uint64_t frames_ = 0;
  int profileIdc_ = 0;
  bool cabac_ = false, keyFrameRequested_ = false;
  std::optional<ReferenceHistory> history_;
};
}

std::unique_ptr<EncoderImplementation> createX264Encoder(int width, int height, int fps, int bitrate, const X264Settings& settings) {
  return std::make_unique<X264Encoder>(width, height, fps, bitrate, settings);
}
uint32_t x264FailureCode(const std::exception& error) {
  if (const auto* failure = dynamic_cast<const X264Failure*>(&error)) return failure->code ? failure->code : uint32_t(encoderFallbackX264Unknown);
  if (dynamic_cast<const std::bad_alloc*>(&error)) return encoderFallbackX264Memory;
  return encoderFallbackX264Unknown;
}
bool x264EncoderAvailable() { return true; }
EncoderBackend encoderBackendFromConfig(const std::string& bytes) {
  try {
    const auto value = JsonParser::parse(bytes);
    if (value.get("videoEncoder").string() == "mft") return EncoderBackend::mft;
  } catch (...) {}
  return EncoderBackend::x264;
}
EncoderBackend configuredEncoderBackend() {
  try {
    const auto path = machineDirectory() / L"encoder.json";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return EncoderBackend::x264;
    // A troubleshooting switch only counts when only administrators can write it.
    if (!trustedMachineFile(path)) return EncoderBackend::x264;
    std::ifstream file(path, std::ios::binary); std::string bytes(4097, '\0');
    file.read(bytes.data(), std::streamsize(bytes.size())); bytes.resize(size_t(file.gcount()));
    if (bytes.size() > 4096) return EncoderBackend::x264;
    return encoderBackendFromConfig(bytes);
  } catch (...) { return EncoderBackend::x264; }
}
X264Settings configuredX264Settings() {
  if (const auto provider = x264testing::settingsProvider.load()) return provider();
  // Existing isolated CPU fixtures must not inherit the machine's policy.
  if (x264testing::configurationProvider.load()) return X264Settings::production();
  try {
    const auto path = machineDirectory() / L"encoder.json";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return X264Settings::production();
    return x264policy::readProtected(trustedMachineFile(path), [&] {
      std::ifstream file(path, std::ios::binary); std::string bytes(x264policy::maximumConfigBytes + 1, '\0');
      file.read(bytes.data(), std::streamsize(bytes.size())); bytes.resize(size_t(file.gcount()));
      return bytes;
    });
  } catch (...) { return X264Settings::production(); }
}
}
