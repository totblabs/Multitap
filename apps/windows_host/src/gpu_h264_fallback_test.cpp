// GPU selection/failure injection with actual CPU H264 and actual client
// decoder interoperability. No desktop, GPU device or driver API is opened.
#include "gpu_h264.hpp"
#include "encoder_diagnostics.hpp"
#include "support.hpp"
#include "remote_decoder.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace host {
std::unique_ptr<EncoderImplementation> createX264Encoder(int, int, int, int, const X264Settings&);
struct EncoderGpuTestAccess {
  static void install(Encoder& encoder, std::unique_ptr<EncoderImplementation> implementation) {
    encoder.impl_ = std::move(implementation); encoder.backend_ = EncoderBackend::nvenc; encoder.gpuFallback_ = encoderGpuNone;
  }
  static void failedInitialization(Encoder& encoder) { encoder.fallBackGpu(encoderGpuInitializationFailed); }

};
}
namespace {
int checks = 0;
void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
constexpr int width = 320, height = 180, fps = 60, bitrate = 4000000;
class InjectedGpu final : public host::EncoderImplementation {
 public:
  explicit InjectedGpu(int failAfter = -1, bool refuseRate = false, host::gpu::Adapter adapter = {UINT64_MAX, 0x10de, 1, "Injected GPU fixture"}, bool corrupt = false,
      host::EncoderGpuFailure failure = {}, uint32_t failureCode = host::encoderGpuDeviceLost)
      : cpu_(host::createX264Encoder(width, height, fps, bitrate, {})), failAfter_(failAfter), refuseRate_(refuseRate), corrupt_(corrupt), adapter_(std::move(adapter)), failure_(failure), failureCode_(failureCode) {}
  void encode(const uint8_t* pixels, uint64_t pts, std::vector<host::Frame>& output) override {
    if (count_++ == failAfter_) throw host::gpu::Failure(failureCode_, "injected GPU failure", failure_);
    cpu_->encode(pixels, pts, output);
    require(output.size() == 1, "Actual CPU fixture delayed output");
    const auto unit = host::gpu::inspectAnnexB(output.front().bytes, host::encoderMaximumFrameBytes);
    require(unit.valid, "Actual CPU fixture did not emit AnnexB"); verified_ = true;
    if(corrupt_) output.front().bytes={1,2,3};
  }
  void finish(std::vector<host::Frame>& output) override { cpu_->finish(output); }
  bool setBitrate(int value) override {
    if (refuseRate_ && failure_.site) throw host::gpu::Failure(host::encoderGpuReconfigureFailed, "injected native rate failure", failure_);
    return !refuseRate_ && cpu_->setBitrate(value);
  }
  int reportedBitrate() const override { return cpu_->reportedBitrate(); }
  host::EncoderTuning reportedTuning() const override { return cpu_->reportedTuning(); }
  void requestKeyFrame() override { cpu_->requestKeyFrame(); }
  host::EncoderHardwareInfo hardwareInfo() const override {
    host::EncoderHardwareInfo result; result.initialized = true; result.hardwareVerified = verified_;
    result.adapterLuid = adapter_.luid; result.vendorId = adapter_.vendorId; result.deviceId = adapter_.deviceId; result.adapterName = adapter_.description; return result;
  }
 private:
  std::unique_ptr<host::EncoderImplementation> cpu_;
  int failAfter_, count_ = 0; bool refuseRate_ = false, corrupt_ = false, verified_ = false;
  host::gpu::Adapter adapter_;
  host::EncoderGpuFailure failure_;
  uint32_t failureCode_;
};
std::vector<uint8_t> pixels(int frame) {
  std::vector<uint8_t> result(size_t(width) * height * 3 / 2, 128);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) result[size_t(y) * width + x] = uint8_t(16 + (x + y + frame * 7) % 210);
  return result;
}
void interoperability(bool rateFailure) {
  host::Encoder encoder(width, height, fps, bitrate, {}, host::EncoderBackend::x264);
  host::EncoderGpuTestAccess::install(encoder, std::make_unique<InjectedGpu>(rateFailure ? -1 : 3, rateFailure));
  require(encoder.hardwareInfo().initialized && !encoder.hardwareInfo().hardwareVerified, "Initialization was incorrectly treated as verified encoding");
  multitap::RemoteDecoder decoder; host::check(decoder.Initialize(width, height), "Initialize actual client software decoder");
  std::vector<uint64_t> encoded, decoded, keys;
  const auto shown = [&](multitap::DecodedFrame&& frame) { require(frame.width == width && frame.height == height, "Fallback changed actual decoded geometry"); decoded.push_back(frame.pts_us); };
  const auto output = [&](host::Frame frame) {
    const auto unit = host::gpu::inspectAnnexB(frame.bytes, host::encoderMaximumFrameBytes);
    require(unit.valid && frame.key == unit.idr, "Output contract changed across GPU fallback");
    if (frame.key) { require(unit.sps && unit.pps, "Fallback IDR lost SPS/PPS"); keys.push_back(frame.ptsUs); }
    encoded.push_back(frame.ptsUs); host::check(decoder.Decode(frame.bytes.data(), frame.bytes.size(), frame.ptsUs, shown), "Decode actual H264 through GPU-to-CPU transition");
  };
  for (int frame = 0; frame < 12; ++frame) {
    if (rateFailure && frame == 3) require(encoder.setBitrate(8000000) && encoder.reportedBitrate() == 8000000, "GPU bitrate refusal did not apply CPU fallback target");
    const auto source = pixels(frame); const uint64_t pts = uint64_t(frame) * 1000000 / fps;
    const auto before = encoded.size(); encoder.encode(source.data(), pts, output);
    require(encoded.size() == before + 1 && encoded.back() == pts, "Fallback dropped, duplicated or retimestamped the failed picture");
    if (frame == 0) require(encoder.hardwareInfo().hardwareVerified, "Injected successful encoder output did not update its fixture evidence");
    if (frame >= 3) {
      const auto state = encoder.hardwareInfo();
      require(encoder.backend() == host::EncoderBackend::x264 && state.initialized && !state.hardwareVerified && state.adapterLuid == 0 && state.vendorId == 0 && state.deviceId == 0 && state.adapterName.empty(), "CPU fallback retained stale hardware identity");
      require(state.gpuFallbackCode == (rateFailure ? host::encoderGpuReconfigureFailed : host::encoderGpuDeviceLost), "Fixed GPU fallback reason was lost");
    }
  }
  encoder.finish(output); host::check(decoder.Drain(shown), "Drain actual client decoder");
  require(decoded == encoded && decoded.size() == 12, "Actual client decoder failed across runtime GPU fallback");
  require(keys == std::vector<uint64_t>{0, 50000}, "Fallback did not create a new IDR exactly at the failed input PTS");
}
void candidatePolicy() {
  using namespace host::gpu;
  const std::vector<Adapter> adapters{{1,0x1002,1,"AMD fixture"},{2,0x10de,2,"NVIDIA busy fixture"},{3,0x10de,3,"NVIDIA selected fixture"}};
  std::vector<uint64_t> opened;
  const auto open = [&](const Adapter& adapter) -> std::unique_ptr<host::EncoderImplementation> {
    opened.push_back(adapter.luid); if (adapter.luid <= 2) throw Failure(host::encoderGpuInitializationFailed, "injected unavailable/busy device"); return std::make_unique<InjectedGpu>(-1, false, adapter);
  };
  auto implementation = createFromAdapters(adapters, {Mode::automatic, {}}, open);
  require(bool(implementation) && opened == std::vector<uint64_t>{1,2,3}, "Automatic candidate fallback did not honor actual init failures across vendors");
  opened.clear(); bool failed = false;
  try { createFromAdapters(adapters, {Mode::nvenc, 2}, open); } catch (const Failure& error) { failed = error.code() == host::encoderGpuInitializationFailed; }
  require(failed && opened == std::vector<uint64_t>{2}, "Explicit adapter silently migrated to a different GPU");
  opened.clear(); failed = false;
  try { createFromAdapters(adapters, {Mode::nvenc, 1}, open); } catch (const Failure& error) { failed = error.code() == host::encoderGpuVendorUnsupported; }
  require(failed && opened.empty(), "AMD inventory was falsely treated as a supported NVENC encoder");
  failed = false;
  try { createFromAdapters(adapters, {Mode::nvenc, 3}, [](const Adapter&) { return std::make_unique<InjectedGpu>(); }); }
  catch (const Failure& error) { failed = error.code() == host::encoderGpuInitializationFailed; }
  require(failed, "Backend success for a different physical adapter was accepted");
  require(selectionFromConfig(R"({"videoEncoder":"x264","gpuAdapter":"bad"})").mode == Mode::disabled, "Existing CPU selection attempted a GPU");
  const auto selection = selectionFromConfig(R"({"videoEncoder":"gpu","gpuAdapter":"ffffffff:ffffffff"})");
  require(selection.valid && selection.adapterLuid == UINT64_MAX, "Protected configuration lost 64-bit adapter identity");
  require(!selectionFromConfig(R"({"videoEncoder":"nvenc","gpuAdapter":123})").valid, "Invalid explicit adapter silently became automatic");
}
void callerFailure() {
  host::Encoder encoder(width, height, fps, bitrate, {}, host::EncoderBackend::x264);
  host::EncoderGpuTestAccess::install(encoder, std::make_unique<InjectedGpu>());
  bool threw = false; const auto source = pixels(0);
  try { encoder.encode(source.data(), 0, [](host::Frame) { throw std::runtime_error("caller_send_failure"); }); }
  catch (const std::runtime_error& error) { threw = std::string(error.what()) == "caller_send_failure"; }
  require(threw && encoder.backend() == host::EncoderBackend::nvenc && encoder.hardwareInfo().gpuFallbackCode == 0, "Caller/transport failure was incorrectly attributed to GPU encoding");
  host::EncoderGpuTestAccess::failedInitialization(encoder);
  require(encoder.backend() == host::EncoderBackend::x264 && !encoder.hardwareInfo().hardwareVerified && encoder.hardwareInfo().gpuFallbackCode == host::encoderGpuInitializationFailed, "Initialization failure did not clear GPU state and choose CPU");
}
void firstOutputSelection() {
  using namespace host::gpu;
  const std::vector<Adapter> adapters{{9,0x8086,1,"Intel fixture"},{7,0x1002,2,"AMD fixture"},{5,0x10de,3,"NVIDIA fixture"},{3,0x10de,4,"Other fixture"}};
  const auto order=orderedCandidates(adapters,{Mode::automatic,{}},{7,9});
  require(order.size()==3&&order[0].luid==7&&order[1].luid==9&&order[2].luid==5,"Capture then valid last-good then actual OS preference was not bounded");
  const auto missing=orderedCandidates(adapters,{Mode::automatic,{}},{123,456});
  require(missing[0].luid==9&&missing[1].luid==7,"Stale hints fabricated an adapter or replaced OS preference by numeric LUID");
  const auto manual=orderedCandidates(adapters,{Mode::automatic,5},{7,9});
  require(manual.size()==1&&manual[0].luid==5,"Automatic hints overrode an explicit manual adapter");
  std::vector<uint64_t> opened,verified;
  auto candidate=createVerifiedFromAdapters(adapters,{Mode::automatic,{}},{7,9},[&](const Adapter& a){opened.push_back(a.luid);return std::make_unique<InjectedGpu>(a.luid==7?0:1,false,a);},[&](uint64_t luid,bool valid){if(valid)verified.push_back(luid);});
  require(opened==std::vector<uint64_t>{7}&&verified.empty()&&!candidate->hardwareInfo().hardwareVerified,"Initialization was logged as GPU output success");
  const auto source=pixels(0);std::vector<host::Frame> frames;candidate->encode(source.data(),12345,frames);
  require(opened==std::vector<uint64_t>{7,9}&&verified==std::vector<uint64_t>{9},"First-output failure did not try the next actual candidate");
  require(frames.size()==1&&frames[0].ptsUs==12345&&frames[0].key&&candidate->hardwareInfo().adapterLuid==9,"Candidate handoff published failed bytes or lost first IDR/PTS");
  multitap::RemoteDecoder decoder;host::check(decoder.Initialize(width,height),"Initialize candidate decoder");size_t decoded=0;
  host::check(decoder.Decode(frames[0].bytes.data(),frames[0].bytes.size(),frames[0].ptsUs,[&](multitap::DecodedFrame&& frame){require(frame.width==width&&frame.height==height&&frame.pts_us==12345,"Selected output did not interoperate with actual client decoder");++decoded;}),"Decode selected first output");
  host::check(decoder.Drain([&](multitap::DecodedFrame&& frame){require(frame.pts_us==12345,"Candidate drain changed PTS");++decoded;}),"Drain selected first output");require(decoded==1,"Candidate output did not reach actual software decoder");
  frames.clear();bool failed=false;try{candidate->encode(source.data(),29012,frames);}catch(const Failure& error){failed=error.code()==host::encoderGpuDeviceLost;}
  require(failed&&frames.empty()&&opened==std::vector<uint64_t>{7,9},"A published GPU epoch oscillated to another adapter after runtime failure");
  opened.clear();candidate=createVerifiedFromAdapters(adapters,{Mode::automatic,5},{7,9},[&](const Adapter& a){opened.push_back(a.luid);return std::make_unique<InjectedGpu>(0,false,a);});
  failed=false;try{candidate->encode(source.data(),12345,frames);}catch(const Failure&){failed=true;}
  require(failed&&frames.empty()&&opened==std::vector<uint64_t>{5},"Manual first-output failure silently migrated to another GPU");
  require(selectionFromConfig(R"({"videoEncoder":"auto"})").mode==Mode::automatic,"Explicit auto selection was not loaded");
  require(!selectionFromConfig(R"({"videoEncoder":"auto","gpuAdapter":"00000000:00000005"})").valid,"Automatic choice silently retained a conflicting manual identity");
  opened.clear();frames.clear();candidate=createVerifiedFromAdapters(adapters,{Mode::automatic,{}},{7,9},[&](const Adapter& a){opened.push_back(a.luid);return std::make_unique<InjectedGpu>(-1,false,a,a.luid==7);});
  candidate->encode(source.data(),45678,frames);
  require(frames.size()==1&&opened==std::vector<uint64_t>{7,9}&&inspectAnnexB(frames[0].bytes,host::encoderMaximumFrameBytes).valid,"Malformed first H264 bytes were accepted as GPU success instead of retrying a bounded candidate");
  opened.clear();candidate=createVerifiedFromAdapters(adapters,{Mode::automatic,{}},{7,9},[&](const Adapter& a){opened.push_back(a.luid);return std::make_unique<InjectedGpu>(a.luid==7?0:-1,false,a);});
  host::Encoder selectedEncoder(width,height,fps,bitrate,{},host::EncoderBackend::x264);host::EncoderGpuTestAccess::install(selectedEncoder,std::move(candidate));
  size_t published=0;selectedEncoder.encode(source.data(),56789,[&](host::Frame frame){require(frame.ptsUs==56789&&frame.key,"Selected Encoder lost first keyframe");++published;});
  require(published==1&&selectedEncoder.backend()==host::EncoderBackend::qsvMft&&selectedEncoder.hardwareInfo().vendorId==0x8086,"Public Encoder backend retained the rejected vendor instead of actual first-output adapter");

}
void actualOutputObservation(){
  static_assert(!std::is_move_constructible_v<host::Encoder>&&!std::is_move_assignable_v<host::Encoder>,"Encoder observers must not become dangling through implicit moves");
  const auto source=pixels(0);std::vector<host::EncoderOutputObservation> observations;
  host::Encoder encoder(width,height,fps,bitrate,{},host::EncoderBackend::x264);
  host::EncoderGpuTestAccess::install(encoder,std::make_unique<InjectedGpu>(-1,true));
  encoder.setOutputObserver([&](const auto& value){observations.push_back(value);});
  require(observations.empty(),"Initialization or observer registration was mistaken for output");
  encoder.encode(source.data(),0,[](host::Frame){});
  require(observations.size()==1&&std::string(observations[0].backend)=="nvenc"&&observations[0].hardware.hardwareVerified,"First actual short-session H264 output was left waiting for a statistics timer");
  encoder.encode(source.data(),16667,[](host::Frame){});
  require(observations.size()==1,"Ordinary output repeated the same encoder-use event");
  require(encoder.setBitrate(8000000)&&encoder.backend()==host::EncoderBackend::x264,"Injected rate failure did not select CPU");
  require(observations.size()==1&&std::string(observations.back().backend)=="nvenc","CPU initialization without a CPU frame replaced the actual GPU-use record");
  encoder.encode(source.data(),33334,[](host::Frame){});
  require(observations.size()==2&&std::string(observations.back().backend)=="x264"&&!observations.back().hardware.hardwareVerified&&observations.back().hardware.gpuFallbackCode==host::encoderGpuReconfigureFailed,"First actual CPU fallback output did not record its own snapshot");
  encoder.setOutputObserver([](const auto&){throw std::runtime_error("injected optional observer failure");});encoder.requestKeyFrame();size_t outputCount=0;
  encoder.encode(source.data(),50001,[&](host::Frame){++outputCount;});require(outputCount==1&&encoder.backend()==host::EncoderBackend::x264,"Optional diagnostic exception failed or reclassified actual media");

  std::vector<int> epochs;std::optional<host::Encoder> fresh;
  for(int epoch=1;epoch<=2;++epoch){fresh.emplace(width,height,fps,bitrate,host::EncoderTuning{},host::EncoderBackend::x264);fresh->setOutputObserver([&,epoch](const auto&){epochs.push_back(epoch);});fresh->encode(source.data(),0,[](host::Frame){});fresh.reset();}
  require(epochs==std::vector<int>{1,2},"Fresh/resize replacement lost observer setup or retained an old epoch capture");

  observations.clear();host::Encoder reentrant(width,height,fps,bitrate,{},host::EncoderBackend::x264);host::EncoderGpuTestAccess::install(reentrant,std::make_unique<InjectedGpu>(-1,true));
  reentrant.setOutputObserver([&](const auto& value){observations.push_back(value);});
  reentrant.encode(source.data(),0,[&](host::Frame){require(reentrant.setBitrate(8000000),"Reentrant test rate change failed");});
  require(reentrant.backend()==host::EncoderBackend::x264&&observations.size()==1&&std::string(observations[0].backend)=="nvenc","Consumer callback changed the identity of the frame already produced");
  reentrant.encode(source.data(),16667,[](host::Frame){});
  require(observations.size()==2&&std::string(observations[1].backend)=="x264","Post-consumer CPU generation was incorrectly marked as already observed");

  host::Encoder refused(width,height,fps,bitrate,{},host::EncoderBackend::x264);size_t refusedNotices=0;refused.setOutputObserver([&](const auto&){++refusedNotices;});bool failed=false;
  try{refused.encode(source.data(),0,[](host::Frame){throw std::runtime_error("injected media consumer rejection");});}catch(const std::runtime_error&){failed=true;}
  require(failed&&refusedNotices==0,"Rejected output was reported as successfully handed off");

  const auto info=host::encoderDiagnostic(observations[0].backend,observations[0].hardware,observations[0].softwareFallbackCode);
  const auto packet=host::encoderUsagePacket({"12345678-1234-4234-8234-123456789abc",0,{"0",1,info}});
  auto parsed=host::JsonParser::parse(packet);require(host::parseEncoderUsagePacket(parsed).has_value(),"Actual private JSON parser rejected valid exact output evidence");
  auto bad=parsed;std::get<host::Json::Object>(bad.value)["epoch"].value=std::string("1");require(!host::parseEncoderUsagePacket(bad),"String epoch widened private observation schema");
  bad=parsed;std::get<host::Json::Object>(bad.value)["extra"].value=true;require(!host::parseEncoderUsagePacket(bad),"Extra private observation fields were accepted");
}
void failureEvidence() {
  using namespace host;
  using namespace host::gpu;
  const std::vector<Adapter> adapters{{7,0x1002,0xa7,"AMD injected candidate"},{9,0x10de,0xb9,"NVIDIA injected candidate"}};
  // Provider-supplied identity is deliberately wrong. The candidate boundary
  // must bind site/status to the actual failed adapter instead.
  const EncoderGpuFailure provider{uint32_t(EncoderGpuFailureSite::nvencOutputLock),UINT32_MAX,0x8086,0xdead};
  const Failure original(encoderGpuOutputContract,"injected private operation",provider);
  require(failureCode(original)==encoderGpuOutputContract&&failureInfo(original)==provider,"Fixed fallback code or unsigned native status changed while extracting evidence");
  require(failureInfo(std::runtime_error("untrusted exception text"))==EncoderGpuFailure{},"Generic exception text became private evidence");
  std::vector<uint64_t> opened;
  bool caught=false;
  try {
    createFromAdapters(adapters,{Mode::automatic,{}},[&](const Adapter& a)->std::unique_ptr<EncoderImplementation>{opened.push_back(a.luid);throw original;});
  } catch(const Failure& error) {
    const auto detail=error.gpuFailure();
    caught=error.code()==encoderGpuOutputContract&&detail.site==provider.site&&detail.nativeStatus==UINT32_MAX&&detail.vendorId==adapters[1].vendorId&&detail.deviceId==adapters[1].deviceId;
  }
  require(caught&&opened==std::vector<uint64_t>{7,9},"Initialization retry lost native evidence or attributed it to a rejected/forged adapter");
  const auto source=pixels(0);
  const auto failing = [&] {
    return createVerifiedFromAdapters(adapters,{Mode::automatic,{}},{7,{}},[&](const Adapter& a){
      auto detail=provider;detail.nativeStatus=a.luid==7?17:UINT32_MAX;
      return std::make_unique<InjectedGpu>(0,false,a,false,detail,encoderGpuOutputContract);
    });
  };
  auto candidates=failing();std::vector<Frame> frames;caught=false;
  try{candidates->encode(source.data(),12345,frames);}catch(const Failure& error){caught=error.code()==encoderGpuOutputContract&&error.gpuFailure()==EncoderGpuFailure{provider.site,UINT32_MAX,adapters[1].vendorId,adapters[1].deviceId};}
  require(caught&&frames.empty(),"First-output retries dropped final candidate details or published rejected bytes");
  require(!candidates->hardwareInfo().initialized,"Exhausted candidates retained initialized hardware metadata");

  const std::vector<Adapter> nvidia{{7,0x10de,0xa7,"First NVIDIA injected candidate"},{9,0x10de,0xb9,"Fenced NVIDIA injected candidate"}};
  const EncoderGpuFailure cause{uint32_t(EncoderGpuFailureSite::nvencOutputAnnexB),0,0,0};
  auto fenced=createVerifiedFromAdapters(nvidia,{Mode::automatic,{}},{7,{}},[&](const Adapter& a)->std::unique_ptr<EncoderImplementation>{
    if(a.luid==9)throw Failure(encoderGpuDeviceLost,"injected process EOS admission fence",failureDetail(EncoderGpuFailureSite::nvencEndOfStream));
    return std::make_unique<InjectedGpu>(0,false,a,false,cause,encoderGpuOutputContract);
  });
  frames.clear();caught=false;
  try{fenced->encode(source.data(),12345,frames);}catch(const Failure& error){caught=error.code()==encoderGpuOutputContract&&error.gpuFailure()==EncoderGpuFailure{cause.site,0,nvidia[0].vendorId,nvidia[0].deviceId};}
  require(caught&&frames.empty(),"Consequential NVENC EOS admission fence replaced the original failed candidate output evidence");

  Encoder fallback(width,height,fps,bitrate,{},EncoderBackend::x264);
  EncoderGpuTestAccess::install(fallback,failing());
  std::vector<EncoderOutputObservation> observations;
  fallback.setOutputObserver([&](const auto& value){observations.push_back(value);});
  require(observations.empty(),"Failed GPU initialization was reported before an actual CPU output");
  size_t outputs=0;
  fallback.encode(source.data(),12345,[&](Frame frame){require(frame.ptsUs==12345&&frame.key,"Detailed fallback lost the failed input PTS/IDR");++outputs;});
  const EncoderGpuFailure expected{provider.site,UINT32_MAX,adapters[1].vendorId,adapters[1].deviceId};
  const auto cpu=fallback.hardwareInfo();
  require(outputs==1&&fallback.backend()==EncoderBackend::x264&&cpu.gpuFallbackCode==encoderGpuOutputContract&&fallback.fallbackCode()==0,"Evidence changed the existing CPU fallback behavior");
  require(cpu.gpuFailure==expected&&cpu.vendorId==0&&cpu.deviceId==0&&!cpu.hardwareVerified,"CPU output lost failed-adapter evidence or claimed current hardware identity");
  require(observations.size()==1&&observations[0].hardware.gpuFailure==expected&&std::string(observations[0].backend)=="x264","First actual CPU output did not carry the preserved failure");
  const auto actualCpuInfo=encoderDiagnostic("x264",observations[0].hardware,observations[0].softwareFallbackCode);
  const auto privateBytes=encoderUsagePacket({"12345678-1234-4234-8234-123456789abc",0,{"0",1,actualCpuInfo}});
  const auto actualPrivate=parseEncoderUsagePacket(JsonParser::parse(privateBytes));
  require(actualPrivate&&actualPrivate->track.info.gpuFailure==expected,"Actual CPU fallback output lost unsigned native evidence in the production private JSON round trip");

  auto runtime=createVerifiedFromAdapters(adapters,{Mode::automatic,9},{},[&](const Adapter& a){return std::make_unique<InjectedGpu>(1,false,a,false,provider,encoderGpuOutputContract);});
  Encoder encoder(width,height,fps,bitrate,{},EncoderBackend::x264);EncoderGpuTestAccess::install(encoder,std::move(runtime));
  observations.clear();encoder.setOutputObserver([&](const auto& value){observations.push_back(value);});
  encoder.encode(source.data(),0,[](Frame){});
  require(observations.size()==1&&observations[0].hardware.hardwareVerified&&observations[0].hardware.gpuFailure==EncoderGpuFailure{},"Successful first GPU output inherited unrelated failure details");
  encoder.encode(source.data(),16667,[](Frame){});
  require(observations.size()==2&&observations[1].hardware.gpuFailure==expected&&observations[0].hardware.gpuFailure==EncoderGpuFailure{},"Runtime failure was lost or retroactively changed the earlier output snapshot");

  auto corrupt=createVerifiedFromAdapters(adapters,{Mode::automatic,9},{},[&](const Adapter& a){return std::make_unique<InjectedGpu>(-1,false,a,true);});
  frames.clear();caught=false;
  try{corrupt->encode(source.data(),23456,frames);}catch(const Failure& error){const auto detail=error.gpuFailure();caught=error.code()==encoderGpuOutputContract&&detail.site==uint32_t(EncoderGpuFailureSite::outputAnnexB)&&detail.nativeStatus==0&&detail.vendorId==adapters[1].vendorId&&detail.deviceId==adapters[1].deviceId;}
  require(caught&&frames.empty(),"Common malformed-AnnexB rejection lost its exact site or was weakened");

  auto rate=createVerifiedFromAdapters(adapters,{Mode::automatic,9},{},[&](const Adapter& a){return std::make_unique<InjectedGpu>(-1,true,a,false,provider);});
  Encoder reconfigure(width,height,fps,bitrate,{},EncoderBackend::x264);EncoderGpuTestAccess::install(reconfigure,std::move(rate));
  observations.clear();reconfigure.setOutputObserver([&](const auto& value){observations.push_back(value);});
  reconfigure.encode(source.data(),0,[](Frame){});
  require(reconfigure.setBitrate(8000000)&&observations.size()==1,"Reconfiguration initialization was reported without CPU output");
  reconfigure.encode(source.data(),16667,[](Frame){});
  require(observations.size()==2&&reconfigure.hardwareInfo().gpuFallbackCode==encoderGpuReconfigureFailed&&observations[1].hardware.gpuFailure==expected,"Native reconfiguration status was lost across CPU fallback");
}
}
int main() {
  try {
    host::Runtime runtime; candidatePolicy(); interoperability(false); interoperability(true); callerFailure(); firstOutputSelection(); actualOutputObservation(); failureEvidence();
    std::cout << "{\"kind\":\"gpu-h264-fallback\",\"checks\":" << checks << ",\"actualGpu\":false,\"gpuFaultInjected\":true,\"actualCpuEncoder\":true,\"actualClientDecoder\":true,\"capture\":false,\"relay\":false,\"passed\":true}\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
