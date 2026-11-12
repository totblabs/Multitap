#include "remote_decoder.h"
#include "video_bounds.h"
#include "video_memory.h"
#include "../../../../native/windows_video/delayed_output.hpp"
#include "../../../../native/windows_video/color.hpp"
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace multitap {
namespace {
// Replay bounds: the packets since the last IDR a fallback can feed the software
// decoder. A hardware decoder that has produced nothing after kStallPackets
// inputs is treated as failed.
constexpr size_t kReplayPackets = 240, kReplayBytes = 24 * 1024 * 1024;
// How long to wait for the picture of a packet the software decoder has not yet released.
constexpr ULONGLONG kLatePictureMs = 250;
constexpr uint32_t kStallPackets = 30;

// The first slice NAL in an Annex B packet is an IDR (type 5). SPS, PPS and SEI
// come first, so only the head is searched.
bool ContainsIdr(const uint8_t* bytes, size_t size) {
  const size_t limit = std::min<size_t>(size, 4096);
  for (size_t i = 0; i + 3 < limit; ++i) {
    if (bytes[i] || bytes[i + 1] || bytes[i + 2] != 1) continue;
    const uint8_t type = bytes[i + 3] & 0x1f;
    if (type == 5) return true;
    if (type == 1) return false;
    i += 2;
  }
  return false;
}
}  // namespace

struct DecoderPrestart::State {
  std::mutex mutex;
  std::condition_variable wake;
  ComPtr<IMFTransform> decoder;
  bool started = false, done = false;  // done: taken or cancelled
};

std::shared_ptr<DecoderPrestart> DecoderPrestart::Begin(uint32_t width, uint32_t height, uint32_t fps) {
  if (!ValidVideoSize(width, height) || !fps) return {};
  try {
    auto state = std::make_shared<State>();
    // Detached: a cancelled start may still wait for its threads, and nobody
    // (the UI thread least of all) waits for it. It owns its COM/MF references.
    std::thread([state, width, height, fps] {
      const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      const bool mf = SUCCEEDED(com) && SUCCEEDED(MFStartup(MF_VERSION));
      ComPtr<IMFTransform> decoder;
      if (mf) {
        // The CPU decoder exactly as RemoteDecoder creates and starts one.
        RemoteDecoder starter; starter.width_ = width; starter.height_ = height; starter.options_.fps = fps;
        if (SUCCEEDED(starter.Create(false))) decoder = std::move(starter.decoder_);
      }
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->decoder = std::move(decoder); state->started = true; state->wake.notify_all();
        state->wake.wait_for(lock, kUnused, [&] { return state->done; });
        decoder = std::move(state->decoder);  // still here: not taken
      }
      decoder.Reset();
      if (mf) MFShutdown();
      if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
    return std::shared_ptr<DecoderPrestart>(new DecoderPrestart(std::move(state)));
  } catch (...) { return {}; }
}

ComPtr<IMFTransform> DecoderPrestart::Take() {
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->wake.wait(lock, [this] { return state_->started || state_->done; });
  ComPtr<IMFTransform> decoder;
  if (!state_->done) decoder = std::move(state_->decoder);
  state_->done = true; state_->wake.notify_all();
  return decoder;
}

void DecoderPrestart::Cancel() {
  { std::lock_guard<std::mutex> lock(state_->mutex); state_->done = true; }
  state_->wake.notify_all();
}

HRESULT RemoteDecoder::Initialize(uint32_t width, uint32_t height, const Options& options) {
  if (!ValidVideoSize(width, height)||!options.fps) return E_INVALIDARG;
  width_ = width; height_ = height; options_ = options;
  hardware_ = false; produced_ = false; replay_valid_ = true; replay_.clear(); replay_bytes_ = 0; inputs_without_output_ = 0;
  test_calls_ = 0; late_misses_ = 0;
  if (options_.test_fail_hardware_after >= 0) {
    const HRESULT hr = Create(false);
    if (SUCCEEDED(hr)) { hardware_ = true; tier_ = options_.tier; vendor_ = 0; }
    return hr;
  }
  if (options_.decode_device) {
    const HRESULT hardware = Create(true);
    if (SUCCEEDED(hardware)) {
      hardware_ = true; tier_ = options_.tier; vendor_ = options_.decode_device->info.vendor;
      if (options_.prestart) options_.prestart->Cancel();
      options_.prestart.reset();
      return S_OK;
    }
    // This adapter, driver or size has no usable hardware decoder: CPU decode.
    fallback_reason_ = hardware; ++fallbacks_;
  }
  return Create(false);
}

HRESULT RemoteDecoder::Create(bool hardware,bool beginStreaming) {
  decoder_.Reset(); output_buffer_.Reset(); output_capacity_ = 0; last_sample_pts_.reset();
  prestarted_ = provisional_ = false;
  if (!hardware && beginStreaming && options_.prestart) {
    const auto prestart = std::move(options_.prestart);
    // A size a new decoder refuses is refused below, at the same stage.
    if (SUCCEEDED(ValidateShape(width_, height_, options_.fps, Options{}))) decoder_ = prestart->Take();
    else prestart->Cancel();
    if (decoder_) {
      provisional_ = true;
      if (SUCCEEDED(SelectOutput())) { prestarted_ = true; validation_stage_ = "types_negotiated"; return S_OK; }
      decoder_.Reset(); provisional_ = false;
    }
  }
  validation_stage_ = "create_mft";
  HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_));
  if (FAILED(hr)) return hr;
  ComPtr<ICodecAPI> codec;
  validation_stage_ = "codec_interface";
  if (SUCCEEDED(decoder_.As(&codec))) {
    VARIANT acceleration; VariantInit(&acceleration);
    // AVDecVideoAcceleration_H264 is UINT32 (VT_UI4) for BOTH values.
    // public105 ignored the setter result. The new checked VT_BOOL FALSE
    // call made a malformed optional setting a fatal admission error.
    acceleration.vt = VT_UI4; acceleration.ulVal = hardware ? 1u : 0u;
    // This property does not prove hardware acceleration in IMFTransform;
    // the actual D3D manager/type negotiation below remains authoritative.
    // Microsoft documents no effect for this property with IMFTransform.
    // Preserve public105's best effort; it is not a decoder capability gate.
    codec->SetValue(&CODECAPI_AVDecVideoAcceleration_H264, &acceleration);
    // The Microsoft H264 decoder specifically requires VT_UI4 here (the
    // encoder uses VT_BOOL). Enforce success to avoid hidden frame buffering.
    VARIANT low_latency; VariantInit(&low_latency); low_latency.vt = VT_UI4; low_latency.ulVal = 1;
    validation_stage_ = "codec_low_latency";
    if (FAILED(hr = codec->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency))) return hr;
    if (!hardware) {
      VARIANT workers; VariantInit(&workers); workers.vt = VT_UI4; workers.ulVal = 2;
      codec->SetValue(&CODECAPI_AVDecNumWorkerThreads, &workers);
    }
  } else if (hardware) return E_NOINTERFACE;
  if (hardware) {
    ComPtr<IMFAttributes> attributes;
    validation_stage_ = "hardware_attributes";
    if (FAILED(hr = decoder_->GetAttributes(&attributes))) return hr;
    UINT32 aware = 0;
    attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
    validation_stage_ = "hardware_d3d_aware";
    if (!aware) return MF_E_UNSUPPORTED_D3D_TYPE;
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    // Sampleable decoder output: the GPU converter reads it without a copy.
    attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE);
    ComPtr<IMFDXGIDeviceManager> manager;
    validation_stage_ = "hardware_manager";
    if (FAILED(hr = options_.decode_device->Manager(&manager))) return hr;
    validation_stage_ = "hardware_manager_attach";
    if (FAILED(hr = decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())))) return hr;
  }
  ComPtr<IMFMediaType> input;
  validation_stage_ = "input_media_type";
  if (FAILED(hr = MFCreateMediaType(&input))) return hr;
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, options_.fps, 1);
  validation_stage_ = "input_type";
  if (FAILED(hr = decoder_->SetInputType(0, input.Get(), 0))) return hr;
  if (FAILED(hr = SelectOutput())) return hr;
  validation_stage_ = "types_negotiated";
  if(!beginStreaming)return S_OK;
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  return decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

bool RemoteDecoder::Accepts(uint32_t width, uint32_t height) {
  return SUCCEEDED(ValidateShape(width,height,60,Options{}));
}
HRESULT RemoteDecoder::ValidateShape(uint32_t width,uint32_t height,uint32_t fps,const Options& options,const char** observedStage){
  if(observedStage)*observedStage="arguments";
  if(!fps||!ValidVideoSize(width,height))return E_INVALIDARG;
  RemoteDecoder probe;probe.width_=width;probe.height_=height;probe.options_=options;probe.options_.fps=fps;
  const auto hr=probe.Create(bool(options.decode_device),false);
  if(observedStage)*observedStage=probe.validation_stage_;
  if(SUCCEEDED(hr)&&options.decode_device&&!options.decode_device->DecodeFits(probe.coded_width_,probe.coded_height_)){
    if(observedStage)*observedStage="hardware_budget";
    return MF_E_UNSUPPORTED_D3D_TYPE;
  }
  return hr;
}

HRESULT RemoteDecoder::SelectOutput() {
  for (DWORD index = 0; index < 32; ++index) {
    ComPtr<IMFMediaType> output;
    validation_stage_ = "output_enumerate";
    HRESULT hr = decoder_->GetOutputAvailableType(0, index, &output);
    if (FAILED(hr)) return hr;
    GUID subtype{};
    output->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (subtype != MFVideoFormat_NV12) continue;
    if (FAILED(MFGetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, &coded_width_, &coded_height_))) {
      coded_width_ = width_; coded_height_ = height_;
      MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width_, height_);
    }
    validation_stage_ = "output_dimensions";
    // A prestart's provisional type need not fit this stream; Convert checks
    // the type that actually carries its pictures.
    if ((!provisional_ && (coded_width_ < width_ || coded_height_ < height_)) || !ValidVideoSize(coded_width_, coded_height_)) return MF_E_INVALIDMEDIATYPE;
    validation_stage_ = "output_type";
    if (FAILED(hr = decoder_->SetOutputType(0, output.Get(), 0))) return hr;
    ComPtr<IMFMediaType> applied;
    validation_stage_ = "output_current_type";
    if (FAILED(hr = decoder_->GetOutputCurrentType(0, &applied))) return hr;
    UINT32 matrix = MFVideoTransferMatrix_Unknown, range = MFNominalRange_Unknown;
    applied->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
    applied->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
    // Resolution is not a colour-space declaration: legacy 1080p hosts also
    // produced 601. Only explicit metadata may select another matrix/range.
    validation_stage_ = "output_colour_matrix";
    if (matrix != MFVideoTransferMatrix_Unknown && matrix != MFVideoTransferMatrix_BT601 && matrix != MFVideoTransferMatrix_BT709) return MF_E_INVALIDMEDIATYPE;
    validation_stage_ = "output_colour_range";
    if (range != MFNominalRange_Unknown && range != MFNominalRange_16_235 && range != MFNominalRange_0_255) return MF_E_INVALIDMEDIATYPE;
    matrix_709_ = matrix == MFVideoTransferMatrix_BT709;
    full_range_ = range == MFNominalRange_0_255;
    return S_OK;
  }
  validation_stage_ = "output_nv12_missing";
  return MF_E_INVALIDMEDIATYPE;
}

HRESULT RemoteDecoder::Decode(const uint8_t* bytes, size_t size, uint64_t pts_us, const FrameCallback& callback) {
  if (!decoder_ || !bytes || !size || size > 4 * 1024 * 1024) return E_INVALIDARG;
  if (!hardware_) return DecodeOnce(bytes, size, pts_us, callback);
  if (ContainsIdr(bytes, size)) { replay_.clear(); replay_bytes_ = 0; replay_valid_ = true; }
  if (replay_valid_) {
    if (replay_.size() >= kReplayPackets || replay_bytes_ + size > kReplayBytes) { replay_valid_ = false; replay_.clear(); replay_bytes_ = 0; }
    else { replay_.push_back({std::vector<uint8_t>(bytes, bytes + size), pts_us}); replay_bytes_ += size; }
  }
  const uint64_t before = frames_out_;
  HRESULT hr = options_.test_fail_hardware_after >= 0 && test_calls_++ == options_.test_fail_hardware_after ? E_FAIL : DecodeOnce(bytes, size, pts_us, callback);
  if (frames_out_ != before) { produced_ = true; inputs_without_output_ = 0; }
  else if (SUCCEEDED(hr) && !produced_ && ++inputs_without_output_ >= kStallPackets) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  if (FAILED(hr)) return FallBack(hr, bytes, size, pts_us, callback);
  return hr;
}

HRESULT RemoteDecoder::FallBack(HRESULT reason, const uint8_t* bytes, size_t size, uint64_t pts_us, const FrameCallback& callback) {
  fallback_reason_ = reason; ++fallbacks_;
  hardware_ = false; tier_ = 0; vendor_ = 0; produced_ = false; inputs_without_output_ = 0;
  auto replay = std::move(replay_); const bool valid = replay_valid_;
  replay_.clear(); replay_bytes_ = 0; replay_valid_ = true;
  options_.decode_device.reset();
  HRESULT hr = Create(false);
  if (FAILED(hr)) return hr;
  if (!valid || replay.empty()) return DecodeOnce(bytes, size, pts_us, callback);
  // Rebuild the software decoder's references from the last IDR. The pictures
  // before the current packet were already shown; only the last one is delivered.
  discard_ = true;
  for (size_t i = 0; i + 1 < replay.size() && SUCCEEDED(hr); ++i) hr = DecodeOnce(replay[i].bytes.data(), replay[i].bytes.size(), replay[i].pts_us, callback);
  discard_ = false;
  if (FAILED(hr)) return hr;
  return DecodeOnce(replay.back().bytes.data(), replay.back().bytes.size(), replay.back().pts_us, callback);
}

HRESULT RemoteDecoder::DecodeOnce(const uint8_t* bytes, size_t size, uint64_t pts_us, const FrameCallback& callback) {
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateSample(&sample);
  if (FAILED(hr)) return hr;
  if (FAILED(hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer))) return hr;
  BYTE* target = nullptr;
  if (FAILED(hr = buffer->Lock(&target, nullptr, nullptr))) return hr;
  std::memcpy(target, bytes, size);
  buffer->Unlock(); buffer->SetCurrentLength(static_cast<DWORD>(size));
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(static_cast<LONGLONG>(pts_us) * 10);
  sample->SetSampleDuration(std::max<LONGLONG>(1,10000000 / options_.fps));
  hr = decoder_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    if (FAILED(hr = ReadOutput(callback))) return hr;
    hr = decoder_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(hr)) return hr;
  const uint64_t produced = samples_out_;
  if (FAILED(hr = ReadOutput(callback))) return hr;
  // A picture straight away: the stream yields pictures, so a later late one
  // is waited for again even after misses (e.g. a CPU stall at start-up).
  if (samples_out_ != produced) late_misses_ = 0;
  // The software MFT finishes a picture on its own worker threads: right after
  // ProcessInput, ProcessOutput can still say "need more input" although this
  // packet's picture is only a few milliseconds away (0-50 ms measured under
  // heavy CPU load, never more than the next packet's arrival otherwise). The
  // picture used to surface only with the following packet - a one-frame lag,
  // and on a still screen (no following packet) a stale picture. The stream has
  // one picture per packet, so wait briefly for it. A stream that really yields
  // no picture (two misses in a row) stops being waited for.
  const auto currentOutput=[&]{return last_sample_pts_&&*last_sample_pts_==pts_us;};
  if (!currentOutput() && (!hardware_ || options_.test_fail_hardware_after >= 0) && late_misses_ < 2) {
    const auto result=video::pumpDelayedOutput(kLatePictureMs,
      []{return uint64_t(GetTickCount64());},
      [&]{return currentOutput();},
      [&]{try{return !discard_&&options_.pending_input&&options_.pending_input();}catch(...){return false;}},
      []{Sleep(1);},
      [&]{hr=ReadOutput(callback);return SUCCEEDED(hr);});
    if(result==video::DelayedOutputResult::failed)return hr;
    // Yielding to accepted queued input is not a timed-out picture. Its later
    // output still uses the actual MFT sample timestamp, never this input PTS.
    if(result==video::DelayedOutputResult::ready)late_misses_=0;
    else if(result==video::DelayedOutputResult::timedOut)++late_misses_;
  }
  return S_OK;
}

HRESULT RemoteDecoder::Drain(const FrameCallback& callback) {
  if (!decoder_) return E_UNEXPECTED;
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  return ReadOutput(callback);
}

HRESULT RemoteDecoder::ReadOutput(const FrameCallback& callback) {
  for (int attempts = 0; attempts < 64; ++attempts) {
    MFT_OUTPUT_STREAM_INFO info{};
    HRESULT hr = decoder_->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return hr;
    ComPtr<IMFSample> sample;
    if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
      if (FAILED(hr = MFCreateSample(&sample))) return hr;
      VideoLayout layout{};if(!CheckedVideoLayout(coded_width_,coded_height_,layout))return E_INVALIDARG;
      const DWORD capacity = std::max<DWORD>(info.cbSize, layout.nv12Bytes);
      if (!output_buffer_ || output_capacity_ < capacity) {
        if(!VideoAllocationFits(capacity))return E_OUTOFMEMORY;
        output_buffer_.Reset(); output_capacity_ = 0;
        if (FAILED(hr = MFCreateMemoryBuffer(capacity, &output_buffer_))) return hr;
        output_capacity_ = capacity;
      }
      // Present the reused memory exactly like a newly created buffer.
      if (FAILED(hr = output_buffer_->SetCurrentLength(0))) return hr;
      sample->AddBuffer(output_buffer_.Get());
    }
    MFT_OUTPUT_DATA_BUFFER output{};
    output.pSample = sample.Get();
    DWORD status = 0;
    hr = decoder_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    if (output.pSample && output.pSample != sample.Get()) sample.Attach(output.pSample);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
    if (SUCCEEDED(hr)) {
      ++samples_out_; last_sample_pts_.reset();
      LONGLONG actualPts=0;
      if(sample&&SUCCEEDED(sample->GetSampleTime(&actualPts))&&actualPts>=0)last_sample_pts_=uint64_t(actualPts/10);
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      provisional_ = false;
      if (FAILED(hr = SelectOutput())) return hr;
      continue;
    }
    if (FAILED(hr)) return hr;
    if (sample && FAILED(hr = Convert(sample.Get(), callback))) return hr;
  }
  return MF_E_NOTACCEPTING;
}

bool RemoteDecoder::Recycle(std::vector<uint8_t>&& rgba) {
  if (!spare_rgba_.empty() || rgba.empty() || rgba.size() != static_cast<size_t>(width_) * height_ * 4) return false;
  spare_rgba_ = std::move(rgba);
  return true;
}

HRESULT RemoteDecoder::Convert(IMFSample* sample, const FrameCallback& callback) {
  // A picture in a prestart's provisional type smaller than this stream.
  if (coded_width_ < width_ || coded_height_ < height_) return MF_E_INVALIDMEDIATYPE;
  // Replayed pictures that were shown before the fallback are not converted.
  if (discard_) return S_OK;
  LONGLONG pts = 0; sample->GetSampleTime(&pts);
  if (options_.converter) return ConvertGpu(sample, pts, callback);
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(hr)) return hr;
  // A newer picture always supersedes a deferred one.
  deferred_.held = false;
  if (options_.defer && options_.defer(uint64_t(std::max<LONGLONG>(0, pts / 10)))) return Defer(buffer.Get(), pts);
  const size_t required = static_cast<size_t>(coded_width_) * coded_height_ * 3 / 2;
  const size_t pixels = static_cast<size_t>(width_) * height_ * 4;
  DecodedFrame frame; frame.width = width_; frame.height = height_;
  frame.pts_us = static_cast<uint64_t>(std::max<LONGLONG>(0, pts / 10));
  // Same validation order as before; a spare buffer is consumed only after it.
  const auto take = [&] {
    if (spare_rgba_.size() == pixels) { frame.rgba = std::move(spare_rgba_); spare_rgba_.clear(); }
    else {if(!VideoAllocationFits(pixels))throw std::bad_alloc();frame.rgba.resize(pixels);}
  };
  ComPtr<IMF2DBuffer> two_d;
  if (SUCCEEDED(buffer.As(&two_d))) {
    DWORD length = 0;
    if (FAILED(hr = two_d->GetContiguousLength(&length))) return hr;
    if (!VideoAllocationFits(length)) return E_OUTOFMEMORY;
    nv12_.resize(length);
    if (FAILED(hr = two_d->ContiguousCopyTo(nv12_.data(), length))) return hr;
    if (nv12_.size() < required) return MF_E_BUFFERTOOSMALL;
    take();
    video::nv12ToRgba(nv12_.data(), frame.rgba.data(), width_, height_, coded_width_, coded_height_, matrix_709_, full_range_);
  } else {
    // Contiguous system memory (the software MFT path): convert in place under
    // the lock instead of copying the whole NV12 picture first.
    BYTE* source = nullptr; DWORD length = 0;
    if (FAILED(hr = buffer->Lock(&source, nullptr, &length))) return hr;
    if (length < required) { buffer->Unlock(); return MF_E_BUFFERTOOSMALL; }
    try { take(); } catch (...) { buffer->Unlock(); throw; }
    video::nv12ToRgba(source, frame.rgba.data(), width_, height_, coded_width_, coded_height_, matrix_709_, full_range_);
    buffer->Unlock();
  }
  ++frames_out_;
  callback(std::move(frame));
  return S_OK;
}

HRESULT RemoteDecoder::Defer(IMFMediaBuffer* buffer, LONGLONG pts) {
  // Only the NV12 copy: about a third of the bytes RGBA conversion writes.
  const size_t required = static_cast<size_t>(coded_width_) * coded_height_ * 3 / 2;
  HRESULT hr = S_OK;
  ComPtr<IMF2DBuffer> two_d;
  if (SUCCEEDED(ComPtr<IMFMediaBuffer>(buffer).As(&two_d))) {
    DWORD length = 0;
    if (FAILED(hr = two_d->GetContiguousLength(&length))) return hr;
    if (!VideoAllocationFits(length)) return E_OUTOFMEMORY;
    nv12_.resize(length);
    if (FAILED(hr = two_d->ContiguousCopyTo(nv12_.data(), length))) return hr;
    if (nv12_.size() < required) return MF_E_BUFFERTOOSMALL;
  } else {
    BYTE* source = nullptr; DWORD length = 0;
    if (FAILED(hr = buffer->Lock(&source, nullptr, &length))) return hr;
    if (length < required || !VideoAllocationFits(required)) { buffer->Unlock(); return MF_E_BUFFERTOOSMALL; }
    try { nv12_.resize(required); } catch (...) { buffer->Unlock(); throw; }
    std::memcpy(nv12_.data(), source, required);
    buffer->Unlock();
  }
  deferred_ = {true, width_, height_, coded_width_, coded_height_, matrix_709_, full_range_, static_cast<uint64_t>(std::max<LONGLONG>(0, pts / 10))};
  ++deferred_count_; ++frames_out_;
  return S_OK;
}

HRESULT RemoteDecoder::ConvertDeferred(const FrameCallback& callback) {
  if (!deferred_.held) return S_FALSE;
  const auto held = deferred_; deferred_.held = false;
  DecodedFrame frame; frame.width = held.width; frame.height = held.height; frame.pts_us = held.pts_us;
  const size_t pixels = static_cast<size_t>(held.width) * held.height * 4;
  if (spare_rgba_.size() == pixels) { frame.rgba = std::move(spare_rgba_); spare_rgba_.clear(); }
  else { if (!VideoAllocationFits(pixels)) return E_OUTOFMEMORY; frame.rgba.resize(pixels); }
  video::nv12ToRgba(nv12_.data(), frame.rgba.data(), held.width, held.height, held.coded_width, held.coded_height, held.matrix_709, held.full_range);
  // A shown window's software decoder converts in place and needs no copy.
  std::vector<uint8_t>().swap(nv12_);
  callback(std::move(frame));
  return S_OK;
}

HRESULT RemoteDecoder::ConvertGpu(IMFSample* sample, LONGLONG pts, const FrameCallback& callback) {
  gpu::Converter& converter = *options_.converter;
  const gpu::Colour colour{matrix_709_, full_range_};
  const uint32_t out_width = out_width_ ? out_width_ : width_, out_height = out_height_ ? out_height_ : height_;
  DecodedFrame frame; frame.width = out_width; frame.height = out_height;
  frame.pts_us = static_cast<uint64_t>(std::max<LONGLONG>(0, pts / 10));
  HRESULT hr = S_OK;
  const auto deliver = [&](HRESULT result) -> HRESULT {
    if (result == E_PENDING) { ++gpu_dropped_; return S_OK; }  // every pooled texture is still in use: drop this picture
    if (FAILED(result)) return result;
    convert_ms_ += converter.last_ms();
    ++frames_out_;
    callback(std::move(frame));
    return S_OK;
  };
  // Hardware decoding on the converter's own device: the decoder's texture is
  // sampled directly, no copy leaves the GPU.
  if (hardware_ && options_.decode_device == converter.device()) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer.As(&dxgi)) &&
        SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&texture))) && SUCCEEDED(dxgi->GetSubresourceIndex(&slice))) {
      return deliver(converter.FromTexture(texture.Get(), slice, width_, height_, out_width, out_height, colour, frame.gpu));
    }
  }
  // System memory NV12 (software decoder, or hardware decode on another adapter
  // whose texture MF reads back for us).
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(hr = sample->ConvertToContiguousBuffer(&buffer))) return hr;
  const size_t required = static_cast<size_t>(coded_width_) * coded_height_ * 3 / 2;
  ComPtr<IMF2DBuffer> two_d;
  if (SUCCEEDED(buffer.As(&two_d))) {
    DWORD length = 0;
    if (FAILED(hr = two_d->GetContiguousLength(&length))) return hr;
    if (!VideoAllocationFits(length)) return E_OUTOFMEMORY;
    nv12_.resize(length);
    if (FAILED(hr = two_d->ContiguousCopyTo(nv12_.data(), length))) return hr;
    if (nv12_.size() < required) return MF_E_BUFFERTOOSMALL;
    return deliver(converter.FromSystem(nv12_.data(), coded_width_, coded_height_, width_, height_, out_width, out_height, colour, frame.gpu));
  }
  BYTE* source = nullptr; DWORD length = 0;
  if (FAILED(hr = buffer->Lock(&source, nullptr, &length))) return hr;
  if (length < required) { buffer->Unlock(); return MF_E_BUFFERTOOSMALL; }
  hr = converter.FromSystem(source, coded_width_, coded_height_, width_, height_, out_width, out_height, colour, frame.gpu);
  buffer->Unlock();
  return deliver(hr);
}
}
